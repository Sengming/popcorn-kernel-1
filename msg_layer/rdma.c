#include <linux/module.h>
#include <linux/bitmap.h>

#include <linux/vmalloc.h>
#include <rdma/rdma_cm.h>

#include "common.h"

#define RDMA_PORT 11453
#define RDMA_ADDR_RESOLVE_TIMEOUT_MS 5000

#define MAX_SEND_DEPTH	((1 << (PAGE_SHIFT + MAX_ORDER - 1)) / PCN_KMSG_MAX_SIZE)
#define MAX_RECV_DEPTH	((1 << (PAGE_SHIFT + MAX_ORDER - 1)) / PCN_KMSG_MAX_SIZE)
#define NR_RDMA_SLOTS	MAX_RECV_DEPTH

struct recv_work {
	struct ib_sge sgl;
	struct ib_recv_wr wr;
	dma_addr_t dma_addr;
	void *buffer;
};

struct send_work {
	struct ib_sge sgl;
	struct ib_send_wr wr;
	dma_addr_t dma_addr;
	void *buffer;
	struct send_work *next;
};

struct rdma_work {
	struct ib_sge sgl;
	struct ib_rdma_wr wr;
	dma_addr_t dma_addr;
	void *buffer;
};

struct rdma_handle {
	int nid;
	enum {
		RDMA_INIT,
		RDMA_ADDR_RESOLVED,
		RDMA_ROUTE_RESOLVED,
		RDMA_CONNECTING,
		RDMA_CONNECTED,
		RDMA_CLOSING,
		RDMA_CLOSED,
	} state;
	struct completion cm_done;

	spinlock_t send_work_pool_lock;
	struct send_work *send_work_pool;
	void *send_buffer;
	dma_addr_t send_buffer_dma_addr;

	struct recv_work *recv_works;
	void *recv_buffer;
	dma_addr_t recv_buffer_dma_addr;

	struct rdma_cm_id *cm_id;
	struct ib_device *device;
	struct ib_cq *cq;
	struct ib_qp *qp;
};

/* RDMA handle for each node */
static struct rdma_handle *rdma_handles[MAX_NUM_NODES] = { NULL };

/* Global protection domain (pd) and memory region (mr) */
static struct ib_pd *rdma_pd = NULL;
static struct ib_mr *rdma_mr = NULL;

/* Global RDMA sink */
static DEFINE_SPINLOCK(__rdma_slots_lock);
static DECLARE_BITMAP(__rdma_slots, NR_RDMA_SLOTS) = {0};
static char *__rdma_sink_addr;
static dma_addr_t __rdma_sink_dma_addr;

static inline int __get_rdma_buffer(char **addr, dma_addr_t *dma_addr) {
	int i;
	spin_lock(&__rdma_slots_lock);
	i = find_first_zero_bit(__rdma_slots, MAX_RECV_DEPTH);
	BUG_ON(i >= MAX_RECV_DEPTH);
	set_bit(i, __rdma_slots);
	spin_unlock(&__rdma_slots_lock);

	if (addr) {
		*addr = __rdma_sink_addr + PCN_KMSG_MAX_SIZE * i;
	}
	if (dma_addr) {
		*dma_addr = __rdma_sink_dma_addr + PCN_KMSG_MAX_SIZE * i;
	}
	return i;
}

static inline void __put_rdma_buffer(int slot) {
	spin_lock(&__rdma_slots_lock);
	BUG_ON(!test_bit(slot, __rdma_slots));
	clear_bit(slot, __rdma_slots);
	spin_unlock(&__rdma_slots_lock);
}

static inline void *__get_rdma_buffer_addr(int slot) {
	return __rdma_sink_addr + PCN_KMSG_MAX_SIZE * slot;
}


/****************************************************************************
 * Send 
 */
