// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2018 Intel Corporation. */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/bpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/ethernet.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <locale.h>
#include <sys/types.h>
#include <poll.h>

#include "bpf_load.h"
#include "bpf_util.h"
#include <bpf/bpf.h>

#include "xdpsock.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef PF_XDP
#define PF_XDP AF_XDP
#endif

#define NUM_FRAMES 131072
#define FRAME_HEADROOM 0
#define FRAME_SIZE 2048
#define NUM_DESCS 1024
#define BATCH_SIZE 16

#define FQ_NUM_DESCS 1024
#define CQ_NUM_DESCS 1024

#define DEBUG_HEXDUMP 0

typedef __u32 u32;

static unsigned long prev_time;

enum benchmark_type {
	BENCH_RXDROP = 0,
	BENCH_TXONLY = 1,
	BENCH_L2FWD = 2,
};

static enum benchmark_type opt_bench = BENCH_RXDROP;
static u32 opt_xdp_flags;
static const char *opt_if = "";
static int opt_ifindex;
static int opt_queue;
static int opt_poll;
static int opt_shared_packet_buffer;
static int opt_interval = 1;

struct xdp_umem_uqueue {
	u32 cached_prod;
	u32 cached_cons;
	u32 mask;
	u32 size;
	struct xdp_umem_ring *ring;
};

struct xdp_umem {
	char (*frames)[FRAME_SIZE];
	struct xdp_umem_uqueue fq;
	struct xdp_umem_uqueue cq;
	int fd;
};

struct xdp_uqueue {
	u32 cached_prod;
	u32 cached_cons;
	u32 mask;
	u32 size;
	struct xdp_rxtx_ring *ring;
};

struct xdpsock {
	struct xdp_uqueue rx;
	struct xdp_uqueue tx;
	int sfd;
	struct xdp_umem *umem;
	u32 outstanding_tx;
	unsigned long rx_npkts;
	unsigned long tx_npkts;
	unsigned long prev_rx_npkts;
	unsigned long prev_tx_npkts;
};

#define MAX_SOCKS 4
static int num_socks;
struct xdpsock *xsks[MAX_SOCKS];

static unsigned long get_nsecs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

static void dump_stats(void);

#define lassert(expr)							\
	do {								\
		if (!(expr)) {						\
			fprintf(stderr, "%s:%s:%i: Assertion failed: "	\
				#expr ": errno: %d/\"%s\"\n",		\
				__FILE__, __func__, __LINE__,		\
				errno, strerror(errno));		\
			dump_stats();					\
			exit(EXIT_FAILURE);				\
		}							\
	} while (0)

#define barrier() __asm__ __volatile__("": : :"memory")
#define u_smp_rmb() barrier()
#define u_smp_wmb() barrier()
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

static const char pkt_data[] =
	"\x3c\xfd\xfe\x9e\x7f\x71\xec\xb1\xd7\x98\x3a\xc0\x08\x00\x45\x00"
	"\x00\x2e\x00\x00\x00\x00\x40\x11\x88\x97\x05\x08\x07\x08\xc8\x14"
	"\x1e\x04\x10\x92\x10\x92\x00\x1a\x6d\xa3\x34\x33\x1f\x69\x40\x6b"
	"\x54\x59\xb6\x14\x2d\x11\x44\xbf\xaf\xd9\xbe\xaa";

static inline u32 umem_nb_free(struct xdp_umem_uqueue *q, u32 nb)
{
	u32 free_entries = q->size - (q->cached_prod - q->cached_cons);

	if (free_entries >= nb)
		return free_entries;

	/* Refresh the local tail pointer */
	q->cached_cons = q->ring->ptrs.consumer;

	return q->size - (q->cached_prod - q->cached_cons);
}

static inline u32 xq_nb_free(struct xdp_uqueue *q, u32 ndescs)
{
	u32 free_entries = q->cached_cons - q->cached_prod;

	if (free_entries >= ndescs)
		return free_entries;

	/* Refresh the local tail pointer */
	q->cached_cons = q->ring->ptrs.consumer + q->size;
	return q->cached_cons - q->cached_prod;
}

static inline u32 umem_nb_avail(struct xdp_umem_uqueue *q, u32 nb)
{
	u32 entries = q->cached_prod - q->cached_cons;

	if (entries == 0) {
		q->cached_prod = q->ring->ptrs.producer;
		entries = q->cached_prod - q->cached_cons;
	}

	return (entries > nb) ? nb : entries;
}

