/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "vbdev_crypto.h"

#include "spdk_internal/assert.h"
#include "spdk/thread.h"
#include "spdk/bdev_module.h"
#include "spdk/likely.h"

/* This namespace UUID was generated using uuid_generate() method. */
#define BDEV_CRYPTO_NAMESPACE_UUID "078e3cf7-f4b4-4545-b2c3-d40045a64ae2"

struct bdev_names {
	struct vbdev_crypto_opts	*opts;
	TAILQ_ENTRY(bdev_names)		link;
};

/* List of crypto_bdev names and their base bdevs via configuration file. */
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

struct vbdev_crypto {
	struct spdk_bdev		*base_bdev;		/* the thing we're attaching to */
	struct spdk_bdev_desc		*base_desc;		/* its descriptor we get from open */
	struct spdk_bdev		crypto_bdev;		/* the crypto virtual bdev */
	struct vbdev_crypto_opts	*opts;			/* crypto options such as names and DEK */
	TAILQ_ENTRY(vbdev_crypto)	link;
	struct spdk_thread		*thread;		/* thread where base device is opened */
};

/* List of virtual bdevs and associated info for each. We keep the device friendly name here even
 * though its also in the device struct because we use it early on.
 */
static TAILQ_HEAD(, vbdev_crypto) g_vbdev_crypto = TAILQ_HEAD_INITIALIZER(g_vbdev_crypto);

/* The crypto vbdev channel struct. It is allocated and freed on my behalf by the io channel code.
 * We store things in here that are needed on per thread basis like the base_channel for this thread.
 */
struct crypto_io_channel {
	struct spdk_io_channel		*base_ch;	/* IO channel of base device */
	struct spdk_io_channel		*accel_channel;	/* Accel engine channel used for crypto ops */
	struct spdk_accel_crypto_key	*crypto_key;
};

enum crypto_io_resubmit_state {
	CRYPTO_IO_DECRYPT_DONE,	/* Appended decrypt, need to read */
	CRYPTO_IO_ENCRYPT_DONE,	/* Need to write */
};

/* This is the crypto per IO context that the bdev layer allocates for us opaquely and attaches to
 * each IO for us.
 */
struct crypto_bdev_io {
	struct crypto_io_channel *crypto_ch;		/* need to store for crypto completion handling */
	struct vbdev_crypto *crypto_bdev;		/* the crypto node struct associated with this IO */
	/* Used for the single contiguous buffer that serves as the crypto destination target for writes */
	uint64_t aux_num_blocks;			/* num of blocks for the contiguous buffer */
	uint64_t aux_offset_blocks;			/* block offset on media */
	void *aux_buf_raw;				/* raw buffer that the bdev layer gave us for write buffer */
	struct iovec aux_buf_iov;			/* iov representing aligned contig write buffer */
	struct spdk_memory_domain *aux_domain;		/* memory domain of the aux buf */
	void *aux_domain_ctx;				/* memory domain ctx of the aux buf */
	struct spdk_accel_sequence *seq;		/* sequence of accel operations */

	/* for bdev_io_wait */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	enum crypto_io_resubmit_state resubmit_state;
};

static void vbdev_crypto_queue_io(struct spdk_bdev_io *bdev_io,
				  enum crypto_io_resubmit_state state);
static void _complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg);
static void vbdev_crypto_examine(struct spdk_bdev *bdev);
static int vbdev_crypto_claim(const char *bdev_name);
static void vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);

static void
crypto_io_fail(struct crypto_bdev_io *crypto_io)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(crypto_io);
	struct crypto_io_channel *crypto_ch = crypto_io->crypto_ch;

	if (crypto_io->aux_buf_raw) {
		spdk_accel_put_buf(crypto_ch->accel_channel, crypto_io->aux_buf_raw,
				   crypto_io->aux_domain, crypto_io->aux_domain_ctx);
	}

	/* This function can only be used to fail an IO that hasn't been sent to the base bdev,
	 * otherwise accel sequence might have already been executed/aborted. */
	spdk_accel_sequence_abort(crypto_io->seq);
	spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
