// SPDX-License-Identifier: GPL-2.0
/* XDP sockets
 *
 * AF_XDP sockets allows a channel between XDP programs and userspace
 * applications.
 * Copyright(c) 2018 Intel Corporation.
 *
 * Author(s): Björn Töpel <bjorn.topel@intel.com>
 *	      Magnus Karlsson <magnus.karlsson@intel.com>
 */

#define pr_fmt(fmt) "AF_XDP: %s: " fmt, __func__

#include <linux/if_xdp.h>
#include <linux/init.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/socket.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <net/xdp_sock.h>
#include <net/xdp.h>

#include "xsk_queue.h"
#include "xdp_umem.h"

#define TX_BATCH_SIZE 16

static struct xdp_sock *xdp_sk(struct sock *sk)
{
	return (struct xdp_sock *)sk;
}

bool xsk_is_setup_for_bpf_map(struct xdp_sock *xs)
{
	return !!xs->rx;
}

static int __xsk_rcv(struct xdp_sock *xs, struct xdp_buff *xdp)
{
	u32 *id, len = xdp->data_end - xdp->data;
	void *buffer;
	int err = 0;

	if (xs->dev != xdp->rxq->dev || xs->queue_id != xdp->rxq->queue_index)
		return -EINVAL;

	id = xskq_peek_id(xs->umem->fq);
	if (!id)
		return -ENOSPC;

	buffer = xdp_umem_get_data_with_headroom(xs->umem, *id);
	memcpy(buffer, xdp->data, len);
	err = xskq_produce_batch_desc(xs->rx, *id, len,
				      xs->umem->frame_headroom);
	if (!err)
		xskq_discard_id(xs->umem->fq);

	return err;
}

int xsk_rcv(struct xdp_sock *xs, struct xdp_buff *xdp)
{
	int err;

	err = __xsk_rcv(xs, xdp);
	if (likely(!err))
		xdp_return_buff(xdp);
	else
		xs->rx_dropped++;

	return err;
}

void xsk_flush(struct xdp_sock *xs)
{
	xskq_produce_flush_desc(xs->rx);
	xs->sk.sk_data_ready(&xs->sk);
}

int xsk_generic_rcv(struct xdp_sock *xs, struct xdp_buff *xdp)
{
	int err;

	err = __xsk_rcv(xs, xdp);
	if (!err)
		xsk_flush(xs);
	else
		xs->rx_dropped++;

	return err;
}

static void xsk_destruct_skb(struct sk_buff *skb)
{
	u32 id = (u32)(long)skb_shinfo(skb)->destructor_arg;
	struct xdp_sock *xs = xdp_sk(skb->sk);

	WARN_ON_ONCE(xskq_produce_id(xs->umem->cq, id));

	sock_wfree(skb);
}

static int xsk_generic_xmit(struct sock *sk, struct msghdr *m,
			    size_t total_len)
{
	bool need_wait = !(m->msg_flags & MSG_DONTWAIT);
	u32 max_batch = TX_BATCH_SIZE;
	struct xdp_sock *xs = xdp_sk(sk);
	bool sent_frame = false;
	struct xdp_desc desc;
	struct sk_buff *skb;
	int err = 0;

	if (unlikely(!xs->tx))
		return -ENOBUFS;
	if (need_wait)
		return -EOPNOTSUPP;

	mutex_lock(&xs->mutex);

	while (xskq_peek_desc(xs->tx, &desc)) {
		char *buffer;
		u32 id, len;

		if (max_batch-- == 0) {
			err = -EAGAIN;
			goto out;
		}

		if (xskq_reserve_id(xs->umem->cq)) {
			err = -EAGAIN;
			goto out;
		}

		len = desc.len;
		if (unlikely(len > xs->dev->mtu)) {
			err = -EMSGSIZE;
			goto out;
		}

		skb = sock_alloc_send_skb(sk, len, !need_wait, &err);
		if (unlikely(!skb)) {
			err = -EAGAIN;
			goto out;
		}

		skb_put(skb, len);
		id = desc.idx;
		buffer = xdp_umem_get_data(xs->umem, id) + desc.offset;
		err = skb_store_bits(skb, 0, buffer, len);
		if (unlikely(err)) {
			kfree_skb(skb);
			goto out;
		}

		skb->dev = xs->dev;
		skb->priority = sk->sk_priority;
		skb->mark = sk->sk_mark;
		skb_shinfo(skb)->destructor_arg = (void *)(long)id;
		skb->destructor = xsk_destruct_skb;

		err = dev_direct_xmit(skb, xs->queue_id);
		/* Ignore NET_XMIT_CN as packet might have been sent */
		if (err == NET_XMIT_DROP || err == NETDEV_TX_BUSY) {
			err = -EAGAIN;
			/* SKB consumed by dev_direct_xmit() */
			goto out;
		}

		sent_frame = true;
		xskq_discard_desc(xs->tx);
	}

out:
	if (sent_frame)
		sk->sk_write_space(sk);

	mutex_unlock(&xs->mutex);
	return err;
}