static inline u32 xq_nb_avail(struct xdp_uqueue *q, u32 ndescs)
{
	u32 entries = q->cached_prod - q->cached_cons;

	if (entries == 0) {
		q->cached_prod = q->ring->ptrs.producer;
		entries = q->cached_prod - q->cached_cons;
	}

	return (entries > ndescs) ? ndescs : entries;
}

static inline int umem_fill_to_kernel_ex(struct xdp_umem_uqueue *fq,
					 struct xdp_desc *d,
					 size_t nb)
{
	u32 i;

	if (umem_nb_free(fq, nb) < nb)
		return -ENOSPC;

	for (i = 0; i < nb; i++) {
		u32 idx = fq->cached_prod++ & fq->mask;

		fq->ring->desc[idx] = d[i].idx;
	}

	u_smp_wmb();

	fq->ring->ptrs.producer = fq->cached_prod;

	return 0;
}

static inline int umem_fill_to_kernel(struct xdp_umem_uqueue *fq, u32 *d,
				      size_t nb)
{
	u32 i;

	if (umem_nb_free(fq, nb) < nb)
		return -ENOSPC;

	for (i = 0; i < nb; i++) {
		u32 idx = fq->cached_prod++ & fq->mask;

		fq->ring->desc[idx] = d[i];
	}

	u_smp_wmb();

	fq->ring->ptrs.producer = fq->cached_prod;

	return 0;
}

static inline size_t umem_complete_from_kernel(struct xdp_umem_uqueue *cq,
					       u32 *d, size_t nb)
{
	u32 idx, i, entries = umem_nb_avail(cq, nb);

	u_smp_rmb();

	for (i = 0; i < entries; i++) {
		idx = cq->cached_cons++ & cq->mask;
		d[i] = cq->ring->desc[idx];
	}

	if (entries > 0) {
		u_smp_wmb();

		cq->ring->ptrs.consumer = cq->cached_cons;
	}

	return entries;
}

static inline void *xq_get_data(struct xdpsock *xsk, __u32 idx, __u32 off)
{
	lassert(idx < NUM_FRAMES);
	return &xsk->umem->frames[idx][off];
}

static inline int xq_enq(struct xdp_uqueue *uq,
			 const struct xdp_desc *descs,
			 unsigned int ndescs)
{
	struct xdp_rxtx_ring *r = uq->ring;
	unsigned int i;

	if (xq_nb_free(uq, ndescs) < ndescs)
		return -ENOSPC;

	for (i = 0; i < ndescs; i++) {
		u32 idx = uq->cached_prod++ & uq->mask;

		r->desc[idx].idx = descs[i].idx;
		r->desc[idx].len = descs[i].len;
		r->desc[idx].offset = descs[i].offset;
	}

	u_smp_wmb();

	r->ptrs.producer = uq->cached_prod;
	return 0;
}

static inline int xq_enq_tx_only(struct xdp_uqueue *uq,
				 __u32 idx, unsigned int ndescs)
{
	struct xdp_rxtx_ring *q = uq->ring;
	unsigned int i;

	if (xq_nb_free(uq, ndescs) < ndescs)
		return -ENOSPC;

	for (i = 0; i < ndescs; i++) {
		u32 idx = uq->cached_prod++ & uq->mask;

		q->desc[idx].idx	= idx + i;
		q->desc[idx].len	= sizeof(pkt_data) - 1;
		q->desc[idx].offset	= 0;
	}

	u_smp_wmb();

	q->ptrs.producer = uq->cached_prod;
	return 0;
}

static inline int xq_deq(struct xdp_uqueue *uq,
			 struct xdp_desc *descs,
			 int ndescs)
{
	struct xdp_rxtx_ring *r = uq->ring;
	unsigned int idx;
	int i, entries;

	entries = xq_nb_avail(uq, ndescs);

	u_smp_rmb();

	for (i = 0; i < entries; i++) {
		idx = uq->cached_cons++ & uq->mask;
		descs[i] = r->desc[idx];
	}

	if (entries > 0) {
		u_smp_wmb();

		r->ptrs.consumer = uq->cached_cons;
	}

	return entries;
}

static void swap_mac_addresses(void *data)
{
	struct ether_header *eth = (struct ether_header *)data;
	struct ether_addr *src_addr = (struct ether_addr *)&eth->ether_shost;
	struct ether_addr *dst_addr = (struct ether_addr *)&eth->ether_dhost;
	struct ether_addr tmp;

	tmp = *src_addr;
	*src_addr = *dst_addr;
	*dst_addr = tmp;
}

