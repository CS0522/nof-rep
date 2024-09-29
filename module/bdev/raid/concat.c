/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) Peng Yu yupeng0921@gmail.com.
 *   All rights reserved.
 */

#include "bdev_raid.h"

#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "spdk/log.h"

struct concat_block_range {
	uint64_t start;
	uint64_t length;
};

/*
 * brief:
 * concat_bdev_io_completion function is called by lower layers to notify raid
 * module that particular bdev_io is completed.
 * params:
 * bdev_io - pointer to bdev io submitted to lower layers, like child io
 * success - bdev_io status
 * cb_arg - function callback context (parent raid_bdev_io)
 * returns:
 * none
 */
static void
concat_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (success) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
	} else {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void concat_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_concat_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	concat_submit_rw_request(raid_io);
}

/*
 * brief:
 * concat_submit_rw_request function is used to submit I/O to the correct
 * member disk for concat bdevs.
 * params:
 * raid_io
 * returns:
 * none
 */
static void
concat_submit_rw_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev_io_channel	*raid_ch = raid_io->raid_ch;
	struct raid_bdev		*raid_bdev = raid_io->raid_bdev;
	struct concat_block_range	*block_range = raid_bdev->module_private;
	uint64_t			pd_lba;
	uint64_t			pd_blocks;
	int				pd_idx;
	int				ret = 0;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;
	struct spdk_bdev_ext_io_opts	io_opts = {};
	int i;

	pd_idx = -1;
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		if (block_range[i].start > raid_io->offset_blocks) {
			break;
		}
		pd_idx = i;
	}
	assert(pd_idx >= 0);
	assert(raid_io->offset_blocks >= block_range[pd_idx].start);
	pd_lba = raid_io->offset_blocks - block_range[pd_idx].start;
	pd_blocks = raid_io->num_blocks;
	base_info = &raid_bdev->base_bdev_info[pd_idx];
	if (base_info->desc == NULL) {
		SPDK_ERRLOG("base bdev desc null for pd_idx %u\n", pd_idx);
		assert(0);
	}

	/*
	 * Submit child io to bdev layer with using base bdev descriptors, base
	 * bdev lba, base bdev child io length in blocks, buffer, completion
	 * function and function callback context
	 */
	assert(raid_ch != NULL);
	base_ch = raid_bdev_channel_get_base_channel(raid_ch, pd_idx);

	io_opts.size = sizeof(io_opts);
	io_opts.memory_domain = raid_io->memory_domain;
	io_opts.memory_domain_ctx = raid_io->memory_domain_ctx;
	io_opts.metadata = raid_io->md_buf;

	if (raid_io->type == SPDK_BDEV_IO_TYPE_READ) {
		ret = raid_bdev_readv_blocks_ext(base_info, base_ch,
						 raid_io->iovs, raid_io->iovcnt,
						 pd_lba, pd_blocks, concat_bdev_io_completion,
						 raid_io, &io_opts);
	} else if (raid_io->type == SPDK_BDEV_IO_TYPE_WRITE) {
		ret = raid_bdev_writev_blocks_ext(base_info, base_ch,
						  raid_io->iovs, raid_io->iovcnt,
						  pd_lba, pd_blocks, concat_bdev_io_completion,
						  raid_io, &io_opts);
	} else {
		SPDK_ERRLOG("Recvd not supported io type %u\n", raid_io->type);
		assert(0);
	}

	if (ret == -ENOMEM) {
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _concat_submit_rw_request);
	} else if (ret != 0) {
		SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
		assert(false);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void concat_submit_null_payload_request(struct raid_bdev_io *raid_io);

static void
_concat_submit_null_payload_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	concat_submit_null_payload_request(raid_io);
}

static void
concat_base_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);

	spdk_bdev_free_io(bdev_io);
}