static int xsk_sendmsg(struct socket *sock, struct msghdr *m, size_t total_len)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);

	if (unlikely(!xs->dev))
		return -ENXIO;
	if (unlikely(!(xs->dev->flags & IFF_UP)))
		return -ENETDOWN;

	return xsk_generic_xmit(sk, m, total_len);
}

static unsigned int xsk_poll(struct file *file, struct socket *sock,
			     struct poll_table_struct *wait)
{
	unsigned int mask = datagram_poll(file, sock, wait);
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);

	if (xs->rx && !xskq_empty_desc(xs->rx))
		mask |= POLLIN | POLLRDNORM;
	if (xs->tx && !xskq_full_desc(xs->tx))
		mask |= POLLOUT | POLLWRNORM;

	return mask;
}

static int xsk_init_queue(u32 entries, struct xsk_queue **queue,
			  bool umem_queue)
{
	struct xsk_queue *q;

	if (entries == 0 || *queue || !is_power_of_2(entries))
		return -EINVAL;

	q = xskq_create(entries, umem_queue);
	if (!q)
		return -ENOMEM;

	*queue = q;
	return 0;
}

static void __xsk_release(struct xdp_sock *xs)
{
	/* Wait for driver to stop using the xdp socket. */
	synchronize_net();

	dev_put(xs->dev);
}

static int xsk_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct net *net;

	if (!sk)
		return 0;

	net = sock_net(sk);

	local_bh_disable();
	sock_prot_inuse_add(net, sk->sk_prot, -1);
	local_bh_enable();

	if (xs->dev) {
		__xsk_release(xs);
		xs->dev = NULL;
	}

	sock_orphan(sk);
	sock->sk = NULL;

	sk_refcnt_debug_release(sk);
	sock_put(sk);

	return 0;
}

static struct socket *xsk_lookup_xsk_from_fd(int fd)
{
	struct socket *sock;
	int err;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		return ERR_PTR(-ENOTSOCK);

	if (sock->sk->sk_family != PF_XDP) {
		sockfd_put(sock);
		return ERR_PTR(-ENOPROTOOPT);
	}

	return sock;
}