#if DEBUG_HEXDUMP
static void hex_dump(void *pkt, size_t length, const char *prefix)
{
	int i = 0;
	const unsigned char *address = (unsigned char *)pkt;
	const unsigned char *line = address;
	size_t line_size = 32;
	unsigned char c;

	printf("length = %zu\n", length);
	printf("%s | ", prefix);
	while (length-- > 0) {
		printf("%02X ", *address++);
		if (!(++i % line_size) || (length == 0 && i % line_size)) {
			if (length == 0) {
				while (i++ % line_size)
					printf("__ ");
			}
			printf(" | ");	/* right close */
			while (line < address) {
				c = *line++;
				printf("%c", (c < 33 || c == 255) ? 0x2E : c);
			}
			printf("\n");
			if (length > 0)
				printf("%s | ", prefix);
		}
	}
	printf("\n");
}
#endif

static size_t gen_eth_frame(char *frame)
{
	memcpy(frame, pkt_data, sizeof(pkt_data) - 1);
	return sizeof(pkt_data) - 1;
}

static struct xdp_umem *xdp_umem_configure(int sfd)
{
	int fq_size = FQ_NUM_DESCS, cq_size = CQ_NUM_DESCS;
	struct xdp_umem_reg mr;
	struct xdp_umem *umem;
	void *bufs;

	umem = calloc(1, sizeof(*umem));
	lassert(umem);

	lassert(posix_memalign(&bufs, getpagesize(), /* PAGE_SIZE aligned */
			       NUM_FRAMES * FRAME_SIZE) == 0);

	mr.addr = (__u64)bufs;
	mr.len = NUM_FRAMES * FRAME_SIZE;
	mr.frame_size = FRAME_SIZE;
	mr.frame_headroom = FRAME_HEADROOM;

	lassert(setsockopt(sfd, SOL_XDP, XDP_UMEM_REG, &mr, sizeof(mr)) == 0);
	lassert(setsockopt(sfd, SOL_XDP, XDP_UMEM_FILL_RING, &fq_size,
			   sizeof(int)) == 0);
	lassert(setsockopt(sfd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &cq_size,
			   sizeof(int)) == 0);

	umem->fq.ring = mmap(0, sizeof(struct xdp_umem_ring) +
			     FQ_NUM_DESCS * sizeof(u32),
			     PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_POPULATE, sfd,
			     XDP_UMEM_PGOFF_FILL_RING);
	lassert(umem->fq.ring != MAP_FAILED);

	umem->fq.mask = FQ_NUM_DESCS - 1;
	umem->fq.size = FQ_NUM_DESCS;

	umem->cq.ring = mmap(0, sizeof(struct xdp_umem_ring) +
			     CQ_NUM_DESCS * sizeof(u32),
			     PROT_READ | PROT_WRITE,
			     MAP_SHARED | MAP_POPULATE, sfd,
			     XDP_UMEM_PGOFF_COMPLETION_RING);
	lassert(umem->cq.ring != MAP_FAILED);

	umem->cq.mask = CQ_NUM_DESCS - 1;
	umem->cq.size = CQ_NUM_DESCS;

	umem->frames = (char (*)[FRAME_SIZE])bufs;
	umem->fd = sfd;

	if (opt_bench == BENCH_TXONLY) {
		int i;

		for (i = 0; i < NUM_FRAMES; i++)
			(void)gen_eth_frame(&umem->frames[i][0]);
	}

	return umem;
}

static struct xdpsock *xsk_configure(struct xdp_umem *umem)
{
	struct sockaddr_xdp sxdp = {};
	int sfd, ndescs = NUM_DESCS;
	struct xdpsock *xsk;
	bool shared = true;
	u32 i;

	sfd = socket(PF_XDP, SOCK_RAW, 0);
	lassert(sfd >= 0);

	xsk = calloc(1, sizeof(*xsk));
	lassert(xsk);

	xsk->sfd = sfd;
	xsk->outstanding_tx = 0;

	if (!umem) {
		shared = false;
		xsk->umem = xdp_umem_configure(sfd);
	} else {
		xsk->umem = umem;
	}

