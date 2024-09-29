/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "ftl_core.h"
#include "ftl_utils.h"
#include "ftl_mngt.h"
#include "ftl_mngt_steps.h"
#include "ftl_band.h"
#include "ftl_internal.h"
#include "ftl_nv_cache.h"
#include "ftl_debug.h"
#include "ftl_utils.h"

void
ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_conf_is_valid(&dev->conf)) {
		ftl_mngt_next_step(mngt);
	} else {
		ftl_mngt_fail_step(mngt);
	}
}

static int
init_p2l_map_pool(struct spdk_ftl_dev *dev)
{
	size_t p2l_pool_el_blks = spdk_divide_round_up(ftl_p2l_map_pool_elem_size(dev), FTL_BLOCK_SIZE);
	size_t p2l_pool_buf_blks = P2L_MEMPOOL_SIZE * p2l_pool_el_blks;
	void *p2l_pool_buf;

	dev->p2l_pool_md = ftl_md_create(dev, p2l_pool_buf_blks, 0, "p2l_pool",
					 ftl_md_create_shm_flags(dev), NULL);
	if (!dev->p2l_pool_md) {
		return -ENOMEM;
	}

	p2l_pool_buf = ftl_md_get_buffer(dev->p2l_pool_md);
	dev->p2l_pool = ftl_mempool_create_ext(p2l_pool_buf, P2L_MEMPOOL_SIZE,
					       p2l_pool_el_blks * FTL_BLOCK_SIZE,
					       FTL_BLOCK_SIZE);
	if (!dev->p2l_pool) {
		return -ENOMEM;
	}

	if (!ftl_fast_startup(dev)) {
		ftl_mempool_initialize_ext(dev->p2l_pool);
	}

	return 0;
}

static int
init_band_md_pool(struct spdk_ftl_dev *dev)
{
	dev->band_md_pool = ftl_mempool_create(P2L_MEMPOOL_SIZE,
					       sizeof(struct ftl_band_md),
					       FTL_BLOCK_SIZE,
					       SPDK_ENV_SOCKET_ID_ANY);
	if (!dev->band_md_pool) {
		return -ENOMEM;
	}

	return 0;
}