static int __post_send(struct rdma_handle *rh, dma_addr_t dma_addr, size_t size, u64 wr_id)
{
	struct ib_send_wr *bad_wr = NULL;
	struct ib_sge sgl = {
		.addr = dma_addr,
		.length = size,
		.lkey = rdma_pd->local_dma_lkey,
	};
	struct ib_send_wr wr = {
		.next = NULL,
		.wr_id = wr_id,
		.sg_list = &sgl,
		.num_sge = 1,
		.opcode = IB_WR_SEND, //IB_WR_SEND_WITH_IMM,
		.send_flags = IB_SEND_SIGNALED,
	};
	int ret;

	ret = ib_post_send(rh->qp, &wr, &bad_wr);
	if (ret) return ret;
	BUG_ON(bad_wr);

	return 0;
}

static int __send_to(int to_nid, void *payload, size_t size)
{
	struct rdma_handle *rh = rdma_handles[to_nid];
	struct ib_device *dev = rh->device;
	dma_addr_t dma_addr;
	int ret;
	DECLARE_COMPLETION_ONSTACK(comp);

	dma_addr = ib_dma_map_single(dev, payload, size, DMA_TO_DEVICE);
	ret = ib_dma_mapping_error(dev, dma_addr);
	if (ret) {
		printk("mapping fail %d\n", ret);
		return -ENODEV;
	}
	ret = __post_send(rh, dma_addr, size, (u64)&comp);
	if (ret) goto out;
	ret = wait_for_completion_io_timeout(&comp, 60 * HZ);
	if (!ret) ret = -EAGAIN;
	ret = 0;

out:
	ib_dma_unmap_single(dev, dma_addr, size, DMA_TO_DEVICE);
	return ret;
}

int rdma_kmsg_send(int dst, struct pcn_kmsg_message *msg, size_t size)
{
	return __send_to(dst, msg, size);
}

int rdma_kmsg_post(int dst, struct pcn_kmsg_message *msg, size_t size)
{
	return __send_to(dst, msg, size);
}


struct rdma_request {
	int nid;
	u32 rkey;
	dma_addr_t addr;
	size_t length;
	char fill;
};

void __test_rdma(int to_nid)
{
	static int sent = 0;
	struct rdma_request req;
	DECLARE_COMPLETION_ONSTACK(comp);
	dma_addr_t dma_addr;
	int ret, i;
	char *dest;
	const int slot = __get_rdma_buffer(&dest, &dma_addr);

	req.nid = my_nid;
	req.rkey = rdma_mr->rkey;
	req.addr = dma_addr;
	req.length = PAGE_SIZE;

	for (i = 0; i < 100000; i++) {
		req.fill = sent++ % 26 + 'a';
		dest[PAGE_SIZE-1] = 0;

		ret = __send_to(to_nid, &req, sizeof(req));
		if (ret) goto out;

		while (true) {
			if (dest[PAGE_SIZE-1] && !dest[0]) {
				printk("What the!!\n");
			}
			if (dest[PAGE_SIZE-1]) break;
		}
		if (dest[0] != req.fill) {
			printk("Somthing happened %c != %c\n", req.fill, dest[0]);
		}
		/*
		if (i && i % 100 == 0) {
			printk("%d completed\n", i);
		}
		*/
	}

out:
	__put_rdma_buffer(slot);
	return;
}

void __perform_rdma(struct ib_wc *wc, struct recv_work *_rw)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	struct rdma_request *req = _rw->buffer;
	struct rdma_work *rw;
	struct ib_sge *sgl;
	struct ib_rdma_wr *wr;
	struct ib_send_wr *bad_wr = NULL;

	char *payload = (void *)__get_free_page(GFP_ATOMIC);
	const int size = PAGE_SIZE;
	dma_addr_t dma_addr;
	int ret;
	BUG_ON(!payload);

	memset(payload, req->fill, PAGE_SIZE);

	dma_addr = ib_dma_map_single(wc->qp->device, payload, size, DMA_TO_DEVICE);
	ret = ib_dma_mapping_error(wc->qp->device, dma_addr);
	BUG_ON(ret);

	rw = kmalloc(sizeof(*rw), GFP_ATOMIC);
	BUG_ON(!rw);

	rw->dma_addr = dma_addr;
	rw->buffer = payload;

	sgl = &rw->sgl;
	sgl->addr = dma_addr;
	sgl->length = size;
	sgl->lkey = rdma_pd->local_dma_lkey;

	wr = &rw->wr;
	wr->wr.next = NULL;
	wr->wr.wr_id = (u64)rw;
	wr->wr.sg_list = sgl;
	wr->wr.num_sge = 1;
	wr->wr.opcode = IB_WR_RDMA_WRITE; // IB_WR_RDMA_WRITE_WITH_IMM;
	wr->wr.send_flags = IB_SEND_SIGNALED;
	wr->remote_addr = req->addr;
	wr->rkey = req->rkey;

	ret = ib_post_send(wc->qp, &wr->wr, &bad_wr);
	if (ret || bad_wr) {
		printk("Cannot post rdma write, %d, %p\n", ret, bad_wr);
		ib_dma_unmap_single(wc->qp->device, dma_addr, size, DMA_TO_DEVICE);
		free_page((unsigned long)payload);
	}
}

