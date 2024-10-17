#include "spdk/util.h"
#ifdef TARGET_LATENCY_LOG
#include"spdk/nvme.h"
struct nvme_bdev_io {
	#ifdef TARGET_LATENCY_LOG
	struct timespec start_time_ssd;
	struct timespec end_time_ssd;
	struct timespec start_time;
	#endif
	/** array of iovecs to transfer. */
	struct iovec *iovs;

	/** Number of iovecs in iovs array. */
	int iovcnt;

	/** Current iovec position. */
	int iovpos;

	/** Offset in current iovec. */
	uint32_t iov_offset;

	/** I/O path the current I/O or admin passthrough is submitted on, or the I/O path
	 *  being reset in a reset I/O.
	 */
	struct nvme_io_path *io_path;

	/** array of iovecs to transfer. */
	struct iovec *fused_iovs;

	/** Number of iovecs in iovs array. */
	int fused_iovcnt;

	/** Current iovec position. */
	int fused_iovpos;

	/** Offset in current iovec. */
	uint32_t fused_iov_offset;

	/** Saved status for admin passthru completion event, PI error verification, or intermediate compare-and-write status */
	struct spdk_nvme_cpl cpl;

	/** Extended IO opts passed by the user to bdev layer and mapped to NVME format */
	struct spdk_nvme_ns_cmd_ext_io_opts ext_opts;

	/** Keeps track if first of fused commands was submitted */
	bool first_fused_submitted;

	/** Keeps track if first of fused commands was completed */
	bool first_fused_completed;

	/** Temporary pointer to zone report buffer */
	struct spdk_nvme_zns_zone_report *zone_report_buf;

	/** Keep track of how many zones that have been copied to the spdk_bdev_zone_info struct */
	uint64_t handled_zones;

	/** Expiration value in ticks to retry the current I/O. */
	uint64_t retry_ticks;

	/* How many times the current I/O was retried. */
	int32_t retry_count;

	/* Current tsc at submit time. */
	uint64_t submit_tsc;
};
#endif