crypto_write(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev_ext_io_opts opts = {};
	int rc;

	opts.size = sizeof(opts);
	opts.accel_sequence = crypto_io->seq;
	opts.memory_domain = crypto_io->aux_domain;
	opts.memory_domain_ctx = crypto_io->aux_domain_ctx;

	/* Write the encrypted data. */
	rc = spdk_bdev_writev_blocks_ext(crypto_bdev->base_desc, crypto_ch->base_ch,
					 &crypto_io->aux_buf_iov, 1, crypto_io->aux_offset_blocks,
					 crypto_io->aux_num_blocks, _complete_internal_io,
					 bdev_io, &opts);
	if (spdk_unlikely(rc != 0)) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			vbdev_crypto_queue_io(bdev_io, CRYPTO_IO_ENCRYPT_DONE);
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
			crypto_io_fail(crypto_io);
		}
	}
}

/* We're either encrypting on the way down or decrypting on the way back. */
static void
crypto_encrypt(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	uint32_t blocklen = crypto_io->crypto_bdev->crypto_bdev.blocklen;
	uint64_t total_length;
	uint64_t alignment;
	void *aux_buf = crypto_io->aux_buf_raw;
	int rc;

	/* For encryption, we need to prepare a single contiguous buffer as the encryption
	 * destination, we'll then pass that along for the write after encryption is done.
	 * This is done to avoiding encrypting the provided write buffer which may be
	 * undesirable in some use cases.
	 */
	total_length = bdev_io->u.bdev.num_blocks * blocklen;
	alignment = spdk_bdev_get_buf_align(&crypto_io->crypto_bdev->crypto_bdev);
	crypto_io->aux_buf_iov.iov_len = total_length;
	crypto_io->aux_buf_iov.iov_base  = (void *)(((uintptr_t)aux_buf + (alignment - 1)) & ~
					   (alignment - 1));
	crypto_io->aux_offset_blocks = bdev_io->u.bdev.offset_blocks;
	crypto_io->aux_num_blocks = bdev_io->u.bdev.num_blocks;

	rc = spdk_accel_append_encrypt(&crypto_io->seq, crypto_ch->accel_channel,
				       crypto_ch->crypto_key, &crypto_io->aux_buf_iov, 1,
				       crypto_io->aux_domain, crypto_io->aux_domain_ctx,
				       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.memory_domain,
				       bdev_io->u.bdev.memory_domain_ctx,
				       bdev_io->u.bdev.offset_blocks, blocklen,
				       NULL, NULL);
	if (spdk_unlikely(rc != 0)) {
		spdk_accel_put_buf(crypto_ch->accel_channel, crypto_io->aux_buf_raw,
				   crypto_io->aux_domain, crypto_io->aux_domain_ctx);
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
			crypto_io_fail(crypto_io);
		}

		return;
	}

	crypto_write(crypto_ch, bdev_io);
}

static void
_complete_internal_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)orig_io->driver_ctx;
	struct crypto_io_channel *crypto_ch = crypto_io->crypto_ch;

	if (crypto_io->aux_buf_raw) {
		spdk_accel_put_buf(crypto_ch->accel_channel, crypto_io->aux_buf_raw,
				   crypto_io->aux_domain, crypto_io->aux_domain_ctx);
	}

	spdk_bdev_io_complete_base_io_status(orig_io, bdev_io);
	spdk_bdev_free_io(bdev_io);
}

static void crypto_read(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io);

static void
vbdev_crypto_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;

	switch (crypto_io->resubmit_state) {
	case CRYPTO_IO_ENCRYPT_DONE:
		crypto_write(crypto_io->crypto_ch, bdev_io);
		break;
	case CRYPTO_IO_DECRYPT_DONE:
		crypto_read(crypto_io->crypto_ch, bdev_io);
		break;
	default:
		SPDK_UNREACHABLE();
	}
}

static void
vbdev_crypto_queue_io(struct spdk_bdev_io *bdev_io, enum crypto_io_resubmit_state state)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc;

	crypto_io->bdev_io_wait.bdev = bdev_io->bdev;
	crypto_io->bdev_io_wait.cb_fn = vbdev_crypto_resubmit_io;
	crypto_io->bdev_io_wait.cb_arg = bdev_io;
	crypto_io->resubmit_state = state;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, crypto_io->crypto_ch->base_ch,
				     &crypto_io->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_crypto_queue_io, rc=%d.\n", rc);
		crypto_io_fail(crypto_io);
	}
}