void rdma_kmsg_free(struct pcn_kmsg_message *msg)
{
	/* Put back the receive work */
	int ret;
	struct ib_recv_wr *bad_wr = NULL;
	int from_nid = PCN_KMSG_FROM_NID(msg);
	struct rdma_handle *rh = rdma_handles[from_nid];
	int index = ((void *)msg - rh->recv_buffer) / PCN_KMSG_MAX_SIZE;

	ret = ib_post_recv(rh->qp, &rh->recv_works[index].wr, &bad_wr);
	BUG_ON(ret || bad_wr);
}

/****************************************************************************
 * Event handlers
 */
static void __process_recv(struct ib_wc *wc)
{
	struct recv_work *rw = (void *)wc->wr_id;

	//__perform_rdma(wc, rw);
	pcn_kmsg_process(rw->buffer);
}

static void __process_rdma_completion(struct ib_wc *wc)
{
	struct rdma_work *rw = (void *)wc->wr_id;
	ib_dma_unmap_single(wc->qp->device, rw->dma_addr, PAGE_SIZE, DMA_TO_DEVICE);
	free_page((unsigned long)rw->buffer);
}

static void __process_comp_wakeup(struct ib_wc *wc, const char *msg)
{
	struct completion *comp = (void *)wc->wr_id;
	complete(comp);
}

void cq_comp_handler(struct ib_cq *cq, void *context)
{
	int ret;
	struct ib_wc wc;

	while ((ret = ib_poll_cq(cq, 1, &wc)) > 0) {
		if (wc.opcode < 0 || wc.status) {
			struct recv_work *rw = (void *)wc.wr_id;
			printk("abnormal status %d with %d %p\n",
					wc.status, wc.opcode, rw);
			continue;
		}
		switch(wc.opcode) {
		case IB_WC_RECV:
			__process_recv(&wc);
			break;
		case IB_WC_SEND:
			__process_comp_wakeup(&wc, "message sent\n");
			break;
		case IB_WC_REG_MR:
			__process_comp_wakeup(&wc, "mr registered\n");
			break;
		case IB_WC_RDMA_WRITE:
		case IB_WC_RDMA_READ:
			__process_rdma_completion(&wc);
			break;
		default:
			printk("Unknown completion op %d\n", wc.opcode);
			break;
		}
	}
	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
}


/****************************************************************************
 * Setup connections
 */
