#include "spdk/util.h"
#ifdef PERF_LATENCY_LOG
#include "spdk_internal/rdma.h"

#define NVME_RDMA_DEFAULT_TX_SGE		2

struct nvme_rdma_wr {
	/* Using this instead of the enum allows this struct to only occupy one byte. */
	uint8_t	type;
};

struct spdk_nvme_rdma_req {
	uint16_t				id;
	uint16_t				completion_flags: 2;
	uint16_t				reserved: 14;
	/* if completion of RDMA_RECV received before RDMA_SEND, we will complete nvme request
	 * during processing of RDMA_SEND. To complete the request we must know the response
	 * received in RDMA_RECV, so store it in this field */
	struct spdk_nvme_rdma_rsp		*rdma_rsp;

	struct nvme_rdma_wr			rdma_wr;

	struct ibv_send_wr			send_wr;

	struct nvme_request			*req;

	struct ibv_sge				send_sgl[NVME_RDMA_DEFAULT_TX_SGE];

	TAILQ_ENTRY(spdk_nvme_rdma_req)		link;
};

#endif