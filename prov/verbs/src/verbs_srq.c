/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include "fi_verbs.h"

static struct fi_ops_ep fi_ibv_srq_ep_base_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = fi_no_cancel,
	.getopt = fi_no_getopt,
	.setopt = fi_no_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

static struct fi_ops_cm fi_ibv_srq_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = fi_no_setname,
	.getname = fi_no_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_no_listen,
	.accept = fi_no_accept,
	.reject = fi_no_reject,
	.shutdown = fi_no_shutdown,
	.join = fi_no_join,
};

static struct fi_ops_rma fi_ibv_srq_rma_ops = {
	.size = sizeof(struct fi_ops_rma),
	.read = fi_no_rma_read,
	.readv = fi_no_rma_readv,
	.readmsg = fi_no_rma_readmsg,
	.write = fi_no_rma_write,
	.writev = fi_no_rma_writev,
	.writemsg = fi_no_rma_writemsg,
	.inject = fi_no_rma_inject,
	.writedata = fi_no_rma_writedata,
	.injectdata = fi_no_rma_injectdata,
};

static struct fi_ops_atomic fi_ibv_srq_atomic_ops = {
	.size = sizeof(struct fi_ops_atomic),
	.write = fi_no_atomic_write,
	.writev = fi_no_atomic_writev,
	.writemsg = fi_no_atomic_writemsg,
	.inject = fi_no_atomic_inject,
	.readwrite = fi_no_atomic_readwrite,
	.readwritev = fi_no_atomic_readwritev,
	.readwritemsg = fi_no_atomic_readwritemsg,
	.compwrite = fi_no_atomic_compwrite,
	.compwritev = fi_no_atomic_compwritev,
	.compwritemsg = fi_no_atomic_compwritemsg,
	.writevalid = fi_no_atomic_writevalid,
	.readwritevalid = fi_no_atomic_readwritevalid,
	.compwritevalid = fi_no_atomic_compwritevalid,
};

static ssize_t
fi_ibv_srq_ep_recvmsg(struct fid_ep *ep, const struct fi_msg *msg, uint64_t flags)
{
	struct fi_ibv_srq_ep *_ep;
	struct fi_ibv_wre *wre;
	struct ibv_sge *sge = NULL;
	size_t i;

	_ep = container_of(ep, struct fi_ibv_srq_ep, ep_fid);
	assert(_ep->srq);

	fastlock_acquire(&_ep->wre_lock);
	wre = util_buf_alloc(_ep->wre_pool);
	if (!wre) {
		fastlock_release(&_ep->wre_lock);
		return -FI_EAGAIN;
	}
	memset(wre, 0, sizeof(*wre));
	dlist_insert_tail(&wre->entry, &_ep->wre_list);
	fastlock_release(&_ep->wre_lock);

	wre->srq = _ep;
	wre->context = msg->context;

	wre->wr.type = IBV_RECV_WR;
	wre->wr.rwr.wr_id = (uintptr_t)wre;
	wre->wr.rwr.next = NULL;
	if (msg->iov_count) {
		sge = alloca(sizeof(*sge) * msg->iov_count);
		for (i = 0; i < msg->iov_count; i++) {
			sge[i].addr = (uintptr_t)msg->msg_iov[i].iov_base;
			sge[i].length = (uint32_t)msg->msg_iov[i].iov_len;
			sge[i].lkey = (uint32_t)(uintptr_t)(msg->desc[i]);
		}
	}
	wre->wr.rwr.sg_list = sge;
	wre->wr.rwr.num_sge = msg->iov_count;

	return FI_IBV_INVOKE_POST(srq_recv, recv, _ep->srq, &wre->wr.rwr,
				  FI_IBV_RELEASE_WRE(_ep, wre));
	
}

static ssize_t
fi_ibv_srq_ep_recv(struct fid_ep *ep, void *buf, size_t len,
		void *desc, fi_addr_t src_addr, void *context)
{
	struct iovec iov;
	struct fi_msg msg;

	iov.iov_base = buf;
	iov.iov_len = len;

	msg.msg_iov = &iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = src_addr;
	msg.context = context;

	return fi_ibv_srq_ep_recvmsg(ep, &msg, 0);
}

static ssize_t
fi_ibv_srq_ep_recvv(struct fid_ep *ep, const struct iovec *iov, void **desc,
                 size_t count, fi_addr_t src_addr, void *context)
{
	struct fi_msg msg;

	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = count;
	msg.addr = src_addr;
	msg.context = context;

	return fi_ibv_srq_ep_recvmsg(ep, &msg, 0);
}