static int __setup_pd_cq_qp(struct rdma_handle *rh)
{
	int ret;

	BUG_ON(rh->state != RDMA_ROUTE_RESOLVED && "for rh->device");

	/* Create global pd if it is not allocated yet */
	if (!rdma_pd) {
		rdma_pd = ib_alloc_pd(rh->device);
		if (IS_ERR(rdma_pd)) {
			ret = PTR_ERR(rdma_pd);
			rdma_pd = NULL;
			goto out_err;
		}
	}

	/* create completion queue */
	if (!rh->cq) {
		struct ib_cq_init_attr cq_attr = {
			.cqe = MAX_SEND_DEPTH + MAX_RECV_DEPTH,
			.comp_vector = 0,
		};

		rh->cq = ib_create_cq(
				rh->device, cq_comp_handler, NULL, rh, &cq_attr);
		if (IS_ERR(rh->cq)) {
			ret = PTR_ERR(rh->cq);
			goto out_err;
		}

		ret = ib_req_notify_cq(rh->cq, IB_CQ_NEXT_COMP);
		if (ret < 0) goto out_err;
	}

	/* create queue pair */
	{
		struct ib_qp_init_attr qp_attr = {
			.event_handler = NULL, // qp_event_handler,
			.qp_context = rh,
			.cap = {
				.max_send_wr = MAX_SEND_DEPTH,
				.max_recv_wr = MAX_RECV_DEPTH,
				.max_send_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
				.max_recv_sge = PCN_KMSG_MAX_SIZE >> PAGE_SHIFT,
			},
			.sq_sig_type = IB_SIGNAL_REQ_WR,
			.qp_type = IB_QPT_RC,
			.send_cq = rh->cq,
			.recv_cq = rh->cq,
		};

		ret = rdma_create_qp(rh->cm_id, rdma_pd, &qp_attr);
		if (ret) goto out_err;
		rh->qp = rh->cm_id->qp;
	}
	return 0;

out_err:
	return ret;
}

static int __setup_buffers_and_pools(struct rdma_handle *rh)
{
	int ret = 0, i;
	dma_addr_t dma_addr;
	char *recv_buffer = NULL;
	struct recv_work *rws = NULL;
	const size_t buffer_size = PCN_KMSG_MAX_SIZE * MAX_RECV_DEPTH;

	/* Initalize receive buffers */
	recv_buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (!recv_buffer) {
		return -ENOMEM;
	}
	rws = kmalloc(sizeof(*rws) * MAX_RECV_DEPTH, GFP_KERNEL);
	if (!rws) {
		ret = -ENOMEM;
		goto out_free;
	}

	/* Populate receive buffer and work requests */
	dma_addr = ib_dma_map_single(
			rh->device, recv_buffer, buffer_size, DMA_FROM_DEVICE);
	ret = ib_dma_mapping_error(rh->device, dma_addr);
	if (ret) goto out_free;

	for (i = 0; i < MAX_RECV_DEPTH; i++) {
		struct recv_work *rw = rws + i;
		struct ib_recv_wr *wr, *bad_wr = NULL;
		struct ib_sge *sgl;

		rw->dma_addr = dma_addr + PCN_KMSG_MAX_SIZE * i;
		rw->buffer = recv_buffer + PCN_KMSG_MAX_SIZE * i;

		sgl = &rw->sgl;
		sgl->lkey = rdma_pd->local_dma_lkey;
		sgl->addr = rw->dma_addr;
		sgl->length = PCN_KMSG_MAX_SIZE;

		wr = &rw->wr;
		wr->sg_list = sgl;
		wr->num_sge = 1;
		wr->next = NULL;
		wr->wr_id = (u64)rw;

		ret = ib_post_recv(rh->qp, wr, &bad_wr);
		if (ret || bad_wr) goto out_free;
	}
	rh->recv_works = rws;
	rh->recv_buffer = recv_buffer;
	rh->recv_buffer_dma_addr = dma_addr;

	return ret;

out_free:
	if (recv_buffer) kfree(recv_buffer);
	if (rws) kfree(rws);
	return ret;
}