void
ftl_mngt_init_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (init_p2l_map_pool(dev)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	if (init_band_md_pool(dev)) {
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_mem_pools(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->p2l_pool) {
		ftl_mempool_destroy_ext(dev->p2l_pool);
		dev->p2l_pool = NULL;
	}

	if (dev->p2l_pool_md) {
		ftl_md_destroy(dev->p2l_pool_md, ftl_md_destroy_shm_flags(dev));
		dev->p2l_pool_md = NULL;
	}

	if (dev->band_md_pool) {
		ftl_mempool_destroy(dev->band_md_pool);
		dev->band_md_pool = NULL;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_init_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->reloc = ftl_reloc_init(dev);
	if (!dev->reloc) {
		FTL_ERRLOG(dev, "Unable to initialize reloc structures\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_reloc(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_reloc_free(dev->reloc);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_init_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_nv_cache_init(dev)) {
		FTL_ERRLOG(dev, "Unable to initialize persistent cache\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_nv_cache_deinit(dev);
	ftl_mngt_next_step(mngt);
}

static void
user_clear_cb(struct spdk_ftl_dev *dev, void *cb_ctx, int status)
{
	struct ftl_mngt_process *mngt = cb_ctx;

	if (status) {
		FTL_ERRLOG(ftl_mngt_get_dev(mngt), "FTL NV Cache: ERROR of clearing user cache data\n");
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	bool is_first_start = (dev->conf.mode & SPDK_FTL_MODE_CREATE) != 0;
	bool is_major_upgrade = dev->sb->clean == 1 && dev->sb_shm->shm_clean == 0 &&
				dev->sb->upgrade_ready == 1;

	if (is_first_start || is_major_upgrade) {
		FTL_NOTICELOG(dev, "NV cache data region needs scrubbing, this may take a while.\n");
		FTL_NOTICELOG(dev, "Scrubbing %"PRIu64" chunks\n", dev->layout.nvc.chunk_count);

		/* Need to scrub user data, so in case of dirty shutdown the recovery won't
		 * pull in data during open chunks recovery from any previous instance (since during short
		 * tests it's very likely that chunks seq_id will be in line between new head md and old VSS)
		 */
		ftl_nv_cache_scrub(dev, user_clear_cb, mngt);
	} else {
		ftl_mngt_skip_step(mngt);
	}
}

void
ftl_mngt_finalize_startup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (ftl_bitmap_find_first_set(dev->trim_map, 0, UINT64_MAX) != UINT64_MAX) {
		dev->trim_in_progress = true;
	}

	ftl_property_register(dev, "superblock_version", &dev->sb->header.version,
			      sizeof(dev->sb->header.version), NULL, NULL,
			      ftl_property_dump_uint64, NULL, NULL, false);

	/* Clear the limit applications as they're incremented incorrectly by
	 * the initialization code.
	 */
	memset(dev->stats.limits, 0, sizeof(dev->stats.limits));
	dev->initialized = 1;
	dev->sb_shm->shm_ready = true;

	ftl_l2p_resume(dev);
	ftl_reloc_resume(dev->reloc);
	ftl_writer_resume(&dev->writer_user);
	ftl_writer_resume(&dev->writer_gc);
	ftl_nv_cache_resume(&dev->nv_cache);

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_start_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->core_poller = SPDK_POLLER_REGISTER(ftl_core_poller, dev, 0);
	if (!dev->core_poller) {
		FTL_ERRLOG(dev, "Unable to register core poller\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_stop_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	dev->halt = true;

	if (dev->core_poller) {
		ftl_mngt_continue_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_dev_dump_bands(dev);
	ftl_dev_dump_stats(dev);
	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_init_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *valid_map_md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_VALID_MAP];

	dev->valid_map = ftl_bitmap_create(ftl_md_get_buffer(valid_map_md),
					   ftl_md_get_buffer_size(valid_map_md));
	if (!dev->valid_map) {
		FTL_ERRLOG(dev, "Failed to create valid map\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

void
ftl_mngt_deinit_vld_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	if (dev->valid_map) {
		ftl_bitmap_destroy(dev->valid_map);
		dev->valid_map = NULL;
	}

	ftl_mngt_next_step(mngt);
}
void
ftl_mngt_init_trim_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	uint64_t num_l2p_pages = spdk_divide_round_up(dev->num_lbas, dev->layout.l2p.lbas_in_page);
	uint64_t map_blocks = ftl_bitmap_bits_to_blocks(num_l2p_pages);

	dev->trim_map_md = ftl_md_create(dev,
					 map_blocks,
					 0,
					 "trim_bitmap",
					 ftl_md_create_shm_flags(dev), NULL);

	if (!dev->trim_map_md) {
		FTL_ERRLOG(dev, "Failed to create trim bitmap md\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	dev->trim_map = ftl_bitmap_create(ftl_md_get_buffer(dev->trim_map_md),
					  ftl_md_get_buffer_size(dev->trim_map_md));

	if (!dev->trim_map) {
		FTL_ERRLOG(dev, "Failed to create trim map\n");
		ftl_mngt_fail_step(mngt);
		return;
	}

	ftl_mngt_next_step(mngt);
}

static void
trim_clear_cb(struct spdk_ftl_dev *dev, struct ftl_md *md, int status)
{
	struct ftl_mngt_process *mngt = md->owner.cb_ctx;

	if (status) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

void
ftl_mngt_trim_metadata_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_MD];

	md->cb = trim_clear_cb;
	md->owner.cb_ctx = mngt;
	ftl_md_clear(md, 0, NULL);
}

void
ftl_mngt_trim_log_clear(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_md *md = dev->layout.md[FTL_LAYOUT_REGION_TYPE_TRIM_LOG];

	md->cb = trim_clear_cb;
	md->owner.cb_ctx = mngt;
	ftl_md_clear(md, 0, NULL);
}

void
ftl_mngt_deinit_trim_map(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	ftl_bitmap_destroy(dev->trim_map);
	dev->trim_map = NULL;

	ftl_md_destroy(dev->trim_map_md, ftl_md_destroy_shm_flags(dev));
	dev->trim_map_md = NULL;

	ftl_mngt_next_step(mngt);
}

struct ftl_mngt_property_caller_ctx {
	struct spdk_ftl_dev *dev;
	struct spdk_jsonrpc_request *request;
	spdk_ftl_fn cb_fn;
	void *cb_arg;
	struct spdk_thread *cb_thread;
	const char *property;
	const char *value;
	size_t value_size;
};

static void
ftl_get_properties_cb(void *arg)
{
	struct ftl_mngt_property_caller_ctx *cctx = arg;

	cctx->cb_fn(cctx->cb_arg, 0);
	free(cctx);
}

static void
ftl_get_properties_msg(void *arg)
{
	struct ftl_mngt_property_caller_ctx *cctx = arg;
	int rc;

	ftl_property_dump(cctx->dev, cctx->request);
	rc = spdk_thread_send_msg(cctx->cb_thread, ftl_get_properties_cb, cctx);
	ftl_bug(rc);
}

int
spdk_ftl_get_properties(struct spdk_ftl_dev *dev, struct spdk_jsonrpc_request *request,
			spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;
	struct ftl_mngt_property_caller_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->dev = dev;
	ctx->request = request;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->cb_thread = spdk_get_thread();

	rc = spdk_thread_send_msg(dev->core_thread, ftl_get_properties_msg, ctx);
	if (rc) {
		free(ctx);
		return rc;
	}

	return 0;
}

struct ftl_set_property_process_ctx {
	void *value;
	size_t value_size;
};

static void
ftl_mngt_set_property_decode(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_set_property_process_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_mngt_property_caller_ctx *cctx = ftl_mngt_get_caller_ctx(mngt);

	if (ftl_property_decode(dev, cctx->property, cctx->value, cctx->value_size,
				&pctx->value, &pctx->value_size)) {
		ftl_mngt_fail_step(mngt);
	} else {
		ftl_mngt_next_step(mngt);
	}
}

static void
ftl_mngt_set_property(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_set_property_process_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	struct ftl_mngt_property_caller_ctx *cctx = ftl_mngt_get_caller_ctx(mngt);

	if (ftl_property_set(dev, mngt, cctx->property, pctx->value, pctx->value_size)) {
		ftl_mngt_fail_step(mngt);
	}
}

static void
ftl_mngt_set_property_cleanup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt)
{
	struct ftl_set_property_process_ctx *pctx = ftl_mngt_get_process_ctx(mngt);
	free(pctx->value);
	pctx->value = NULL;
	pctx->value_size = 0;
	ftl_mngt_next_step(mngt);
}

static const struct ftl_mngt_process_desc desc_set_property = {
	.name = "Set FTL property",
	.ctx_size = sizeof(struct ftl_set_property_process_ctx),
	.steps = {
		{
			.name = "Decode property",
			.action = ftl_mngt_set_property_decode,
			.cleanup = ftl_mngt_set_property_cleanup
		},
		{
			.name = "Set property",
			.action = ftl_mngt_set_property,
			.cleanup = ftl_mngt_set_property_cleanup
		},
		{
			.name = "Property setting cleanup",
			.action = ftl_mngt_set_property_cleanup,
		},
		{}
	}
};

static void
ftl_mngt_property_caller_cb(struct spdk_ftl_dev *dev, void *ctx, int status)
{
	struct ftl_mngt_property_caller_ctx *cctx = ctx;

	cctx->cb_fn(cctx->cb_arg, status);
	free(cctx);
}

int
spdk_ftl_set_property(struct spdk_ftl_dev *dev,
		      const char *property, const char *value, size_t value_size,
		      spdk_ftl_fn cb_fn, void *cb_arg)
{
	int rc;
	struct ftl_mngt_property_caller_ctx *cctx = calloc(1, sizeof(*cctx));

	if (cctx == NULL) {
		return -EAGAIN;
	}
	cctx->cb_fn = cb_fn;
	cctx->cb_arg = cb_arg;
	cctx->property = property;
	cctx->value = value;
	cctx->value_size = value_size;

	rc = ftl_mngt_process_execute(dev, &desc_set_property, ftl_mngt_property_caller_cb, cctx);
	if (rc) {
		free(cctx);
	}

	return rc;
}
