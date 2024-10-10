#include "spdk/util.h"
#ifdef LANTENCY_LOG
#include "spdk/nvmf_transport.h"
#include "spdk_internal/rdma.h"

#define SPDK_NVMF_MAX_SGL_ENTRIES	16
#define NVMF_DEFAULT_MSDBD		16
#define NVMF_DEFAULT_TX_SGE		SPDK_NVMF_MAX_SGL_ENTRIES
#define NVMF_DEFAULT_RSP_SGE		1
#define NVMF_DEFAULT_RX_SGE		2

struct spdk_nvmf_rdma_wr {
	/* Uses enum spdk_nvmf_rdma_wr_type */
	uint8_t type;
};

/* This structure holds commands as they are received off the wire.
 * It must be dynamically paired with a full request object
 * (spdk_nvmf_rdma_request) to service a request. It is separate
 * from the request because RDMA does not appear to order
 * completions, so occasionally we'll get a new incoming
 * command when there aren't any free request objects.
 */
struct spdk_nvmf_rdma_recv {
	struct ibv_recv_wr			wr;
	#ifdef LANTENCY_LOG
	uint64_t io_id;
	#endif
	struct ibv_sge				sgl[NVMF_DEFAULT_RX_SGE];

	struct spdk_nvmf_rdma_qpair		*qpair;

	/* In-capsule data buffer */
	uint8_t					*buf;

	struct spdk_nvmf_rdma_wr		rdma_wr;
	uint64_t				receive_tsc;

	STAILQ_ENTRY(spdk_nvmf_rdma_recv)	link;
};

struct spdk_nvmf_rdma_request_data {
	struct ibv_send_wr		wr;
	struct ibv_sge			sgl[SPDK_NVMF_MAX_SGL_ENTRIES];
};

struct spdk_nvmf_rdma_request {
	struct spdk_nvmf_request		req;
	#ifdef LANTENCY_LOG
	uint64_t io_id;
	struct timespec start_time;
	#endif

	bool					fused_failed;

	struct spdk_nvmf_rdma_wr		data_wr;
	struct spdk_nvmf_rdma_wr		rsp_wr;

	/* Uses enum spdk_nvmf_rdma_request_state */
	uint8_t					state;

	/* Data offset in req.iov */
	uint32_t				offset;

	struct spdk_nvmf_rdma_recv		*recv;

	struct {
		struct	ibv_send_wr		wr;
		struct	ibv_sge			sgl[NVMF_DEFAULT_RSP_SGE];
	} rsp;

	uint16_t				iovpos;
	uint16_t				num_outstanding_data_wr;
	/* Used to split Write IO with multi SGL payload */
	uint16_t				num_remaining_data_wr;
	uint64_t				receive_tsc;
	struct spdk_nvmf_rdma_request		*fused_pair;
	STAILQ_ENTRY(spdk_nvmf_rdma_request)	state_link;
	struct ibv_send_wr			*remaining_tranfer_in_wrs;
	struct ibv_send_wr			*transfer_wr;
	struct spdk_nvmf_rdma_request_data	data;
};
#endif