static int __setup_rdma_buffer(const int nr_chunks)
{
	int ret;
	DECLARE_COMPLETION_ONSTACK(comp);
	struct ib_mr *mr = NULL;
	struct ib_send_wr *bad_wr = NULL;
	struct ib_reg_wr reg_wr = {
		.wr = {
			.opcode = IB_WR_REG_MR,
			.send_flags = IB_SEND_SIGNALED,
			.wr_id = (u64)&comp,
		},
		.access = IB_ACCESS_LOCAL_WRITE |
				  IB_ACCESS_REMOTE_READ |
				  IB_ACCESS_REMOTE_WRITE,
	};
	struct scatterlist sg = {};
	const int alloc_order = MAX_ORDER - 1;

	__rdma_sink_addr = (void *)__get_free_pages(GFP_KERNEL, alloc_order);
	if (!__rdma_sink_addr) return -EINVAL;

	__rdma_sink_dma_addr = ib_dma_map_single(
			rdma_pd->device, __rdma_sink_addr, 1 << (PAGE_SHIFT + alloc_order),
			DMA_FROM_DEVICE);
	ret = ib_dma_mapping_error(rdma_pd->device, __rdma_sink_dma_addr);
	if (ret) goto out_free;

	mr = ib_alloc_mr(rdma_pd, IB_MR_TYPE_MEM_REG, 1 << alloc_order);
	if (IS_ERR(mr)) goto out_free;

	sg_dma_address(&sg) = __rdma_sink_dma_addr;
	sg_dma_len(&sg) = 1 << (PAGE_SHIFT + alloc_order);

	ret = ib_map_mr_sg(mr, &sg, 1, PAGE_SIZE);
	if (ret != 1) {
		printk("Cannot map scatterlist to mr, %d\n", ret);
		goto out_dereg;
	}
	reg_wr.mr = mr;
	reg_wr.key = mr->rkey;

	/**
	 * rdma_handles[my_nid] is for accepting connection without qp & cp.
	 * So, let's use rdma_handles[1] for nid 0 and rdma_handles[0] otherwise.
	 */
	ret = ib_post_send(rdma_handles[!my_nid]->qp, &reg_wr.wr, &bad_wr);
	if (ret || bad_wr) {
		printk("Cannot register mr, %d %p\n", ret, bad_wr);
		if (bad_wr) ret = -EINVAL;
		goto out_dereg;
	}
	ret = wait_for_completion_io_timeout(&comp, 5 * HZ);
	if (!ret) {
		printk("Timed-out to register mr\n");
		ret = -EBUSY;
		goto out_dereg;
	}

	rdma_mr = mr;
	//printk("lkey: %x, rkey: %x, length: %x\n", mr->lkey, mr->rkey, mr->length);
	return 0;

out_dereg:
	ib_dereg_mr(mr);
	return ret;
	
out_free:
	free_pages((unsigned long)__rdma_sink_addr, alloc_order);
	__rdma_sink_addr = NULL;
	return ret;
}


/****************************************************************************
 * Client-side connection handling
 */
int cm_client_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_handle *rh = cm_id->context;

	switch (cm_event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rh->state = RDMA_ADDR_RESOLVED;
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		rh->state = RDMA_ROUTE_RESOLVED;
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		rh->state = RDMA_CONNECTED;
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		MSGPRINTK("Disconnected from %d\n", rh->nid);
		/* TODO deallocate associated resources */
		break;
	case RDMA_CM_EVENT_REJECTED:
	case RDMA_CM_EVENT_CONNECT_ERROR:
		complete(&rh->cm_done);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	default:
		printk("Unhandled client event %d\n", cm_event->event);
		break;
	}
	return 0;
}