	lassert(setsockopt(sfd, SOL_XDP, XDP_RX_RING,
			   &ndescs, sizeof(int)) == 0);
	lassert(setsockopt(sfd, SOL_XDP, XDP_TX_RING,
			   &ndescs, sizeof(int)) == 0);

	/* Rx */
	xsk->rx.ring = mmap(NULL,
			    sizeof(struct xdp_ring) +
			    NUM_DESCS * sizeof(struct xdp_desc),
			    PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_POPULATE, sfd,
			    XDP_PGOFF_RX_RING);
	lassert(xsk->rx.ring != MAP_FAILED);

	if (!shared) {
		for (i = 0; i < NUM_DESCS / 2; i++)
			lassert(umem_fill_to_kernel(&xsk->umem->fq, &i, 1)
				== 0);
	}

	/* Tx */
	xsk->tx.ring = mmap(NULL,
			 sizeof(struct xdp_ring) +
			 NUM_DESCS * sizeof(struct xdp_desc),
			 PROT_READ | PROT_WRITE,
			 MAP_SHARED | MAP_POPULATE, sfd,
			 XDP_PGOFF_TX_RING);
	lassert(xsk->tx.ring != MAP_FAILED);

	xsk->rx.mask = NUM_DESCS - 1;
	xsk->rx.size = NUM_DESCS;

	xsk->tx.mask = NUM_DESCS - 1;
	xsk->tx.size = NUM_DESCS;

	sxdp.sxdp_family = PF_XDP;
	sxdp.sxdp_ifindex = opt_ifindex;
	sxdp.sxdp_queue_id = opt_queue;
	if (shared) {
		sxdp.sxdp_flags = XDP_SHARED_UMEM;
		sxdp.sxdp_shared_umem_fd = umem->fd;
	}

	lassert(bind(sfd, (struct sockaddr *)&sxdp, sizeof(sxdp)) == 0);

	return xsk;
}

static void print_benchmark(bool running)
{
	const char *bench_str = "INVALID";

	if (opt_bench == BENCH_RXDROP)
		bench_str = "rxdrop";
	else if (opt_bench == BENCH_TXONLY)
		bench_str = "txonly";
	else if (opt_bench == BENCH_L2FWD)
		bench_str = "l2fwd";

	printf("%s:%d %s ", opt_if, opt_queue, bench_str);
	if (opt_xdp_flags & XDP_FLAGS_SKB_MODE)
		printf("xdp-skb ");
	else if (opt_xdp_flags & XDP_FLAGS_DRV_MODE)
		printf("xdp-drv ");
	else
		printf("	");

	if (opt_poll)
		printf("poll() ");

	if (running) {
		printf("running...");
		fflush(stdout);
	}
}

static void dump_stats(void)
{
	unsigned long now = get_nsecs();
	long dt = now - prev_time;
	int i;

	prev_time = now;

	for (i = 0; i < num_socks; i++) {
		char *fmt = "%-15s %'-11.0f %'-11lu\n";
		double rx_pps, tx_pps;

		rx_pps = (xsks[i]->rx_npkts - xsks[i]->prev_rx_npkts) *
			 1000000000. / dt;
		tx_pps = (xsks[i]->tx_npkts - xsks[i]->prev_tx_npkts) *
			 1000000000. / dt;

		printf("\n sock%d@", i);
		print_benchmark(false);
		printf("\n");

		printf("%-15s %-11s %-11s %-11.2f\n", "", "pps", "pkts",
		       dt / 1000000000.);
		printf(fmt, "rx", rx_pps, xsks[i]->rx_npkts);
		printf(fmt, "tx", tx_pps, xsks[i]->tx_npkts);

		xsks[i]->prev_rx_npkts = xsks[i]->rx_npkts;
		xsks[i]->prev_tx_npkts = xsks[i]->tx_npkts;
	}
}

static void *poller(void *arg)
{
	(void)arg;
	for (;;) {
		sleep(opt_interval);
		dump_stats();
	}

	return NULL;
}

static void int_exit(int sig)
{
	(void)sig;
	dump_stats();
	bpf_set_link_xdp_fd(opt_ifindex, -1, opt_xdp_flags);
	exit(EXIT_SUCCESS);
}