/*
 * brief:
 * concat_submit_null_payload_request function submits the next batch of
 * io requests with range but without payload, like FLUSH and UNMAP, to member disks;
 * it will submit as many as possible unless one base io request fails with -ENOMEM,
 * in which case it will queue itself for later submission.
 * params:
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
concat_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev		*raid_bdev;
	int				ret;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;
	uint64_t			pd_lba;
	uint64_t			pd_blocks;
	uint64_t			offset_blocks;
	uint64_t			num_blocks;
	struct concat_block_range	*block_range;
	int				i, start_idx, stop_idx;

	raid_bdev = raid_io->raid_bdev;
	block_range = raid_bdev->module_private;

	offset_blocks = raid_io->offset_blocks;
	num_blocks = raid_io->num_blocks;
	start_idx = -1;
	stop_idx = -1;
	/*
	 * Go through all base bdevs, find the first bdev and the last bdev
	 */
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* skip the bdevs before the offset_blocks */
		if (offset_blocks >= block_range[i].start + block_range[i].length) {
			continue;
		}
		if (start_idx == -1) {
			start_idx = i;
		} else {
			/*
			 * The offset_blocks might be at the middle of the first bdev.
			 * Besides the first bdev, the offset_blocks should be always
			 * at the start of the bdev.
			 */
			assert(offset_blocks == block_range[i].start);
		}
		pd_lba = offset_blocks - block_range[i].start;
		pd_blocks = spdk_min(num_blocks, block_range[i].length - pd_lba);
		offset_blocks += pd_blocks;
		num_blocks -= pd_blocks;
		if (num_blocks == 0) {
			stop_idx = i;
			break;
		}
	}
	assert(start_idx >= 0);
	assert(stop_idx >= 0);

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_io->base_bdev_io_remaining = stop_idx - start_idx + 1;
	}
	offset_blocks = raid_io->offset_blocks;
	num_blocks = raid_io->num_blocks;
	for (i = start_idx; i <= stop_idx; i++) {
		assert(offset_blocks >= block_range[i].start);
		assert(offset_blocks < block_range[i].start + block_range[i].length);
		pd_lba = offset_blocks -  block_range[i].start;
		pd_blocks = spdk_min(num_blocks, block_range[i].length - pd_lba);
		offset_blocks += pd_blocks;
		num_blocks -= pd_blocks;
		/*
		 * Skip the IOs we have submitted
		 */
		if (i < start_idx + raid_io->base_bdev_io_submitted) {
			continue;
		}
		base_info = &raid_bdev->base_bdev_info[i];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, i);
		switch (raid_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			ret = raid_bdev_unmap_blocks(base_info, base_ch,
						     pd_lba, pd_blocks,
						     concat_base_io_complete, raid_io);
			break;
		case SPDK_BDEV_IO_TYPE_FLUSH:
			ret = raid_bdev_flush_blocks(base_info, base_ch,
						     pd_lba, pd_blocks,
						     concat_base_io_complete, raid_io);
			break;
		default:
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", raid_io->type);
			assert(false);
			ret = -EIO;
		}
		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _concat_submit_null_payload_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

static int
concat_start(struct raid_bdev *raid_bdev)
{
	uint64_t total_blockcnt = 0;
	struct raid_base_bdev_info *base_info;
	struct concat_block_range *block_range;

	block_range = calloc(raid_bdev->num_base_bdevs, sizeof(struct concat_block_range));
	if (!block_range) {
		SPDK_ERRLOG("Can not allocate block_range, num_base_bdevs: %u",
			    raid_bdev->num_base_bdevs);
		return -ENOMEM;
	}

	int idx = 0;
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		uint64_t strip_cnt = base_info->data_size >> raid_bdev->strip_size_shift;
		uint64_t pd_block_cnt = strip_cnt << raid_bdev->strip_size_shift;

		base_info->data_size = pd_block_cnt;

		block_range[idx].start = total_blockcnt;
		block_range[idx].length = pd_block_cnt;
		total_blockcnt += pd_block_cnt;
		idx++;
	}

	raid_bdev->module_private = block_range;

	SPDK_DEBUGLOG(bdev_concat, "total blockcount %" PRIu64 ",  numbasedev %u, strip size shift %u\n",
		      total_blockcnt, raid_bdev->num_base_bdevs, raid_bdev->strip_size_shift);
	raid_bdev->bdev.blockcnt = total_blockcnt;

	raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
	raid_bdev->bdev.split_on_optimal_io_boundary = true;

	return 0;
}

static bool
concat_stop(struct raid_bdev *raid_bdev)
{
	struct concat_block_range *block_range = raid_bdev->module_private;

	free(block_range);

	return true;
}

static struct raid_bdev_module g_concat_module = {
	.level = CONCAT,
	.base_bdevs_min = 1,
	.memory_domains_supported = true,
	.start = concat_start,
	.stop = concat_stop,
	.submit_rw_request = concat_submit_rw_request,
	.submit_null_payload_request = concat_submit_null_payload_request,
};
RAID_MODULE_REGISTER(&g_concat_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_concat)