static int __connect_to_server(int nid)
{
	int ret;
	const char *step;
	struct rdma_handle *rh = rdma_handles[nid];

	step = "create rdma id";
	rh->cm_id = rdma_create_id(&init_net,
			cm_client_event_handler, rh, RDMA_PS_IB, IB_QPT_RC);
	if (IS_ERR(rh->cm_id)) goto out_err;

	step = "resolve server address";
	{
		struct sockaddr_in addr = {
			.sin_family = AF_INET,
			.sin_port = htons(RDMA_PORT),
			.sin_addr.s_addr = ip_table[nid],
		};

		ret = rdma_resolve_addr(rh->cm_id, NULL,
				(struct sockaddr *)&addr, RDMA_ADDR_RESOLVE_TIMEOUT_MS);
		if (ret) goto out_err;
		ret = wait_for_completion_interruptible(&rh->cm_done);
		if (ret || rh->state != RDMA_ADDR_RESOLVED) goto out_err;
	}

	step = "resolve routing path";
	ret = rdma_resolve_route(rh->cm_id, RDMA_ADDR_RESOLVE_TIMEOUT_MS);
	if (ret) goto out_err;
	ret = wait_for_completion_interruptible(&rh->cm_done);
	if (ret || rh->state != RDMA_ROUTE_RESOLVED) goto out_err;

	/* cm_id->device is valid after the address and route are resolved */
	rh->device = rh->cm_id->device;

	step = "setup ib";
	ret = __setup_pd_cq_qp(rh);
	if (ret) goto out_err;

	step = "setup buffers and pools";
	ret = __setup_buffers_and_pools(rh);
	if (ret) goto out_err;

	step = "connect";
	{
		struct rdma_conn_param conn_param = {
			.private_data = &my_nid,
			.private_data_len = sizeof(my_nid),
		};

		rh->state = RDMA_CONNECTING;
		ret = rdma_connect(rh->cm_id, &conn_param);
		if (ret) goto out_err;
		ret = wait_for_completion_interruptible(&rh->cm_done);
		if (ret) goto out_err;
		if (rh->state != RDMA_CONNECTED) {
			ret = -EAGAIN;
			goto out_err;
		}
	}

	MSGPRINTK("Connected to %d\n", nid);
	return 0;

out_err:
	PCNPRINTK_ERR("Unable to %s, %pI4, %d\n", step, ip_table + nid, ret);
	return ret;
}


/****************************************************************************
 * Server-side connection handling
 */
static int __accept_client(int nid)
{
	struct rdma_handle *rh = rdma_handles[nid];
	struct rdma_conn_param conn_param = {};
	int ret;

	ret = wait_for_completion_io_timeout(&rh->cm_done, 60 * HZ);
	if (!ret) return -EAGAIN;
	if (rh->state != RDMA_ROUTE_RESOLVED) return -EINVAL;

	ret = __setup_pd_cq_qp(rh);
	if (ret) return ret;

	ret = __setup_buffers_and_pools(rh);
	if (ret) return ret;

	rh->state = RDMA_CONNECTING;
	ret = rdma_accept(rh->cm_id, &conn_param);
	if (ret) return ret;

	ret = wait_for_completion_interruptible(&rh->cm_done);
	if (ret) return ret;

	return 0;
}
static int __on_client_connecting(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	int peer_nid = *(int *)cm_event->param.conn.private_data;
	struct rdma_handle *rh = rdma_handles[peer_nid];

	cm_id->context = rh;
	rh->cm_id = cm_id;
	rh->device = cm_id->device;
	rh->state = RDMA_ROUTE_RESOLVED;

	complete(&rh->cm_done);
	return 0;
}

static int __on_client_connected(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_handle *rh = cm_id->context;
	rh->state = RDMA_CONNECTED;
	complete(&rh->cm_done);

	MSGPRINTK("Connected to %d\n", rh->nid);
	return 0;
}

static int __on_client_disconnected(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	struct rdma_handle *rh = cm_id->context;
	rh->state = RDMA_INIT;
	set_popcorn_node_online(rh->nid, false);

	MSGPRINTK("Disconnected from %d\n", rh->nid);
	return 0;
}

int cm_server_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *cm_event)
{
	int ret = 0;
	switch (cm_event->event) {
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		ret = __on_client_connecting(cm_id, cm_event);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		ret = __on_client_connected(cm_id, cm_event);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		ret = __on_client_disconnected(cm_id, cm_event);
		break;
	default:
		MSGPRINTK("Unhandled server event %d\n", cm_event->event);
		break;
	}
	return 0;
}

static int __listen_to_connection(void)
{
	int ret;
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(RDMA_PORT),
		.sin_addr.s_addr = ip_table[my_nid],
	};

	struct rdma_cm_id *cm_id = rdma_create_id(&init_net,
			cm_server_event_handler, NULL, RDMA_PS_IB, IB_QPT_RC);
	if (IS_ERR(cm_id)) return PTR_ERR(cm_id);
	rdma_handles[my_nid]->cm_id = cm_id;

	ret = rdma_bind_addr(cm_id, (struct sockaddr *)&addr);
	if (ret) {
		PCNPRINTK_ERR("Cannot bind server address, %d\n", ret);
		return ret;
	}

	ret = rdma_listen(cm_id, MAX_NUM_NODES);
	if (ret) {
		PCNPRINTK_ERR("Cannot listen to incoming requests, %d\n", ret);
		return ret;
	}

	return 0;
}