static void
crypto_read(struct crypto_io_channel *crypto_ch, struct spdk_bdev_io *bdev_io)
{
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct spdk_bdev_ext_io_opts opts = {};
	int rc;

	opts.size = sizeof(opts);
	opts.accel_sequence = crypto_io->seq;
	opts.memory_domain = bdev_io->u.bdev.memory_domain;
	opts.memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;

	rc = spdk_bdev_readv_blocks_ext(crypto_bdev->base_desc, crypto_ch->base_ch,
					bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
					_complete_internal_io, bdev_io, &opts);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			vbdev_crypto_queue_io(bdev_io, CRYPTO_IO_DECRYPT_DONE);
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
			crypto_io_fail(crypto_io);
		}
	}
}

/* Callback for getting a buf from the bdev pool in the event that the caller passed
 * in NULL, we need to own the buffer so it doesn't get freed by another vbdev module
 * beneath us before we're done with it.
 */
static void
crypto_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		       bool success)
{
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	uint32_t blocklen = crypto_io->crypto_bdev->crypto_bdev.blocklen;
	int rc;

	if (!success) {
		crypto_io_fail(crypto_io);
		return;
	}

	rc = spdk_accel_append_decrypt(&crypto_io->seq, crypto_ch->accel_channel,
				       crypto_ch->crypto_key,
				       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.memory_domain,
				       bdev_io->u.bdev.memory_domain_ctx,
				       bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				       bdev_io->u.bdev.memory_domain,
				       bdev_io->u.bdev.memory_domain_ctx,
				       bdev_io->u.bdev.offset_blocks, blocklen,
				       NULL, NULL);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
			crypto_io_fail(crypto_io);
		}

		return;
	}

	crypto_read(crypto_ch, bdev_io);
}

/* Called when someone submits IO to this crypto vbdev. For IO's not relevant to crypto,
 * we're simply passing it on here via SPDK IO calls which in turn allocate another bdev IO
 * and call our cpl callback provided below along with the original bdev_io so that we can
 * complete it once this IO completes. For crypto operations, we'll either encrypt it first
 * (writes) then call back into bdev to submit it or we'll submit a read and then catch it
 * on the way back for decryption.
 */
static void
vbdev_crypto_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_crypto *crypto_bdev = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_crypto,
					   crypto_bdev);
	struct crypto_io_channel *crypto_ch = spdk_io_channel_get_ctx(ch);
	struct crypto_bdev_io *crypto_io = (struct crypto_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	memset(crypto_io, 0, sizeof(struct crypto_bdev_io));
	crypto_io->crypto_bdev = crypto_bdev;
	crypto_io->crypto_ch = crypto_ch;
	crypto_io->seq = bdev_io->u.bdev.accel_sequence;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, crypto_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		/* For encryption we don't want to encrypt the data in place as the host isn't
		 * expecting us to mangle its data buffers so we need to encrypt into the aux accel
		 * buffer, then we can use that as the source for the disk data transfer.
		 */
		rc = spdk_accel_get_buf(crypto_ch->accel_channel,
					bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen,
					&crypto_io->aux_buf_raw, &crypto_io->aux_domain,
					&crypto_io->aux_domain_ctx);
		if (rc == 0) {
			crypto_encrypt(crypto_ch, bdev_io);
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(crypto_bdev->base_desc, crypto_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(crypto_bdev->base_desc, crypto_ch->base_ch,
				     _complete_internal_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	default:
		SPDK_ERRLOG("crypto: unknown I/O type %d\n", bdev_io->type);
		rc = -EINVAL;
		break;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			SPDK_DEBUGLOG(vbdev_crypto, "No memory, queue the IO.\n");
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_NOMEM);
		} else {
			SPDK_ERRLOG("Failed to submit bdev_io!\n");
			crypto_io_fail(crypto_io);
		}
	}
}

/* We'll just call the base bdev and let it answer except for WZ command which
 * we always say we don't support so that the bdev layer will actually send us
 * real writes that we can encrypt.
 */