static struct option long_options[] = {
	{"rxdrop", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"l2fwd", no_argument, 0, 'l'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"poll", no_argument, 0, 'p'},
	{"shared-buffer", no_argument, 0, 's'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
	{"interval", required_argument, 0, 'n'},
	{0, 0, 0, 0}
};

static void usage(const char *prog)
{
	const char *str =
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -r, --rxdrop		Discard all incoming packets (default)\n"
		"  -t, --txonly		Only send packets\n"
		"  -l, --l2fwd		MAC swap L2 forwarding\n"
		"  -i, --interface=n	Run on interface n\n"
		"  -q, --queue=n	Use queue n (default 0)\n"
		"  -p, --poll		Use poll syscall\n"
		"  -s, --shared-buffer	Use shared packet buffer\n"
		"  -S, --xdp-skb=n	Use XDP skb-mod\n"
		"  -N, --xdp-native=n	Enfore XDP native mode\n"
		"  -n, --interval=n	Specify statistics update interval (default 1 sec).\n"
		"\n";
	fprintf(stderr, str, prog);
	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, "rtli:q:psSNn:", long_options,
				&option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'r':
			opt_bench = BENCH_RXDROP;
			break;
		case 't':
			opt_bench = BENCH_TXONLY;
			break;
		case 'l':
			opt_bench = BENCH_L2FWD;
			break;
		case 'i':
			opt_if = optarg;
			break;
		case 'q':
			opt_queue = atoi(optarg);
			break;
		case 's':
			opt_shared_packet_buffer = 1;
			break;
		case 'p':
			opt_poll = 1;
			break;
		case 'S':
			opt_xdp_flags |= XDP_FLAGS_SKB_MODE;
			break;
		case 'N':
			opt_xdp_flags |= XDP_FLAGS_DRV_MODE;
			break;
		case 'n':
			opt_interval = atoi(optarg);
			break;
		default:
			usage(basename(argv[0]));
		}
	}

	opt_ifindex = if_nametoindex(opt_if);
	if (!opt_ifindex) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
			opt_if);
		usage(basename(argv[0]));
	}
}

static void kick_tx(int fd)
{
	int ret;

	ret = sendto(fd, NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN)
		return;
	lassert(0);
}

static inline void complete_tx_l2fwd(struct xdpsock *xsk)
{
	u32 descs[BATCH_SIZE];
	unsigned int rcvd;
	size_t ndescs;

	if (!xsk->outstanding_tx)
		return;

	kick_tx(xsk->sfd);
	ndescs = (xsk->outstanding_tx > BATCH_SIZE) ? BATCH_SIZE :
		 xsk->outstanding_tx;

	/* re-add completed Tx buffers */
	rcvd = umem_complete_from_kernel(&xsk->umem->cq, descs, ndescs);
	if (rcvd > 0) {
		umem_fill_to_kernel(&xsk->umem->fq, descs, rcvd);
		xsk->outstanding_tx -= rcvd;
		xsk->tx_npkts += rcvd;
	}
}

static inline void complete_tx_only(struct xdpsock *xsk)
{
	u32 descs[BATCH_SIZE];
	unsigned int rcvd;

	if (!xsk->outstanding_tx)
		return;

	kick_tx(xsk->sfd);

	rcvd = umem_complete_from_kernel(&xsk->umem->cq, descs, BATCH_SIZE);
	if (rcvd > 0) {
		xsk->outstanding_tx -= rcvd;
		xsk->tx_npkts += rcvd;
	}
}

static void rx_drop(struct xdpsock *xsk)
{
	struct xdp_desc descs[BATCH_SIZE];
	unsigned int rcvd, i;

	rcvd = xq_deq(&xsk->rx, descs, BATCH_SIZE);
	if (!rcvd)
		return;

	for (i = 0; i < rcvd; i++) {
		u32 idx = descs[i].idx;

		lassert(idx < NUM_FRAMES);
#if DEBUG_HEXDUMP
		char *pkt;
		char buf[32];

		pkt = xq_get_data(xsk, idx, descs[i].offset);
		sprintf(buf, "idx=%d", idx);
		hex_dump(pkt, descs[i].len, buf);
#endif
	}

	xsk->rx_npkts += rcvd;

	umem_fill_to_kernel_ex(&xsk->umem->fq, descs, rcvd);
}

static void rx_drop_all(void)
{
	struct pollfd fds[MAX_SOCKS + 1];
	int i, ret, timeout, nfds = 1;

	memset(fds, 0, sizeof(fds));

	for (i = 0; i < num_socks; i++) {
		fds[i].fd = xsks[i]->sfd;
		fds[i].events = POLLIN;
		timeout = 1000; /* 1sn */
	}

	for (;;) {
		if (opt_poll) {
			ret = poll(fds, nfds, timeout);
			if (ret <= 0)
				continue;
		}

		for (i = 0; i < num_socks; i++)
			rx_drop(xsks[i]);
	}
}