static int __establish_connections(void)
{
	int i, ret;

	ret = __listen_to_connection();
	if (ret) return ret;

	/* Wait for a while so that nodes are ready to listen to connections */
	msleep(100);

	for (i = 0; i < my_nid; i++) {
		if ((ret = __connect_to_server(i))) return ret;
		set_popcorn_node_online(i, true);
	}

	set_popcorn_node_online(my_nid, true);

	for (i = my_nid + 1; i < MAX_NUM_NODES; i++) {
		if ((ret = __accept_client(i))) return ret;
		set_popcorn_node_online(rh->nid, true);
	}

	if ((ret = __setup_rdma_buffer(1))) return ret;

	printk("Connections are established.\n");
	return 0;
}

void __exit exit_kmsg_rdma(void)
{
	int i;

	/* Detach from upper layer to prevent race condition during exit */
	pcn_kmsg_set_transport(NULL);

	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_handle *rh = rdma_handles[i];
		set_popcorn_node_online(i, false);
		if (!rh) continue;

		if (rh->recv_buffer) {
			ib_dma_unmap_single(rh->device, rh->recv_buffer_dma_addr,
					PCN_KMSG_MAX_SIZE * MAX_RECV_DEPTH, DMA_FROM_DEVICE);
			kfree(rh->recv_buffer);
			kfree(rh->recv_works);
		}

		if (rh->qp && !IS_ERR(rh->qp)) rdma_destroy_qp(rh->cm_id);
		if (rh->cq && !IS_ERR(rh->cq)) ib_destroy_cq(rh->cq);
		if (rh->cm_id && !IS_ERR(rh->cm_id)) rdma_destroy_id(rh->cm_id);

		kfree(rdma_handles[i]);
	}

	/* MR is set correctly iff rdma buffer and pd are correctly allocated */
	if (rdma_mr && !IS_ERR(rdma_mr)) {
		ib_dereg_mr(rdma_mr);
		ib_dma_unmap_single(rdma_pd->device, __rdma_sink_dma_addr,
				1 << (PAGE_SHIFT + MAX_ORDER - 1), DMA_FROM_DEVICE);
		free_pages((unsigned long)__rdma_sink_addr, MAX_ORDER - 1);
		ib_dealloc_pd(rdma_pd);
	}

	MSGPRINTK("Popcorn message layer over RDMA unloaded\n");
	return;
}

struct pcn_kmsg_transport transport_rdma = {
	.name = "rdma",
	.type = PCN_KMSG_LAYER_TYPE_RDMA,

	.send_fn = rdma_kmsg_send,
	.post_fn = rdma_kmsg_post,
	.free_fn = rdma_kmsg_free,
};

int __init init_kmsg_rdma(void)
{
	int i;

	MSGPRINTK("\nLoading Popcorn messaging layer over RDMA...\n");

	if (!identify_myself()) return -EINVAL;
	pcn_kmsg_set_transport(&transport_rdma);

	for (i = 0; i < MAX_NUM_NODES; i++) {
		struct rdma_handle *rh;
		rh = rdma_handles[i] = kzalloc(sizeof(struct rdma_handle), GFP_KERNEL);
		if (!rh) goto out_free;

		rh->nid = i;
		rh->state = RDMA_INIT;
		init_completion(&rh->cm_done);
	}

	if (__establish_connections()) {
		goto out_free;
	}

	broadcast_my_node_info(i);

	PCNPRINTK("Popcorn messaging layer over RDMA is ready\n");
	return 0;

out_free:
	exit_kmsg_rdma();
	return -EINVAL;
}

module_init(init_kmsg_rdma);
module_exit(exit_kmsg_rdma);
MODULE_LICENSE("GPL");