static bool
vbdev_crypto_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return spdk_bdev_io_type_supported(crypto_bdev->base_bdev, io_type);
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	/* Force the bdev layer to issue actual writes of zeroes so we can
	 * encrypt them as regular writes.
	 */
	default:
		return false;
	}
}

/* Callback for unregistering the IO device. */
static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_crypto *crypto_bdev = io_device;

	/* Done with this crypto_bdev. */
	crypto_bdev->opts = NULL;

	spdk_bdev_destruct_done(&crypto_bdev->crypto_bdev, 0);
	free(crypto_bdev->crypto_bdev.name);
	free(crypto_bdev);
}

/* Wrapper for the bdev close operation. */
static void
_vbdev_crypto_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

/* Called after we've unregistered following a hot remove callback.
 * Our finish entry point will be called next.
 */
static int
vbdev_crypto_destruct(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* Remove this device from the internal list */
	TAILQ_REMOVE(&g_vbdev_crypto, crypto_bdev, link);

	/* Unclaim the underlying bdev. */
	spdk_bdev_module_release_bdev(crypto_bdev->base_bdev);

	/* Close the underlying bdev on its same opened thread. */
	if (crypto_bdev->thread && crypto_bdev->thread != spdk_get_thread()) {
		spdk_thread_send_msg(crypto_bdev->thread, _vbdev_crypto_destruct, crypto_bdev->base_desc);
	} else {
		spdk_bdev_close(crypto_bdev->base_desc);
	}

	/* Unregister the io_device. */
	spdk_io_device_unregister(crypto_bdev, _device_unregister_cb);

	return 1;
}

/* We supplied this as an entry point for upper layers who want to communicate to this
 * bdev.  This is how they get a channel. We are passed the same context we provided when
 * we created our crypto vbdev in examine() which, for this bdev, is the address of one of
 * our context nodes. From here we'll ask the SPDK channel code to fill out our channel
 * struct and we'll keep it in our crypto node.
 */
static struct spdk_io_channel *
vbdev_crypto_get_io_channel(void *ctx)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	/* The IO channel code will allocate a channel for us which consists of
	 * the SPDK channel structure plus the size of our crypto_io_channel struct
	 * that we passed in when we registered our IO device. It will then call
	 * our channel create callback to populate any elements that we need to
	 * update.
	 */
	return spdk_get_io_channel(crypto_bdev);
}

/* This is the output for bdev_get_bdevs() for this vbdev */
static int
vbdev_crypto_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev = (struct vbdev_crypto *)ctx;

	spdk_json_write_name(w, "crypto");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
	spdk_json_write_named_string(w, "key_name", crypto_bdev->opts->key->param.key_name);
	spdk_json_write_object_end(w);

	return 0;
}

static int
vbdev_crypto_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_crypto *crypto_bdev;

	TAILQ_FOREACH(crypto_bdev, &g_vbdev_crypto, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_crypto_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(crypto_bdev->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&crypto_bdev->crypto_bdev));
		spdk_json_write_named_string(w, "key_name", crypto_bdev->opts->key->param.key_name);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

/* We provide this callback for the SPDK channel code to create a channel using
 * the channel struct we provided in our module get_io_channel() entry point. Here
 * we get and save off an underlying base channel of the device below us so that
 * we can communicate with the base bdev on a per channel basis. We also register the
 * poller used to complete crypto operations from the device.
 */
static int
crypto_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;
	struct vbdev_crypto *crypto_bdev = io_device;

	crypto_ch->base_ch = spdk_bdev_get_io_channel(crypto_bdev->base_desc);
	if (crypto_ch->base_ch == NULL) {
		SPDK_ERRLOG("Failed to get base bdev IO channel (bdev: %s)\n",
			    crypto_bdev->crypto_bdev.name);
		return -ENOMEM;
	}

	crypto_ch->accel_channel = spdk_accel_get_io_channel();
	if (crypto_ch->accel_channel == NULL) {
		SPDK_ERRLOG("Failed to get accel IO channel (bdev: %s)\n",
			    crypto_bdev->crypto_bdev.name);
		spdk_put_io_channel(crypto_ch->base_ch);
		return -ENOMEM;
	}

	crypto_ch->crypto_key = crypto_bdev->opts->key;

	return 0;
}

/* We provide this callback for the SPDK channel code to destroy a channel
 * created with our create callback. We just need to undo anything we did
 * when we created.
 */