static int xsk_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	struct sockaddr_xdp *sxdp = (struct sockaddr_xdp *)addr;
	struct sock *sk = sock->sk;
	struct net_device *dev, *dev_curr;
	struct xdp_sock *xs = xdp_sk(sk);
	struct xdp_umem *old_umem = NULL;
	int err = 0;

	if (addr_len < sizeof(struct sockaddr_xdp))
		return -EINVAL;
	if (sxdp->sxdp_family != AF_XDP)
		return -EINVAL;

	mutex_lock(&xs->mutex);
	dev_curr = xs->dev;
	dev = dev_get_by_index(sock_net(sk), sxdp->sxdp_ifindex);
	if (!dev) {
		err = -ENODEV;
		goto out_release;
	}

	if (!xs->rx && !xs->tx) {
		err = -EINVAL;
		goto out_unlock;
	}

	if (sxdp->sxdp_queue_id >= dev->num_rx_queues) {
		err = -EINVAL;
		goto out_unlock;
	}

	if (sxdp->sxdp_flags & XDP_SHARED_UMEM) {
		struct xdp_sock *umem_xs;
		struct socket *sock;

		if (xs->umem) {
			/* We have already our own. */
			err = -EINVAL;
			goto out_unlock;
		}

		sock = xsk_lookup_xsk_from_fd(sxdp->sxdp_shared_umem_fd);
		if (IS_ERR(sock)) {
			err = PTR_ERR(sock);
			goto out_unlock;
		}

		umem_xs = xdp_sk(sock->sk);
		if (!umem_xs->umem) {
			/* No umem to inherit. */
			err = -EBADF;
			sockfd_put(sock);
			goto out_unlock;
		} else if (umem_xs->dev != dev ||
			   umem_xs->queue_id != sxdp->sxdp_queue_id) {
			err = -EINVAL;
			sockfd_put(sock);
			goto out_unlock;
		}

		xdp_get_umem(umem_xs->umem);
		old_umem = xs->umem;
		xs->umem = umem_xs->umem;
		sockfd_put(sock);
	} else if (!xs->umem || !xdp_umem_validate_queues(xs->umem)) {
		err = -EINVAL;
		goto out_unlock;
	} else {
		/* This xsk has its own umem. */
		xskq_set_umem(xs->umem->fq, &xs->umem->props);
		xskq_set_umem(xs->umem->cq, &xs->umem->props);
	}

	/* Rebind? */
	if (dev_curr && (dev_curr != dev ||
			 xs->queue_id != sxdp->sxdp_queue_id)) {
		__xsk_release(xs);
		if (old_umem)
			xdp_put_umem(old_umem);
	}

	xs->dev = dev;
	xs->queue_id = sxdp->sxdp_queue_id;

	xskq_set_umem(xs->rx, &xs->umem->props);
	xskq_set_umem(xs->tx, &xs->umem->props);

out_unlock:
	if (err)
		dev_put(dev);
out_release:
	mutex_unlock(&xs->mutex);
	return err;
}

static int xsk_setsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, unsigned int optlen)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	int err;

	if (level != SOL_XDP)
		return -ENOPROTOOPT;

	switch (optname) {
	case XDP_RX_RING:
	case XDP_TX_RING:
	{
		struct xsk_queue **q;
		int entries;

		if (optlen < sizeof(entries))
			return -EINVAL;
		if (copy_from_user(&entries, optval, sizeof(entries)))
			return -EFAULT;

		mutex_lock(&xs->mutex);
		q = (optname == XDP_TX_RING) ? &xs->tx : &xs->rx;
		err = xsk_init_queue(entries, q, false);
		mutex_unlock(&xs->mutex);
		return err;
	}
	case XDP_UMEM_REG:
	{
		struct xdp_umem_reg mr;
		struct xdp_umem *umem;

		if (xs->umem)
			return -EBUSY;

		if (copy_from_user(&mr, optval, sizeof(mr)))
			return -EFAULT;

		mutex_lock(&xs->mutex);
		err = xdp_umem_create(&umem);

		err = xdp_umem_reg(umem, &mr);
		if (err) {
			kfree(umem);
			mutex_unlock(&xs->mutex);
			return err;
		}

		/* Make sure umem is ready before it can be seen by others */
		smp_wmb();

		xs->umem = umem;
		mutex_unlock(&xs->mutex);
		return 0;
	}
	case XDP_UMEM_FILL_RING:
	case XDP_UMEM_COMPLETION_RING:
	{
		struct xsk_queue **q;
		int entries;

		if (!xs->umem)
			return -EINVAL;

		if (copy_from_user(&entries, optval, sizeof(entries)))
			return -EFAULT;

		mutex_lock(&xs->mutex);
		q = (optname == XDP_UMEM_FILL_RING) ? &xs->umem->fq :
			&xs->umem->cq;
		err = xsk_init_queue(entries, q, true);
		mutex_unlock(&xs->mutex);
		return err;
	}
	default:
		break;
	}

	return -ENOPROTOOPT;
}