static struct fi_ops_msg fi_ibv_srq_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = fi_ibv_srq_ep_recv,
	.recvv = fi_ibv_srq_ep_recvv,
	.recvmsg = fi_ibv_srq_ep_recvmsg,
	.send = fi_no_msg_send,
	.sendv = fi_no_msg_sendv,
	.sendmsg = fi_no_msg_sendmsg,
	.inject = fi_no_msg_inject,
	.senddata = fi_no_msg_senddata,
	.injectdata = fi_no_msg_injectdata,
};

int fi_ibv_srq_close(fid_t fid)
{
	struct fi_ibv_srq_ep *srq_ep;
	int ret;

	srq_ep = container_of(fid, struct fi_ibv_srq_ep, ep_fid.fid);
	ret = ibv_destroy_srq(srq_ep->srq);
	if (ret)
		VERBS_WARN(FI_LOG_EP_CTRL,
			   "Cannot destroy SRQ rc=%d\n", ret);

	/* All WCs from Receive CQ belongs to SRQ, no need to check EP. */
	/* Assumes that all EP that associated with the SRQ have
	 * already been closed (therefore, no more completions would
	 * arrive in CQ for the recv posted to SRQ) */
	/* Just to be clear, passes `IBV_RECV_WR`, because SRQ's WREs
	 * have `IBV_RECV_WR` type only */
	fi_ibv_empty_wre_list(srq_ep->wre_pool, &srq_ep->wre_list, IBV_RECV_WR);
	util_buf_pool_destroy(srq_ep->wre_pool);
	fastlock_destroy(&srq_ep->wre_lock);

	free(srq_ep);

	return FI_SUCCESS;
}

static struct fi_ops fi_ibv_srq_ep_ops = {
	.size = sizeof(struct fi_ops),
	.close = fi_ibv_srq_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};


int fi_ibv_srq_context(struct fid_domain *domain, struct fi_rx_attr *attr,
		       struct fid_ep **srq_ep_fid, void *context)
{
	struct ibv_srq_init_attr srq_init_attr = { 0 };
	struct fi_ibv_domain *dom;
	struct fi_ibv_srq_ep *srq_ep;
	int ret;

	if (!domain)
		return -FI_EINVAL;

	srq_ep = calloc(1, sizeof(*srq_ep));
	if (!srq_ep) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	dom = container_of(domain, struct fi_ibv_domain,
			   util_domain.domain_fid);

	srq_ep->ep_fid.fid.fclass = FI_CLASS_SRX_CTX;
	srq_ep->ep_fid.fid.context = context;
	srq_ep->ep_fid.fid.ops = &fi_ibv_srq_ep_ops;
	srq_ep->ep_fid.ops = &fi_ibv_srq_ep_base_ops;
	srq_ep->ep_fid.msg = &fi_ibv_srq_msg_ops;
	srq_ep->ep_fid.cm = &fi_ibv_srq_cm_ops;
	srq_ep->ep_fid.rma = &fi_ibv_srq_rma_ops;
	srq_ep->ep_fid.atomic = &fi_ibv_srq_atomic_ops;

	srq_init_attr.attr.max_wr = attr->size;
	srq_init_attr.attr.max_sge = attr->iov_limit;

	srq_ep->srq = ibv_create_srq(dom->pd, &srq_init_attr);
	if (!srq_ep->srq) {
		VERBS_INFO_ERRNO(FI_LOG_DOMAIN, "ibv_create_srq", errno);
		ret = -errno;
		goto err2;
	}

	fastlock_init(&srq_ep->wre_lock);
	srq_ep->wre_pool = util_buf_pool_create(sizeof(struct fi_ibv_wre),
						16, 0, VERBS_WRE_CNT);
	if (!srq_ep->wre_pool) {
		VERBS_WARN(FI_LOG_DOMAIN, "Failed to create wre_pool\n");
		ret = -FI_ENOMEM;
		goto err3;
	}
	dlist_init(&srq_ep->wre_list);

	*srq_ep_fid = &srq_ep->ep_fid;

	return FI_SUCCESS;
err3:
	fastlock_destroy(&srq_ep->wre_lock);
	if (ibv_destroy_srq(srq_ep->srq))
		VERBS_INFO_ERRNO(FI_LOG_DOMAIN, "ibv_destroy_srq", errno);
err2:
	free(srq_ep);
err1:
	return ret;
}