static void
crypto_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct crypto_io_channel *crypto_ch = ctx_buf;

	spdk_put_io_channel(crypto_ch->base_ch);
	spdk_put_io_channel(crypto_ch->accel_channel);
}

/* Create the association from the bdev and vbdev name and insert
 * on the global list. */
static int
vbdev_crypto_insert_name(struct vbdev_crypto_opts *opts, struct bdev_names **out)
{
	struct bdev_names *name;

	assert(opts);
	assert(out);

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(opts->vbdev_name, name->opts->vbdev_name) == 0) {
			SPDK_ERRLOG("Crypto bdev %s already exists\n", opts->vbdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		SPDK_ERRLOG("Failed to allocate memory for bdev_names.\n");
		return -ENOMEM;
	}

	name->opts = opts;
	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);
	*out = name;

	return 0;
}

void
free_crypto_opts(struct vbdev_crypto_opts *opts)
{
	free(opts->bdev_name);
	free(opts->vbdev_name);
	free(opts);
}

static void
vbdev_crypto_delete_name(struct bdev_names *name)
{
	TAILQ_REMOVE(&g_bdev_names, name, link);
	if (name->opts) {
		if (name->opts->key_owner && name->opts->key) {
			spdk_accel_crypto_key_destroy(name->opts->key);
		}
		free_crypto_opts(name->opts);
		name->opts = NULL;
	}
	free(name);
}

/* RPC entry point for crypto creation. */
int
create_crypto_disk(struct vbdev_crypto_opts *opts)
{
	struct bdev_names *name = NULL;
	int rc;

	rc = vbdev_crypto_insert_name(opts, &name);
	if (rc) {
		return rc;
	}

	rc = vbdev_crypto_claim(opts->bdev_name);
	if (rc == -ENODEV) {
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	if (rc) {
		assert(name != NULL);
		/* In case of error we let the caller function to deallocate @opts
		 * since it is its responsibility. Setting name->opts = NULL let's
		 * vbdev_crypto_delete_name() know it does not have to do anything
		 * about @opts.
		 */
		name->opts = NULL;
		vbdev_crypto_delete_name(name);
	}
	return rc;
}

/* Called at driver init time, parses config file to prepare for examine calls,
 * also fully initializes the crypto drivers.
 */
static int
vbdev_crypto_init(void)
{
	return 0;
}

/* Called when the entire module is being torn down. */
static void
vbdev_crypto_finish(void)
{
	struct bdev_names *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		vbdev_crypto_delete_name(name);
	}
}

/* During init we'll be asked how much memory we'd like passed to us
 * in bev_io structures as context. Here's where we specify how
 * much context we want per IO.
 */
static int
vbdev_crypto_get_ctx_size(void)
{
	return sizeof(struct crypto_bdev_io);
}

static void
vbdev_crypto_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_crypto *crypto_bdev, *tmp;

	TAILQ_FOREACH_SAFE(crypto_bdev, &g_vbdev_crypto, link, tmp) {
		if (bdev_find == crypto_bdev->base_bdev) {
			spdk_bdev_unregister(&crypto_bdev->crypto_bdev, NULL, NULL);
		}
	}
}

/* Called when the underlying base bdev triggers asynchronous event such as bdev removal. */
static void
vbdev_crypto_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_crypto_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static int
vbdev_crypto_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct vbdev_crypto *crypto_bdev = ctx;
	int num_domains;

	/* Report base bdev's memory domains plus accel memory domain */
	num_domains = spdk_bdev_get_memory_domains(crypto_bdev->base_bdev, domains, array_size);
	if (domains != NULL && num_domains < array_size) {
		domains[num_domains] = spdk_accel_get_memory_domain();
	}

	return num_domains + 1;
}

static bool
vbdev_crypto_sequence_supported(void *ctx, enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;
	default:
		return false;
	}
}

/* When we register our bdev this is how we specify our entry points. */
static const struct spdk_bdev_fn_table vbdev_crypto_fn_table = {
	.destruct			= vbdev_crypto_destruct,
	.submit_request			= vbdev_crypto_submit_request,
	.io_type_supported		= vbdev_crypto_io_type_supported,
	.get_io_channel			= vbdev_crypto_get_io_channel,
	.dump_info_json			= vbdev_crypto_dump_info_json,
	.get_memory_domains		= vbdev_crypto_get_memory_domains,
	.accel_sequence_supported	= vbdev_crypto_sequence_supported,
};