static int xsk_getsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	int len;

	if (level != SOL_XDP)
		return -ENOPROTOOPT;

	if (get_user(len, optlen))
		return -EFAULT;
	if (len < 0)
		return -EINVAL;

	switch (optname) {
	case XDP_STATISTICS:
	{
		struct xdp_statistics stats;

		if (len < sizeof(stats))
			return -EINVAL;

		mutex_lock(&xs->mutex);
		stats.rx_dropped = xs->rx_dropped;
		stats.rx_invalid_descs = xskq_nb_invalid_descs(xs->rx);
		stats.tx_invalid_descs = xskq_nb_invalid_descs(xs->tx);
		mutex_unlock(&xs->mutex);

		if (copy_to_user(optval, &stats, sizeof(stats)))
			return -EFAULT;
		if (put_user(sizeof(stats), optlen))
			return -EFAULT;

		return 0;
	}
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int xsk_mmap(struct file *file, struct socket *sock,
		    struct vm_area_struct *vma)
{
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct xdp_sock *xs = xdp_sk(sock->sk);
	struct xsk_queue *q = NULL;
	unsigned long pfn;
	struct page *qpg;

	if (offset == XDP_PGOFF_RX_RING) {
		q = xs->rx;
	} else if (offset == XDP_PGOFF_TX_RING) {
		q = xs->tx;
	} else {
		if (!xs->umem)
			return -EINVAL;

		if (offset == XDP_UMEM_PGOFF_FILL_RING)
			q = xs->umem->fq;
		else if (offset == XDP_UMEM_PGOFF_COMPLETION_RING)
			q = xs->umem->cq;
	}

	if (!q)
		return -EINVAL;

	qpg = virt_to_head_page(q->ring);
	if (size > (PAGE_SIZE << compound_order(qpg)))
		return -EINVAL;

	pfn = virt_to_phys(q->ring) >> PAGE_SHIFT;
	return remap_pfn_range(vma, vma->vm_start, pfn,
			       size, vma->vm_page_prot);
}

static struct proto xsk_proto = {
	.name =		"XDP",
	.owner =	THIS_MODULE,
	.obj_size =	sizeof(struct xdp_sock),
};

static const struct proto_ops xsk_proto_ops = {
	.family		= PF_XDP,
	.owner		= THIS_MODULE,
	.release	= xsk_release,
	.bind		= xsk_bind,
	.connect	= sock_no_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= sock_no_getname,
	.poll		= xsk_poll,
	.ioctl		= sock_no_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.setsockopt	= xsk_setsockopt,
	.getsockopt	= xsk_getsockopt,
	.sendmsg	= xsk_sendmsg,
	.recvmsg	= sock_no_recvmsg,
	.mmap		= xsk_mmap,
	.sendpage	= sock_no_sendpage,
};

static void xsk_destruct(struct sock *sk)
{
	struct xdp_sock *xs = xdp_sk(sk);

	if (!sock_flag(sk, SOCK_DEAD))
		return;

	xskq_destroy(xs->rx);
	xskq_destroy(xs->tx);
	xdp_put_umem(xs->umem);

	sk_refcnt_debug_dec(sk);
}

static int xsk_create(struct net *net, struct socket *sock, int protocol,
		      int kern)
{
	struct sock *sk;
	struct xdp_sock *xs;

	if (!ns_capable(net->user_ns, CAP_NET_RAW))
		return -EPERM;
	if (sock->type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;

	if (protocol)
		return -EPROTONOSUPPORT;

	sock->state = SS_UNCONNECTED;

	sk = sk_alloc(net, PF_XDP, GFP_KERNEL, &xsk_proto, kern);
	if (!sk)
		return -ENOBUFS;

	sock->ops = &xsk_proto_ops;

	sock_init_data(sock, sk);

	sk->sk_family = PF_XDP;

	sk->sk_destruct = xsk_destruct;
	sk_refcnt_debug_inc(sk);

	xs = xdp_sk(sk);
	mutex_init(&xs->mutex);

	local_bh_disable();
	sock_prot_inuse_add(net, &xsk_proto, 1);
	local_bh_enable();

	return 0;
}

static const struct net_proto_family xsk_family_ops = {
	.family = PF_XDP,
	.create = xsk_create,
	.owner	= THIS_MODULE,
};

static int __init xsk_init(void)
{
	int err;

	err = proto_register(&xsk_proto, 0 /* no slab */);
	if (err)
		goto out;

	err = sock_register(&xsk_family_ops);
	if (err)
		goto out_proto;

	return 0;

out_proto:
	proto_unregister(&xsk_proto);
out:
	return err;
}

fs_initcall(xsk_init);