static void tx_only(struct xdpsock *xsk)
{
	int timeout, ret, nfds = 1;
	struct pollfd fds[nfds + 1];
	unsigned int idx = 0;

	memset(fds, 0, sizeof(fds));
	fds[0].fd = xsk->sfd;
	fds[0].events = POLLOUT;
	timeout = 1000; /* 1sn */

	for (;;) {
		if (opt_poll) {
			ret = poll(fds, nfds, timeout);
			if (ret <= 0)
				continue;

			if (fds[0].fd != xsk->sfd ||
			    !(fds[0].revents & POLLOUT))
				continue;
		}

		if (xq_nb_free(&xsk->tx, BATCH_SIZE) >= BATCH_SIZE) {
			lassert(xq_enq_tx_only(&xsk->tx, idx, BATCH_SIZE) == 0);

			xsk->outstanding_tx += BATCH_SIZE;
			idx += BATCH_SIZE;
			idx %= NUM_FRAMES;
		}

		complete_tx_only(xsk);
	}
}

static void l2fwd(struct xdpsock *xsk)
{
	for (;;) {
		struct xdp_desc descs[BATCH_SIZE];
		unsigned int rcvd, i;
		int ret;

		for (;;) {
			complete_tx_l2fwd(xsk);

			rcvd = xq_deq(&xsk->rx, descs, BATCH_SIZE);
			if (rcvd > 0)
				break;
		}

		for (i = 0; i < rcvd; i++) {
			char *pkt = xq_get_data(xsk, descs[i].idx,
						descs[i].offset);

			swap_mac_addresses(pkt);
#if DEBUG_HEXDUMP
			char buf[32];
			u32 idx = descs[i].idx;

			sprintf(buf, "idx=%d", idx);
			hex_dump(pkt, descs[i].len, buf);
#endif
		}

		xsk->rx_npkts += rcvd;

		ret = xq_enq(&xsk->tx, descs, rcvd);
		lassert(ret == 0);
		xsk->outstanding_tx += rcvd;
	}
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	char xdp_filename[256];
	int i, ret, key = 0;
	pthread_t pt;

	parse_command_line(argc, argv);

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		fprintf(stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	snprintf(xdp_filename, sizeof(xdp_filename), "%s_kern.o", argv[0]);

	if (load_bpf_file(xdp_filename)) {
		fprintf(stderr, "ERROR: load_bpf_file %s\n", bpf_log_buf);
		exit(EXIT_FAILURE);
	}

	if (!prog_fd[0]) {
		fprintf(stderr, "ERROR: load_bpf_file: \"%s\"\n",
			strerror(errno));
		exit(EXIT_FAILURE);
	}

	if (bpf_set_link_xdp_fd(opt_ifindex, prog_fd[0], opt_xdp_flags) < 0) {
		fprintf(stderr, "ERROR: link set xdp fd failed\n");
		exit(EXIT_FAILURE);
	}

	ret = bpf_map_update_elem(map_fd[0], &key, &opt_queue, 0);
	if (ret) {
		fprintf(stderr, "ERROR: bpf_map_update_elem qidconf\n");
		exit(EXIT_FAILURE);
	}

	/* Create sockets... */
	xsks[num_socks++] = xsk_configure(NULL);

#if RR_LB
	for (i = 0; i < MAX_SOCKS - 1; i++)
		xsks[num_socks++] = xsk_configure(xsks[0]->umem);
#endif

	/* ...and insert them into the map. */
	for (i = 0; i < num_socks; i++) {
		key = i;
		ret = bpf_map_update_elem(map_fd[1], &key, &xsks[i]->sfd, 0);
		if (ret) {
			fprintf(stderr, "ERROR: bpf_map_update_elem %d\n", i);
			exit(EXIT_FAILURE);
		}
	}

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);
	signal(SIGABRT, int_exit);

	setlocale(LC_ALL, "");

	ret = pthread_create(&pt, NULL, poller, NULL);
	lassert(ret == 0);

	prev_time = get_nsecs();

	if (opt_bench == BENCH_RXDROP)
		rx_drop_all();
	else if (opt_bench == BENCH_TXONLY)
		tx_only(xsks[0]);
	else
		l2fwd(xsks[0]);

	return 0;
}