static struct spdk_bdev_module crypto_if = {
	.name = "crypto",
	.module_init = vbdev_crypto_init,
	.get_ctx_size = vbdev_crypto_get_ctx_size,
	.examine_config = vbdev_crypto_examine,
	.module_fini = vbdev_crypto_finish,
	.config_json = vbdev_crypto_config_json
};

SPDK_BDEV_MODULE_REGISTER(crypto, &crypto_if)

static int
vbdev_crypto_claim(const char *bdev_name)
{
	struct bdev_names *name;
	struct vbdev_crypto *vbdev;
	struct spdk_bdev *bdev;
	struct spdk_iobuf_opts iobuf_opts;
	struct spdk_accel_operation_exec_ctx opctx = {};
	struct spdk_uuid ns_uuid;
	int rc = 0;

	spdk_uuid_parse(&ns_uuid, BDEV_CRYPTO_NAMESPACE_UUID);

	/* Limit the max IO size by some reasonable value. Since in write operation we use aux buffer,
	 * let's set the limit to the large_bufsize value */
	spdk_iobuf_get_opts(&iobuf_opts, sizeof(iobuf_opts));

	/* Check our list of names from config versus this bdev and if
	 * there's a match, create the crypto_bdev & bdev accordingly.
	 */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->opts->bdev_name, bdev_name) != 0) {
			continue;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Match on %s\n", bdev_name);

		vbdev = calloc(1, sizeof(struct vbdev_crypto));
		if (!vbdev) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev.\n");
			return -ENOMEM;
		}
		vbdev->crypto_bdev.product_name = "crypto";

		vbdev->crypto_bdev.name = strdup(name->opts->vbdev_name);
		if (!vbdev->crypto_bdev.name) {
			SPDK_ERRLOG("Failed to allocate memory for crypto_bdev name.\n");
			rc = -ENOMEM;
			goto error_bdev_name;
		}

		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_crypto_base_bdev_event_cb,
					NULL, &vbdev->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("Failed to open bdev %s: error %d\n", bdev_name, rc);
			}
			goto error_open;
		}

		bdev = spdk_bdev_desc_get_bdev(vbdev->base_desc);
		vbdev->base_bdev = bdev;

		vbdev->crypto_bdev.write_cache = bdev->write_cache;
		vbdev->crypto_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		vbdev->crypto_bdev.max_rw_size = spdk_min(
				bdev->max_rw_size ? bdev->max_rw_size : UINT32_MAX,
				iobuf_opts.large_bufsize / bdev->blocklen);

		opctx.size = SPDK_SIZEOF(&opctx, block_size);
		opctx.block_size = bdev->blocklen;
		vbdev->crypto_bdev.required_alignment =
			spdk_max(bdev->required_alignment,
				 spdk_max(spdk_accel_get_buf_align(SPDK_ACCEL_OPC_ENCRYPT, &opctx),
					  spdk_accel_get_buf_align(SPDK_ACCEL_OPC_DECRYPT, &opctx)));

		vbdev->crypto_bdev.blocklen = bdev->blocklen;
		vbdev->crypto_bdev.blockcnt = bdev->blockcnt;

		/* This is the context that is passed to us when the bdev
		 * layer calls in so we'll save our crypto_bdev node here.
		 */
		vbdev->crypto_bdev.ctxt = vbdev;
		vbdev->crypto_bdev.fn_table = &vbdev_crypto_fn_table;
		vbdev->crypto_bdev.module = &crypto_if;

		/* Assign crypto opts from the name. The pointer is valid up to the point
		 * the module is unloaded and all names removed from the list. */
		vbdev->opts = name->opts;

		/* Generate UUID based on namespace UUID + base bdev UUID */
		rc = spdk_uuid_generate_sha1(&vbdev->crypto_bdev.uuid, &ns_uuid,
					     (const char *)&vbdev->base_bdev->uuid, sizeof(struct spdk_uuid));
		if (rc) {
			SPDK_ERRLOG("Unable to generate new UUID for crypto bdev\n");
			goto error_uuid;
		}

		TAILQ_INSERT_TAIL(&g_vbdev_crypto, vbdev, link);

		spdk_io_device_register(vbdev, crypto_bdev_ch_create_cb, crypto_bdev_ch_destroy_cb,
					sizeof(struct crypto_io_channel), vbdev->crypto_bdev.name);

		/* Save the thread where the base device is opened */
		vbdev->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, vbdev->base_desc, vbdev->crypto_bdev.module);
		if (rc) {
			SPDK_ERRLOG("Failed to claim bdev %s\n", spdk_bdev_get_name(bdev));
			goto error_claim;
		}

		rc = spdk_bdev_register(&vbdev->crypto_bdev);
		if (rc < 0) {
			SPDK_ERRLOG("Failed to register vbdev: error %d\n", rc);
			rc = -EINVAL;
			goto error_bdev_register;
		}
		SPDK_DEBUGLOG(vbdev_crypto, "Registered io_device and virtual bdev for: %s\n",
			      vbdev->opts->vbdev_name);
		break;
	}

	return rc;

	/* Error cleanup paths. */
