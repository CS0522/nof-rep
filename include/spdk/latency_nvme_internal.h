#include "spdk/util.h"
#ifdef PERF_LATENCY_LOG
#include "spdk/nvme.h"

struct nvme_error_cmd {
	bool				do_not_submit;
	uint64_t			timeout_tsc;
	uint32_t			err_count;
	uint8_t				opc;
	struct spdk_nvme_status		status;
	TAILQ_ENTRY(nvme_error_cmd)	link;
};

struct nvme_payload {
	/**
	 * Functions for retrieving physical addresses for scattered payloads.
	 */
	spdk_nvme_req_reset_sgl_cb reset_sgl_fn;
	spdk_nvme_req_next_sge_cb next_sge_fn;

	/**
	 * Extended IO options passed by the user
	 */
	struct spdk_nvme_ns_cmd_ext_io_opts *opts;
	/**
	 * If reset_sgl_fn == NULL, this is a contig payload, and contig_or_cb_arg contains the
	 * virtual memory address of a single virtually contiguous buffer.
	 *
	 * If reset_sgl_fn != NULL, this is a SGL payload, and contig_or_cb_arg contains the
	 * cb_arg that will be passed to the SGL callback functions.
	 */
	void *contig_or_cb_arg;

	/** Virtual memory address of a single virtually contiguous metadata buffer */
	void *md;
};

struct nvme_request {
	// cmd.cid 与 rdma_req 绑定
	struct spdk_nvme_cmd		cmd;
	#ifdef TARGET_LATENCY_LOG
	struct timespec start_time;
	#endif

	uint8_t				retries;

	uint8_t				timed_out : 1;

	/**
	 * True if the request is in the queued_req list.
	 */
	uint8_t				queued : 1;
	uint8_t				reserved : 6;

	/**
	 * Number of children requests still outstanding for this
	 *  request which was split into multiple child requests.
	 */
	uint16_t			num_children;

	/**
	 * Offset in bytes from the beginning of payload for this request.
	 * This is used for I/O commands that are split into multiple requests.
	 */
	uint32_t			payload_offset;
	uint32_t			md_offset;

	uint32_t			payload_size;

	/**
	 * Timeout ticks for error injection requests, can be extended in future
	 * to support per-request timeout feature.
	 */
	uint64_t			timeout_tsc;

	/**
	 * Data payload for this request's command.
	 */
	struct nvme_payload		payload;

	spdk_nvme_cmd_cb		cb_fn;
	void				*cb_arg;
	STAILQ_ENTRY(nvme_request)	stailq;

	struct spdk_nvme_qpair		*qpair;

	#ifdef PERF_LATENCY_LOG
	// 统计性能涉及 id
    uint32_t io_id;
	uint32_t ns_id;
	// 统计性能涉及计算时间
	// 提交 nvme req 的时间
    struct timespec req_submit_time;
	// 完成 nvme req 的时间
	struct timespec req_complete_time;
	// 提交 wr 的时间
	struct timespec wr_send_time;
	// 提交 wr 完成的时间
	struct timespec wr_send_complete_time;
    // wr 完成的时间
    struct timespec wr_recv_time;
	#endif

	/*
	 * The value of spdk_get_ticks() when the request was submitted to the hardware.
	 * Only set if ctrlr->timeout_enabled is true.
	 */
	uint64_t			submit_tick;

	/**
	 * The active admin request can be moved to a per process pending
	 *  list based on the saved pid to tell which process it belongs
	 *  to. The cpl saves the original completion information which
	 *  is used in the completion callback.
	 * NOTE: these below two fields are only used for admin request.
	 */
	pid_t				pid;
	struct spdk_nvme_cpl		cpl;

	uint32_t			md_size;

	/**
	 * The following members should not be reordered with members
	 *  above.  These members are only needed when splitting
	 *  requests which is done rarely, and the driver is careful
	 *  to not touch the following fields until a split operation is
	 *  needed, to avoid touching an extra cacheline.
	 */

	/**
	 * Points to the outstanding child requests for a parent request.
	 *  Only valid if a request was split into multiple children
	 *  requests, and is not initialized for non-split requests.
	 */
	TAILQ_HEAD(, nvme_request)	children;

	/**
	 * Linked-list pointers for a child request in its parent's list.
	 */
	TAILQ_ENTRY(nvme_request)	child_tailq;

	/**
	 * Points to a parent request if part of a split request,
	 *   NULL otherwise.
	 */
	struct nvme_request		*parent;

	/**
	 * Completion status for a parent request.  Initialized to all 0's
	 *  (SUCCESS) before child requests are submitted.  If a child
	 *  request completes with error, the error status is copied here,
	 *  to ensure that the parent request is also completed with error
	 *  status once all child requests are completed.
	 */
	struct spdk_nvme_cpl		parent_status;

	/**
	 * The user_cb_fn and user_cb_arg fields are used for holding the original
	 * callback data when using nvme_allocate_request_user_copy.
	 */
	spdk_nvme_cmd_cb		user_cb_fn;
	void				*user_cb_arg;
	void				*user_buffer;

	/** Sequence of accel operations associated with this request */
	void				*accel_sequence;
};
#endif