error_bdev_register:
	spdk_bdev_module_release_bdev(vbdev->base_bdev);
error_claim:
	TAILQ_REMOVE(&g_vbdev_crypto, vbdev, link);
	spdk_io_device_unregister(vbdev, NULL);
error_uuid:
	spdk_bdev_close(vbdev->base_desc);
error_open:
	free(vbdev->crypto_bdev.name);
error_bdev_name:
	free(vbdev);

	return rc;
}

struct crypto_delete_disk_ctx {
	spdk_delete_crypto_complete cb_fn;
	void *cb_arg;
	char *bdev_name;
};

static void
delete_crypto_disk_bdev_name(void *ctx, int rc)
{
	struct bdev_names *name;
	struct crypto_delete_disk_ctx *disk_ctx = ctx;

	/* Remove the association (vbdev, bdev) from g_bdev_names. This is required so that the
	 * vbdev does not get re-created if the same bdev is constructed at some other time,
	 * unless the underlying bdev was hot-removed. */
	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->opts->vbdev_name, disk_ctx->bdev_name) == 0) {
			vbdev_crypto_delete_name(name);
			break;
		}
	}

	disk_ctx->cb_fn(disk_ctx->cb_arg, rc);

	free(disk_ctx->bdev_name);
	free(disk_ctx);
}

/* RPC entry for deleting a crypto vbdev. */
void
delete_crypto_disk(const char *bdev_name, spdk_delete_crypto_complete cb_fn,
		   void *cb_arg)
{
	int rc;
	struct crypto_delete_disk_ctx *ctx;

	ctx = calloc(1, sizeof(struct crypto_delete_disk_ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to allocate delete crypto disk ctx\n");
		cb_fn(cb_arg, -ENOMEM);
		return;
	}

	ctx->bdev_name = strdup(bdev_name);
	if (!ctx->bdev_name) {
		SPDK_ERRLOG("Failed to copy bdev_name\n");
		free(ctx);
		cb_fn(cb_arg, -ENOMEM);
		return;
	}
	ctx->cb_arg = cb_arg;
	ctx->cb_fn = cb_fn;
	/* Some cleanup happens in the destruct callback. */
	rc = spdk_bdev_unregister_by_name(bdev_name, &crypto_if, delete_crypto_disk_bdev_name, ctx);
	if (rc != 0) {
		SPDK_ERRLOG("Encountered an error during bdev unregistration\n");
		cb_fn(cb_arg, rc);
		free(ctx->bdev_name);
		free(ctx);
	}
}

/* Because we specified this function in our crypto bdev function table when we
 * registered our crypto bdev, we'll get this call anytime a new bdev shows up.
 * Here we need to decide if we care about it and if so what to do. We
 * parsed the config file at init so we check the new bdev against the list
 * we built up at that time and if the user configured us to attach to this
 * bdev, here's where we do it.
 */
static void
vbdev_crypto_examine(struct spdk_bdev *bdev)
{
	vbdev_crypto_claim(spdk_bdev_get_name(bdev));
	spdk_bdev_module_examine_done(&crypto_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_crypto)
