/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/accel_module.h"

#include "accel_internal.h"

#include "spdk/dma.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/crc32.h"
#include "spdk/util.h"
#include "spdk/hexlify.h"
#include "spdk/string.h"

/* Accelerator Framework: The following provides a top level
 * generic API for the accelerator functions defined here. Modules,
 * such as the one in /module/accel/ioat, supply the implementation
 * with the exception of the pure software implementation contained
 * later in this file.
 */

#define ALIGN_4K			0x1000
#define MAX_TASKS_PER_CHANNEL		0x800
#define ACCEL_SMALL_CACHE_SIZE		128
#define ACCEL_LARGE_CACHE_SIZE		16
/* Set MSB, so we don't return NULL pointers as buffers */
#define ACCEL_BUFFER_BASE		((void *)(1ull << 63))
#define ACCEL_BUFFER_OFFSET_MASK	((uintptr_t)ACCEL_BUFFER_BASE - 1)

#define ACCEL_CRYPTO_TWEAK_MODE_DEFAULT	SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA

struct accel_module {
	struct spdk_accel_module_if	*module;
	bool				supports_memory_domains;
};

/* Largest context size for all accel modules */
static size_t g_max_accel_module_size = sizeof(struct spdk_accel_task);

static struct spdk_accel_module_if *g_accel_module = NULL;
static spdk_accel_fini_cb g_fini_cb_fn = NULL;
static void *g_fini_cb_arg = NULL;
static bool g_modules_started = false;
static struct spdk_memory_domain *g_accel_domain;

/* Global list of registered accelerator modules */
static TAILQ_HEAD(, spdk_accel_module_if) spdk_accel_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_accel_module_list);

/* Crypto keyring */
static TAILQ_HEAD(, spdk_accel_crypto_key) g_keyring = TAILQ_HEAD_INITIALIZER(g_keyring);
static struct spdk_spinlock g_keyring_spin;

/* Global array mapping capabilities to modules */
static struct accel_module g_modules_opc[SPDK_ACCEL_OPC_LAST] = {};
static char *g_modules_opc_override[SPDK_ACCEL_OPC_LAST] = {};
TAILQ_HEAD(, spdk_accel_driver) g_accel_drivers = TAILQ_HEAD_INITIALIZER(g_accel_drivers);
static struct spdk_accel_driver *g_accel_driver;
static struct spdk_accel_opts g_opts = {
	.small_cache_size = ACCEL_SMALL_CACHE_SIZE,
	.large_cache_size = ACCEL_LARGE_CACHE_SIZE,
	.task_count = MAX_TASKS_PER_CHANNEL,
	.sequence_count = MAX_TASKS_PER_CHANNEL,
	.buf_count = MAX_TASKS_PER_CHANNEL,
};
static struct accel_stats g_stats;
static struct spdk_spinlock g_stats_lock;

static const char *g_opcode_strings[SPDK_ACCEL_OPC_LAST] = {
	"copy", "fill", "dualcast", "compare", "crc32c", "copy_crc32c",
	"compress", "decompress", "encrypt", "decrypt", "xor",
	"dif_verify", "dif_verify_copy", "dif_generate", "dif_generate_copy"
};

enum accel_sequence_state {
	ACCEL_SEQUENCE_STATE_INIT,
	ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF,
	ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF,
	ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF,
	ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF,
	ACCEL_SEQUENCE_STATE_PULL_DATA,
	ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA,
	ACCEL_SEQUENCE_STATE_EXEC_TASK,
	ACCEL_SEQUENCE_STATE_AWAIT_TASK,
	ACCEL_SEQUENCE_STATE_COMPLETE_TASK,
	ACCEL_SEQUENCE_STATE_NEXT_TASK,
	ACCEL_SEQUENCE_STATE_PUSH_DATA,
	ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA,
	ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS,
	ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS,
	ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS,
	ACCEL_SEQUENCE_STATE_ERROR,
	ACCEL_SEQUENCE_STATE_MAX,
};

static const char *g_seq_states[]
__attribute__((unused)) = {
	[ACCEL_SEQUENCE_STATE_INIT] = "init",
	[ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF] = "check-virtbuf",
	[ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF] = "await-virtbuf",
	[ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF] = "check-bouncebuf",
	[ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF] = "await-bouncebuf",
	[ACCEL_SEQUENCE_STATE_PULL_DATA] = "pull-data",
	[ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA] = "await-pull-data",
	[ACCEL_SEQUENCE_STATE_EXEC_TASK] = "exec-task",
	[ACCEL_SEQUENCE_STATE_AWAIT_TASK] = "await-task",
	[ACCEL_SEQUENCE_STATE_COMPLETE_TASK] = "complete-task",
	[ACCEL_SEQUENCE_STATE_NEXT_TASK] = "next-task",
	[ACCEL_SEQUENCE_STATE_PUSH_DATA] = "push-data",
	[ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA] = "await-push-data",
	[ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS] = "driver-exec-tasks",
	[ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS] = "driver-await-tasks",
	[ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS] = "driver-complete-tasks",
	[ACCEL_SEQUENCE_STATE_ERROR] = "error",
	[ACCEL_SEQUENCE_STATE_MAX] = "",
};

#define ACCEL_SEQUENCE_STATE_STRING(s) \
	(((s) >= ACCEL_SEQUENCE_STATE_INIT && (s) < ACCEL_SEQUENCE_STATE_MAX) \
	 ? g_seq_states[s] : "unknown")

struct accel_buffer {
	struct spdk_accel_sequence	*seq;
	void				*buf;
	uint64_t			len;
	struct spdk_iobuf_entry		iobuf;
	spdk_accel_sequence_get_buf_cb	cb_fn;
	void				*cb_ctx;
	SLIST_ENTRY(accel_buffer)	link;
	struct accel_io_channel		*ch;
};

struct accel_io_channel {
	struct spdk_io_channel			*module_ch[SPDK_ACCEL_OPC_LAST];
	struct spdk_io_channel			*driver_channel;
	void					*task_pool_base;
	struct spdk_accel_sequence		*seq_pool_base;
	struct accel_buffer			*buf_pool_base;
	struct spdk_accel_task_aux_data		*task_aux_data_base;
	STAILQ_HEAD(, spdk_accel_task)		task_pool;
	SLIST_HEAD(, spdk_accel_task_aux_data)	task_aux_data_pool;
	SLIST_HEAD(, spdk_accel_sequence)	seq_pool;
	SLIST_HEAD(, accel_buffer)		buf_pool;
	struct spdk_iobuf_channel		iobuf;
	struct accel_stats			stats;
};

TAILQ_HEAD(accel_sequence_tasks, spdk_accel_task);

struct spdk_accel_sequence {
	struct accel_io_channel			*ch;
	struct accel_sequence_tasks		tasks;
	SLIST_HEAD(, accel_buffer)		bounce_bufs;
	int					status;
	/* state uses enum accel_sequence_state */
	uint8_t					state;
	bool					in_process_sequence;
	spdk_accel_completion_cb		cb_fn;
	void					*cb_arg;
	SLIST_ENTRY(spdk_accel_sequence)	link;
};
SPDK_STATIC_ASSERT(sizeof(struct spdk_accel_sequence) == 64, "invalid size");

#define accel_update_stats(ch, event, v) \
	do { \
		(ch)->stats.event += (v); \
	} while (0)

#define accel_update_task_stats(ch, task, event, v) \
	accel_update_stats(ch, operations[(task)->op_code].event, v)

static inline void accel_sequence_task_cb(void *cb_arg, int status);

static inline void
accel_sequence_set_state(struct spdk_accel_sequence *seq, enum accel_sequence_state state)
{
	SPDK_DEBUGLOG(accel, "seq=%p, setting state: %s -> %s\n", seq,
		      ACCEL_SEQUENCE_STATE_STRING(seq->state), ACCEL_SEQUENCE_STATE_STRING(state));
	assert(seq->state != ACCEL_SEQUENCE_STATE_ERROR || state == ACCEL_SEQUENCE_STATE_ERROR);
	seq->state = state;
}

static void
accel_sequence_set_fail(struct spdk_accel_sequence *seq, int status)
{
	accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_ERROR);
	assert(status != 0);
	seq->status = status;
}

int
spdk_accel_get_opc_module_name(enum spdk_accel_opcode opcode, const char **module_name)
{
	if (opcode >= SPDK_ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	if (g_modules_opc[opcode].module) {
		*module_name = g_modules_opc[opcode].module->name;
	} else {
		return -ENOENT;
	}

	return 0;
}

void
_accel_for_each_module(struct module_info *info, _accel_for_each_module_fn fn)
{
	struct spdk_accel_module_if *accel_module;
	enum spdk_accel_opcode opcode;
	int j = 0;

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		for (opcode = 0; opcode < SPDK_ACCEL_OPC_LAST; opcode++) {
			if (accel_module->supports_opcode(opcode)) {
				info->ops[j] = opcode;
				j++;
			}
		}
		info->name = accel_module->name;
		info->num_ops = j;
		fn(info);
		j = 0;
	}
}

const char *
spdk_accel_get_opcode_name(enum spdk_accel_opcode opcode)
{
	if (opcode < SPDK_ACCEL_OPC_LAST) {
		return g_opcode_strings[opcode];
	}

	return NULL;
}

int
spdk_accel_assign_opc(enum spdk_accel_opcode opcode, const char *name)
{
	char *copy;

	if (g_modules_started == true) {
		/* we don't allow re-assignment once things have started */
		return -EINVAL;
	}

	if (opcode >= SPDK_ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	copy = strdup(name);
	if (copy == NULL) {
		return -ENOMEM;
	}

	/* module selection will be validated after the framework starts. */
	free(g_modules_opc_override[opcode]);
	g_modules_opc_override[opcode] = copy;

	return 0;
}

void
spdk_accel_task_complete(struct spdk_accel_task *accel_task, int status)
{
	struct accel_io_channel		*accel_ch = accel_task->accel_ch;
	spdk_accel_completion_cb	cb_fn;
	void				*cb_arg;

	accel_update_task_stats(accel_ch, accel_task, executed, 1);
	accel_update_task_stats(accel_ch, accel_task, num_bytes, accel_task->nbytes);
	if (spdk_unlikely(status != 0)) {
		accel_update_task_stats(accel_ch, accel_task, failed, 1);
	}

	if (accel_task->seq) {
		accel_sequence_task_cb(accel_task->seq, status);
		return;
	}

	cb_fn = accel_task->cb_fn;
	cb_arg = accel_task->cb_arg;

	if (accel_task->has_aux) {
		SLIST_INSERT_HEAD(&accel_ch->task_aux_data_pool, accel_task->aux, link);
		accel_task->aux = NULL;
		accel_task->has_aux = false;
	}

	/* We should put the accel_task into the list firstly in order to avoid
	 * the accel task list is exhausted when there is recursive call to
	 * allocate accel_task in user's call back function (cb_fn)
	 */
	STAILQ_INSERT_HEAD(&accel_ch->task_pool, accel_task, link);

	cb_fn(cb_arg, status);
}

inline static struct spdk_accel_task *
_get_task(struct accel_io_channel *accel_ch, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_task;

	accel_task = STAILQ_FIRST(&accel_ch->task_pool);
	if (spdk_unlikely(accel_task == NULL)) {
		accel_update_stats(accel_ch, retry.task, 1);
		return NULL;
	}

	STAILQ_REMOVE_HEAD(&accel_ch->task_pool, link);
	accel_task->link.stqe_next = NULL;

	accel_task->cb_fn = cb_fn;
	accel_task->cb_arg = cb_arg;
	accel_task->accel_ch = accel_ch;
	accel_task->s.iovs = NULL;
	accel_task->d.iovs = NULL;

	return accel_task;
}

static inline int
accel_submit_task(struct accel_io_channel *accel_ch, struct spdk_accel_task *task)
{
	struct spdk_io_channel *module_ch = accel_ch->module_ch[task->op_code];
	struct spdk_accel_module_if *module = g_modules_opc[task->op_code].module;
	int rc;

	rc = module->submit_tasks(module_ch, task);
	if (spdk_unlikely(rc != 0)) {
		accel_update_task_stats(accel_ch, task, failed, 1);
	}

	return rc;
}

static inline uint64_t
accel_get_iovlen(struct iovec *iovs, uint32_t iovcnt)
{
	uint64_t result = 0;
	uint32_t i;

	for (i = 0; i < iovcnt; ++i) {
		result += iovs[i].iov_len;
	}

	return result;
}

#define ACCEL_TASK_ALLOC_AUX_BUF(task)						\
do {										\
        (task)->aux = SLIST_FIRST(&(task)->accel_ch->task_aux_data_pool);	\
        if (spdk_unlikely(!(task)->aux)) {					\
                SPDK_ERRLOG("Fatal problem, aux data was not allocated\n");	\
                STAILQ_INSERT_HEAD(&(task)->accel_ch->task_pool, (task), link);	\
                assert(0);							\
                return -ENOMEM;							\
        }									\
        SLIST_REMOVE_HEAD(&(task)->accel_ch->task_aux_data_pool, link);		\
        (task)->has_aux = true;							\
} while (0)

/* Accel framework public API for copy function */
int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src,
		       uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->nbytes = nbytes;
	accel_task->op_code = SPDK_ACCEL_OPC_COPY;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for dual cast copy function */
int
spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1,
			   void *dst2, void *src, uint64_t nbytes,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d2.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST2];
	accel_task->d.iovs[0].iov_base = dst1;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->d2.iovs[0].iov_base = dst2;
	accel_task->d2.iovs[0].iov_len = nbytes;
	accel_task->d2.iovcnt = 1;
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->nbytes = nbytes;
	accel_task->op_code = SPDK_ACCEL_OPC_DUALCAST;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for compare function */

int
spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1,
			  void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			  void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->s2.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC2];
	accel_task->s.iovs[0].iov_base = src1;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->s2.iovs[0].iov_base = src2;
	accel_task->s2.iovs[0].iov_len = nbytes;
	accel_task->s2.iovcnt = 1;
	accel_task->nbytes = nbytes;
	accel_task->op_code = SPDK_ACCEL_OPC_COMPARE;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for fill function */
int
spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst,
		       uint8_t fill, uint64_t nbytes,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->nbytes = nbytes;
	memset(&accel_task->fill_pattern, fill, sizeof(uint64_t));
	accel_task->op_code = SPDK_ACCEL_OPC_FILL;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for CRC-32C function */
int
spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *crc_dst,
			 void *src, uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			 void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->nbytes = nbytes;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = SPDK_ACCEL_OPC_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for chained CRC-32C function */
int
spdk_accel_submit_crc32cv(struct spdk_io_channel *ch, uint32_t *crc_dst,
			  struct iovec *iov, uint32_t iov_cnt, uint32_t seed,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	if (iov == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	accel_task->s.iovs = iov;
	accel_task->s.iovcnt = iov_cnt;
	accel_task->nbytes = accel_get_iovlen(iov, iov_cnt);
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = SPDK_ACCEL_OPC_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for copy with CRC-32C function */
int
spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst,
			      void *src, uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
			      spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->s.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->nbytes = nbytes;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = SPDK_ACCEL_OPC_COPY_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

/* Accel framework public API for chained copy + CRC-32C function */
int
spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst,
			       struct iovec *src_iovs, uint32_t iov_cnt, uint32_t *crc_dst,
			       uint32_t seed, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	uint64_t nbytes;

	if (src_iovs == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	nbytes = accel_get_iovlen(src_iovs, iov_cnt);

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = iov_cnt;
	accel_task->nbytes = nbytes;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = SPDK_ACCEL_OPC_COPY_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst, uint64_t nbytes,
			   struct iovec *src_iovs, size_t src_iovcnt, uint32_t *output_size,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->output_size = output_size;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->nbytes = nbytes;
	accel_task->op_code = SPDK_ACCEL_OPC_COMPRESS;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_decompress(struct spdk_io_channel *ch, struct iovec *dst_iovs,
			     size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
			     uint32_t *output_size, spdk_accel_completion_cb cb_fn,
			     void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	accel_task->output_size = output_size;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	accel_task->op_code = SPDK_ACCEL_OPC_DECOMPRESS;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_encrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key || !block_size)) {
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	accel_task->crypto_key = key;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	accel_task->iv = iv;
	accel_task->block_size = block_size;
	accel_task->op_code = SPDK_ACCEL_OPC_ENCRYPT;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_decrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key || !block_size)) {
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	accel_task->crypto_key = key;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	accel_task->iv = iv;
	accel_task->block_size = block_size;
	accel_task->op_code = SPDK_ACCEL_OPC_DECRYPT;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_xor(struct spdk_io_channel *ch, void *dst, void **sources, uint32_t nsrcs,
		      uint64_t nbytes, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (spdk_unlikely(accel_task == NULL)) {
		return -ENOMEM;
	}

	ACCEL_TASK_ALLOC_AUX_BUF(accel_task);

	accel_task->d.iovs = &accel_task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->nsrcs.srcs = sources;
	accel_task->nsrcs.cnt = nsrcs;
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->nbytes = nbytes;
	accel_task->op_code = SPDK_ACCEL_OPC_XOR;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_dif_verify(struct spdk_io_channel *ch,
			     struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
			     const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
			     spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = iovs;
	accel_task->s.iovcnt = iovcnt;
	accel_task->dif.ctx = ctx;
	accel_task->dif.err = err;
	accel_task->dif.num_blocks = num_blocks;
	accel_task->nbytes = num_blocks * ctx->block_size;
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_VERIFY;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_dif_generate(struct spdk_io_channel *ch,
			       struct iovec *iovs, size_t iovcnt, uint32_t num_blocks,
			       const struct spdk_dif_ctx *ctx,
			       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = iovs;
	accel_task->s.iovcnt = iovcnt;
	accel_task->dif.ctx = ctx;
	accel_task->dif.num_blocks = num_blocks;
	accel_task->nbytes = num_blocks * ctx->block_size;
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_GENERATE;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_dif_generate_copy(struct spdk_io_channel *ch, struct iovec *dst_iovs,
				    size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
				    uint32_t num_blocks, const struct spdk_dif_ctx *ctx,
				    spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->dif.ctx = ctx;
	accel_task->dif.num_blocks = num_blocks;
	accel_task->nbytes = num_blocks * ctx->block_size;
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_GENERATE_COPY;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

int
spdk_accel_submit_dif_verify_copy(struct spdk_io_channel *ch,
				  struct iovec *dst_iovs, size_t dst_iovcnt,
				  struct iovec *src_iovs, size_t src_iovcnt, uint32_t num_blocks,
				  const struct spdk_dif_ctx *ctx, struct spdk_dif_error *err,
				  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->dif.ctx = ctx;
	accel_task->dif.err = err;
	accel_task->dif.num_blocks = num_blocks;
	accel_task->nbytes = num_blocks * ctx->block_size;
	accel_task->op_code = SPDK_ACCEL_OPC_DIF_VERIFY_COPY;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;

	return accel_submit_task(accel_ch, accel_task);
}

static inline struct accel_buffer *
accel_get_buf(struct accel_io_channel *ch, uint64_t len)
{
	struct accel_buffer *buf;

	buf = SLIST_FIRST(&ch->buf_pool);
	if (spdk_unlikely(buf == NULL)) {
		accel_update_stats(ch, retry.bufdesc, 1);
		return NULL;
	}

	SLIST_REMOVE_HEAD(&ch->buf_pool, link);
	buf->len = len;
	buf->buf = NULL;
	buf->seq = NULL;
	buf->cb_fn = NULL;

	return buf;
}

static inline void
accel_put_buf(struct accel_io_channel *ch, struct accel_buffer *buf)
{
	if (buf->buf != NULL) {
		spdk_iobuf_put(&ch->iobuf, buf->buf, buf->len);
	}

	SLIST_INSERT_HEAD(&ch->buf_pool, buf, link);
}

static inline struct spdk_accel_sequence *
accel_sequence_get(struct accel_io_channel *ch)
{
	struct spdk_accel_sequence *seq;

	seq = SLIST_FIRST(&ch->seq_pool);
	if (spdk_unlikely(seq == NULL)) {
		accel_update_stats(ch, retry.sequence, 1);
		return NULL;
	}

	SLIST_REMOVE_HEAD(&ch->seq_pool, link);

	TAILQ_INIT(&seq->tasks);
	SLIST_INIT(&seq->bounce_bufs);

	seq->ch = ch;
	seq->status = 0;
	seq->state = ACCEL_SEQUENCE_STATE_INIT;
	seq->in_process_sequence = false;

	return seq;
}

static inline void
accel_sequence_put(struct spdk_accel_sequence *seq)
{
	struct accel_io_channel *ch = seq->ch;
	struct accel_buffer *buf;

	while (!SLIST_EMPTY(&seq->bounce_bufs)) {
		buf = SLIST_FIRST(&seq->bounce_bufs);
		SLIST_REMOVE_HEAD(&seq->bounce_bufs, link);
		accel_put_buf(seq->ch, buf);
	}

	assert(TAILQ_EMPTY(&seq->tasks));
	seq->ch = NULL;

	SLIST_INSERT_HEAD(&ch->seq_pool, seq, link);
}

static void accel_sequence_task_cb(void *cb_arg, int status);

static inline struct spdk_accel_task *
accel_sequence_get_task(struct accel_io_channel *ch, struct spdk_accel_sequence *seq,
			spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *task;

	task = _get_task(ch, NULL, NULL);
	if (spdk_unlikely(task == NULL)) {
		return task;
	}

	task->step_cb_fn = cb_fn;
	task->cb_arg = cb_arg;
	task->seq = seq;

	return task;
}

int
spdk_accel_append_copy(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
		       struct iovec *dst_iovs, uint32_t dst_iovcnt,
		       struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
		       struct iovec *src_iovs, uint32_t src_iovcnt,
		       struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		       spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	task->op_code = SPDK_ACCEL_OPC_COPY;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_fill(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
		       void *buf, uint64_t len,
		       struct spdk_memory_domain *domain, void *domain_ctx, uint8_t pattern,
		       spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	memset(&task->fill_pattern, pattern, sizeof(uint64_t));

	task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
	if (spdk_unlikely(!task->aux)) {
		SPDK_ERRLOG("Fatal problem, aux data was not allocated\n");
		if (*pseq == NULL) {
			accel_sequence_put((seq));
		}
		STAILQ_INSERT_HEAD(&task->accel_ch->task_pool, task, link);
		task->seq = NULL;
		assert(0);
		return -ENOMEM;
	}
	SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
	task->has_aux = true;

	task->d.iovs = &task->aux->iovs[SPDK_ACCEL_AUX_IOV_DST];
	task->d.iovs[0].iov_base = buf;
	task->d.iovs[0].iov_len = len;
	task->d.iovcnt = 1;
	task->nbytes = len;
	task->src_domain = NULL;
	task->dst_domain = domain;
	task->dst_domain_ctx = domain_ctx;
	task->op_code = SPDK_ACCEL_OPC_FILL;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_decompress(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			     struct iovec *dst_iovs, size_t dst_iovcnt,
			     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *src_iovs, size_t src_iovcnt,
			     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* TODO: support output_size for chaining */
	task->output_size = NULL;
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	task->op_code = SPDK_ACCEL_OPC_DECOMPRESS;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_encrypt(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			  struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	assert(dst_iovs && dst_iovcnt && src_iovs && src_iovcnt && key && block_size);

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	task->crypto_key = key;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	task->iv = iv;
	task->block_size = block_size;
	task->op_code = SPDK_ACCEL_OPC_ENCRYPT;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_decrypt(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			  struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			  uint64_t iv, uint32_t block_size,
			  spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	assert(dst_iovs && dst_iovcnt && src_iovs && src_iovcnt && key && block_size);

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	task->crypto_key = key;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->nbytes = accel_get_iovlen(src_iovs, src_iovcnt);
	task->iv = iv;
	task->block_size = block_size;
	task->op_code = SPDK_ACCEL_OPC_DECRYPT;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_crc32c(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			 uint32_t *dst, struct iovec *iovs, uint32_t iovcnt,
			 struct spdk_memory_domain *domain, void *domain_ctx,
			 uint32_t seed, spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	task->s.iovs = iovs;
	task->s.iovcnt = iovcnt;
	task->src_domain = domain;
	task->src_domain_ctx = domain_ctx;
	task->nbytes = accel_get_iovlen(iovs, iovcnt);
	task->crc_dst = dst;
	task->seed = seed;
	task->op_code = SPDK_ACCEL_OPC_CRC32C;
	task->dst_domain = NULL;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_get_buf(struct spdk_io_channel *ch, uint64_t len, void **buf,
		   struct spdk_memory_domain **domain, void **domain_ctx)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_buffer *accel_buf;

	accel_buf = accel_get_buf(accel_ch, len);
	if (spdk_unlikely(accel_buf == NULL)) {
		return -ENOMEM;
	}

	accel_buf->ch = accel_ch;

	/* We always return the same pointer and identify the buffers through domain_ctx */
	*buf = ACCEL_BUFFER_BASE;
	*domain_ctx = accel_buf;
	*domain = g_accel_domain;

	return 0;
}

void
spdk_accel_put_buf(struct spdk_io_channel *ch, void *buf,
		   struct spdk_memory_domain *domain, void *domain_ctx)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_buffer *accel_buf = domain_ctx;

	assert(domain == g_accel_domain);
	assert(buf == ACCEL_BUFFER_BASE);

	accel_put_buf(accel_ch, accel_buf);
}

static void
accel_sequence_complete_task(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	struct accel_io_channel *ch = seq->ch;
	spdk_accel_step_cb cb_fn;
	void *cb_arg;

	TAILQ_REMOVE(&seq->tasks, task, seq_link);
	cb_fn = task->step_cb_fn;
	cb_arg = task->cb_arg;
	task->seq = NULL;
	if (task->has_aux) {
		SLIST_INSERT_HEAD(&ch->task_aux_data_pool, task->aux, link);
		task->aux = NULL;
		task->has_aux = false;
	}
	STAILQ_INSERT_HEAD(&ch->task_pool, task, link);
	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}
}

static void
accel_sequence_complete_tasks(struct spdk_accel_sequence *seq)
{
	struct spdk_accel_task *task;

	while (!TAILQ_EMPTY(&seq->tasks)) {
		task = TAILQ_FIRST(&seq->tasks);
		accel_sequence_complete_task(seq, task);
	}
}

static void
accel_sequence_complete(struct spdk_accel_sequence *seq)
{
	SPDK_DEBUGLOG(accel, "Completed sequence: %p with status: %d\n", seq, seq->status);

	accel_update_stats(seq->ch, sequence_executed, 1);
	if (spdk_unlikely(seq->status != 0)) {
		accel_update_stats(seq->ch, sequence_failed, 1);
	}

	/* First notify all users that appended operations to this sequence */
	accel_sequence_complete_tasks(seq);

	/* Then notify the user that finished the sequence */
	seq->cb_fn(seq->cb_arg, seq->status);

	accel_sequence_put(seq);
}

static void
accel_update_virt_iov(struct iovec *diov, struct iovec *siov, struct accel_buffer *accel_buf)
{
	uintptr_t offset;

	offset = (uintptr_t)siov->iov_base & ACCEL_BUFFER_OFFSET_MASK;
	assert(offset < accel_buf->len);

	diov->iov_base = (char *)accel_buf->buf + offset;
	diov->iov_len = siov->iov_len;
}

static void
accel_sequence_set_virtbuf(struct spdk_accel_sequence *seq, struct accel_buffer *buf)
{
	struct spdk_accel_task *task;
	struct iovec *iov;

	/* Now that we've allocated the actual data buffer for this accel_buffer, update all tasks
	 * in a sequence that were using it.
	 */
	TAILQ_FOREACH(task, &seq->tasks, seq_link) {
		if (task->src_domain == g_accel_domain && task->src_domain_ctx == buf) {
			if (!task->has_aux) {
				task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
				assert(task->aux && "Can't allocate aux data structure");
				task->has_aux = true;
				SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
			}

			iov = &task->aux->iovs[SPDK_ACCEL_AXU_IOV_VIRT_SRC];
			assert(task->s.iovcnt == 1);
			accel_update_virt_iov(iov, &task->s.iovs[0], buf);
			task->src_domain = NULL;
			task->s.iovs = iov;
		}
		if (task->dst_domain == g_accel_domain && task->dst_domain_ctx == buf) {
			if (!task->has_aux) {
				task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
				assert(task->aux && "Can't allocate aux data structure");
				task->has_aux = true;
				SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
			}

			iov = &task->aux->iovs[SPDK_ACCEL_AXU_IOV_VIRT_DST];
			assert(task->d.iovcnt == 1);
			accel_update_virt_iov(iov, &task->d.iovs[0], buf);
			task->dst_domain = NULL;
			task->d.iovs = iov;
		}
	}
}

static void accel_process_sequence(struct spdk_accel_sequence *seq);

static void
accel_iobuf_get_virtbuf_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct accel_buffer *accel_buf;

	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);

	assert(accel_buf->seq != NULL);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF);
	accel_sequence_set_virtbuf(accel_buf->seq, accel_buf);
	accel_process_sequence(accel_buf->seq);
}

static bool
accel_sequence_alloc_buf(struct spdk_accel_sequence *seq, struct accel_buffer *buf,
			 spdk_iobuf_get_cb cb_fn)
{
	struct accel_io_channel *ch = seq->ch;

	assert(buf->seq == NULL);

	buf->seq = seq;

	/* Buffer might be already allocated by memory domain translation. */
	if (buf->buf) {
		return true;
	}

	buf->buf = spdk_iobuf_get(&ch->iobuf, buf->len, &buf->iobuf, cb_fn);
	if (spdk_unlikely(buf->buf == NULL)) {
		accel_update_stats(ch, retry.iobuf, 1);
		return false;
	}

	return true;
}

static bool
accel_sequence_check_virtbuf(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	/* If a task doesn't have dst/src (e.g. fill, crc32), its dst/src domain should be set to
	 * NULL */
	if (task->src_domain == g_accel_domain) {
		if (!accel_sequence_alloc_buf(seq, task->src_domain_ctx,
					      accel_iobuf_get_virtbuf_cb)) {
			return false;
		}

		accel_sequence_set_virtbuf(seq, task->src_domain_ctx);
	}

	if (task->dst_domain == g_accel_domain) {
		if (!accel_sequence_alloc_buf(seq, task->dst_domain_ctx,
					      accel_iobuf_get_virtbuf_cb)) {
			return false;
		}

		accel_sequence_set_virtbuf(seq, task->dst_domain_ctx);
	}

	return true;
}

static void
accel_sequence_get_buf_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct accel_buffer *accel_buf;

	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);

	assert(accel_buf->seq != NULL);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	accel_sequence_set_virtbuf(accel_buf->seq, accel_buf);
	accel_buf->cb_fn(accel_buf->seq, accel_buf->cb_ctx);
}

bool
spdk_accel_alloc_sequence_buf(struct spdk_accel_sequence *seq, void *buf,
			      struct spdk_memory_domain *domain, void *domain_ctx,
			      spdk_accel_sequence_get_buf_cb cb_fn, void *cb_ctx)
{
	struct accel_buffer *accel_buf = domain_ctx;

	assert(domain == g_accel_domain);
	accel_buf->cb_fn = cb_fn;
	accel_buf->cb_ctx = cb_ctx;

	if (!accel_sequence_alloc_buf(seq, accel_buf, accel_sequence_get_buf_cb)) {
		return false;
	}

	accel_sequence_set_virtbuf(seq, accel_buf);

	return true;
}

struct spdk_accel_task *
spdk_accel_sequence_first_task(struct spdk_accel_sequence *seq)
{
	return TAILQ_FIRST(&seq->tasks);
}

struct spdk_accel_task *
spdk_accel_sequence_next_task(struct spdk_accel_task *task)
{
	return TAILQ_NEXT(task, seq_link);
}

static inline void
accel_set_bounce_buffer(struct spdk_accel_bounce_buffer *bounce, struct iovec **iovs,
			uint32_t *iovcnt, struct spdk_memory_domain **domain, void **domain_ctx,
			struct accel_buffer *buf)
{
	bounce->orig_iovs = *iovs;
	bounce->orig_iovcnt = *iovcnt;
	bounce->orig_domain = *domain;
	bounce->orig_domain_ctx = *domain_ctx;
	bounce->iov.iov_base = buf->buf;
	bounce->iov.iov_len = buf->len;

	*iovs = &bounce->iov;
	*iovcnt = 1;
	*domain = NULL;
}

static void
accel_iobuf_get_src_bounce_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_accel_task *task;
	struct accel_buffer *accel_buf;

	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	task = TAILQ_FIRST(&accel_buf->seq->tasks);
	assert(task != NULL);

	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
	assert(task->aux);
	assert(task->has_aux);
	accel_set_bounce_buffer(&task->aux->bounce.s, &task->s.iovs, &task->s.iovcnt, &task->src_domain,
				&task->src_domain_ctx, accel_buf);
	accel_process_sequence(accel_buf->seq);
}

static void
accel_iobuf_get_dst_bounce_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_accel_task *task;
	struct accel_buffer *accel_buf;

	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	task = TAILQ_FIRST(&accel_buf->seq->tasks);
	assert(task != NULL);

	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
	assert(task->aux);
	assert(task->has_aux);
	accel_set_bounce_buffer(&task->aux->bounce.d, &task->d.iovs, &task->d.iovcnt, &task->dst_domain,
				&task->dst_domain_ctx, accel_buf);
	accel_process_sequence(accel_buf->seq);
}

static int
accel_sequence_check_bouncebuf(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	struct accel_buffer *buf;

	if (task->src_domain != NULL) {
		/* By the time we're here, accel buffers should have been allocated */
		assert(task->src_domain != g_accel_domain);

		if (!task->has_aux) {
			task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
			if (spdk_unlikely(!task->aux)) {
				SPDK_ERRLOG("Can't allocate aux data structure\n");
				assert(0);
				return -EAGAIN;
			}
			task->has_aux = true;
			SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
		}
		buf = accel_get_buf(seq->ch, accel_get_iovlen(task->s.iovs, task->s.iovcnt));
		if (buf == NULL) {
			SPDK_ERRLOG("Couldn't allocate buffer descriptor\n");
			return -ENOMEM;
		}

		SLIST_INSERT_HEAD(&seq->bounce_bufs, buf, link);
		if (!accel_sequence_alloc_buf(seq, buf, accel_iobuf_get_src_bounce_cb)) {
			return -EAGAIN;
		}

		accel_set_bounce_buffer(&task->aux->bounce.s, &task->s.iovs, &task->s.iovcnt,
					&task->src_domain, &task->src_domain_ctx, buf);
	}

	if (task->dst_domain != NULL) {
		/* By the time we're here, accel buffers should have been allocated */
		assert(task->dst_domain != g_accel_domain);

		if (!task->has_aux) {
			task->aux = SLIST_FIRST(&task->accel_ch->task_aux_data_pool);
			if (spdk_unlikely(!task->aux)) {
				SPDK_ERRLOG("Can't allocate aux data structure\n");
				assert(0);
				return -EAGAIN;
			}
			task->has_aux = true;
			SLIST_REMOVE_HEAD(&task->accel_ch->task_aux_data_pool, link);
		}
		buf = accel_get_buf(seq->ch, accel_get_iovlen(task->d.iovs, task->d.iovcnt));
		if (buf == NULL) {
			/* The src buffer will be released when a sequence is completed */
			SPDK_ERRLOG("Couldn't allocate buffer descriptor\n");
			return -ENOMEM;
		}

		SLIST_INSERT_HEAD(&seq->bounce_bufs, buf, link);
		if (!accel_sequence_alloc_buf(seq, buf, accel_iobuf_get_dst_bounce_cb)) {
			return -EAGAIN;
		}

		accel_set_bounce_buffer(&task->aux->bounce.d, &task->d.iovs, &task->d.iovcnt,
					&task->dst_domain, &task->dst_domain_ctx, buf);
	}

	return 0;
}

static void
accel_task_pull_data_cb(void *ctx, int status)
{
	struct spdk_accel_sequence *seq = ctx;

	assert(seq->state == ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA);
	if (spdk_likely(status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
	} else {
		accel_sequence_set_fail(seq, status);
	}

	accel_process_sequence(seq);
}

static void
accel_task_pull_data(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	int rc;

	assert(task->has_aux);
	assert(task->aux);
	assert(task->aux->bounce.s.orig_iovs != NULL);
	assert(task->aux->bounce.s.orig_domain != NULL);
	assert(task->aux->bounce.s.orig_domain != g_accel_domain);
	assert(!g_modules_opc[task->op_code].supports_memory_domains);

	rc = spdk_memory_domain_pull_data(task->aux->bounce.s.orig_domain,
					  task->aux->bounce.s.orig_domain_ctx,
					  task->aux->bounce.s.orig_iovs, task->aux->bounce.s.orig_iovcnt,
					  task->s.iovs, task->s.iovcnt,
					  accel_task_pull_data_cb, seq);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to pull data from memory domain: %s, rc: %d\n",
			    spdk_memory_domain_get_dma_device_id(task->aux->bounce.s.orig_domain), rc);
		accel_sequence_set_fail(seq, rc);
	}
}

static void
accel_task_push_data_cb(void *ctx, int status)
{
	struct spdk_accel_sequence *seq = ctx;

	assert(seq->state == ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA);
	if (spdk_likely(status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_NEXT_TASK);
	} else {
		accel_sequence_set_fail(seq, status);
	}

	accel_process_sequence(seq);
}

static void
accel_task_push_data(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	int rc;

	assert(task->has_aux);
	assert(task->aux);
	assert(task->aux->bounce.d.orig_iovs != NULL);
	assert(task->aux->bounce.d.orig_domain != NULL);
	assert(task->aux->bounce.d.orig_domain != g_accel_domain);
	assert(!g_modules_opc[task->op_code].supports_memory_domains);

	rc = spdk_memory_domain_push_data(task->aux->bounce.d.orig_domain,
					  task->aux->bounce.d.orig_domain_ctx,
					  task->aux->bounce.d.orig_iovs, task->aux->bounce.d.orig_iovcnt,
					  task->d.iovs, task->d.iovcnt,
					  accel_task_push_data_cb, seq);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to push data to memory domain: %s, rc: %d\n",
			    spdk_memory_domain_get_dma_device_id(task->aux->bounce.s.orig_domain), rc);
		accel_sequence_set_fail(seq, rc);
	}
}

static void
accel_process_sequence(struct spdk_accel_sequence *seq)
{
	struct accel_io_channel *accel_ch = seq->ch;
	struct spdk_accel_task *task;
	enum accel_sequence_state state;
	int rc;

	/* Prevent recursive calls to this function */
	if (spdk_unlikely(seq->in_process_sequence)) {
		return;
	}
	seq->in_process_sequence = true;

	task = TAILQ_FIRST(&seq->tasks);
	do {
		state = seq->state;
		switch (state) {
		case ACCEL_SEQUENCE_STATE_INIT:
			if (g_accel_driver != NULL) {
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS);
				break;
			}
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF);
			if (!accel_sequence_check_virtbuf(seq, task)) {
				/* We couldn't allocate a buffer, wait until one is available */
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF:
			/* If a module supports memory domains, we don't need to allocate bounce
			 * buffers */
			if (g_modules_opc[task->op_code].supports_memory_domains) {
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
			rc = accel_sequence_check_bouncebuf(seq, task);
			if (spdk_unlikely(rc != 0)) {
				/* We couldn't allocate a buffer, wait until one is available */
				if (rc == -EAGAIN) {
					break;
				}
				accel_sequence_set_fail(seq, rc);
				break;
			}
			if (task->has_aux && task->s.iovs == &task->aux->bounce.s.iov) {
				assert(task->aux->bounce.s.orig_iovs);
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_PULL_DATA);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_EXEC_TASK:
			SPDK_DEBUGLOG(accel, "Executing %s operation, sequence: %p\n",
				      g_opcode_strings[task->op_code], seq);

			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_TASK);
			rc = accel_submit_task(accel_ch, task);
			if (spdk_unlikely(rc != 0)) {
				SPDK_ERRLOG("Failed to submit %s operation, sequence: %p\n",
					    g_opcode_strings[task->op_code], seq);
				accel_sequence_set_fail(seq, rc);
			}
			break;
		case ACCEL_SEQUENCE_STATE_PULL_DATA:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA);
			accel_task_pull_data(seq, task);
			break;
		case ACCEL_SEQUENCE_STATE_COMPLETE_TASK:
			if (task->has_aux && task->d.iovs == &task->aux->bounce.d.iov) {
				assert(task->aux->bounce.d.orig_iovs);
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_PUSH_DATA);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_NEXT_TASK);
			break;
		case ACCEL_SEQUENCE_STATE_PUSH_DATA:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA);
			accel_task_push_data(seq, task);
			break;
		case ACCEL_SEQUENCE_STATE_NEXT_TASK:
			accel_sequence_complete_task(seq, task);
			/* Check if there are any remaining tasks */
			task = TAILQ_FIRST(&seq->tasks);
			if (task == NULL) {
				/* Immediately return here to make sure we don't touch the sequence
				 * after it's completed */
				accel_sequence_complete(seq);
				return;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_INIT);
			break;
		case ACCEL_SEQUENCE_STATE_DRIVER_EXEC_TASKS:
			assert(!TAILQ_EMPTY(&seq->tasks));

			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS);
			rc = g_accel_driver->execute_sequence(accel_ch->driver_channel, seq);
			if (spdk_unlikely(rc != 0)) {
				SPDK_ERRLOG("Failed to execute sequence: %p using driver: %s\n",
					    seq, g_accel_driver->name);
				accel_sequence_set_fail(seq, rc);
			}
			break;
		case ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS:
			/* Get the task again, as the driver might have completed some tasks
			 * synchronously */
			task = TAILQ_FIRST(&seq->tasks);
			if (task == NULL) {
				/* Immediately return here to make sure we don't touch the sequence
				 * after it's completed */
				accel_sequence_complete(seq);
				return;
			}
			/* We don't want to execute the next task through the driver, so we
			 * explicitly omit the INIT state here */
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF);
			break;
		case ACCEL_SEQUENCE_STATE_ERROR:
			/* Immediately return here to make sure we don't touch the sequence
			 * after it's completed */
			assert(seq->status != 0);
			accel_sequence_complete(seq);
			return;
		case ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF:
		case ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF:
		case ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA:
		case ACCEL_SEQUENCE_STATE_AWAIT_TASK:
		case ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA:
		case ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS:
			break;
		default:
			assert(0 && "bad state");
			break;
		}
	} while (seq->state != state);

	seq->in_process_sequence = false;
}

static void
accel_sequence_task_cb(void *cb_arg, int status)
{
	struct spdk_accel_sequence *seq = cb_arg;
	struct spdk_accel_task *task = TAILQ_FIRST(&seq->tasks);

	switch (seq->state) {
	case ACCEL_SEQUENCE_STATE_AWAIT_TASK:
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_COMPLETE_TASK);
		if (spdk_unlikely(status != 0)) {
			SPDK_ERRLOG("Failed to execute %s operation, sequence: %p\n",
				    g_opcode_strings[task->op_code], seq);
			accel_sequence_set_fail(seq, status);
		}

		accel_process_sequence(seq);
		break;
	case ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS:
		assert(g_accel_driver != NULL);
		/* Immediately remove the task from the outstanding list to make sure the next call
		 * to spdk_accel_sequence_first_task() doesn't return it */
		accel_sequence_complete_task(seq, task);
		if (spdk_unlikely(status != 0)) {
			SPDK_ERRLOG("Failed to execute %s operation, sequence: %p through "
				    "driver: %s\n", g_opcode_strings[task->op_code], seq,
				    g_accel_driver->name);
			/* Update status without using accel_sequence_set_fail() to avoid changing
			 * seq's state to ERROR until driver calls spdk_accel_sequence_continue() */
			seq->status = status;
		}
		break;
	default:
		assert(0 && "bad state");
		break;
	}
}

void
spdk_accel_sequence_continue(struct spdk_accel_sequence *seq)
{
	assert(g_accel_driver != NULL);
	assert(seq->state == ACCEL_SEQUENCE_STATE_DRIVER_AWAIT_TASKS);

	if (spdk_likely(seq->status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_DRIVER_COMPLETE_TASKS);
	} else {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_ERROR);
	}

	accel_process_sequence(seq);
}

static bool
accel_compare_iovs(struct iovec *iova, uint32_t iovacnt, struct iovec *iovb, uint32_t iovbcnt)
{
	/* For now, just do a dumb check that the iovecs arrays are exactly the same */
	if (iovacnt != iovbcnt) {
		return false;
	}

	return memcmp(iova, iovb, sizeof(*iova) * iovacnt) == 0;
}

static bool
accel_task_set_dstbuf(struct spdk_accel_task *task, struct spdk_accel_task *next)
{
	struct spdk_accel_task *prev;

	switch (task->op_code) {
	case SPDK_ACCEL_OPC_DECOMPRESS:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
		if (task->dst_domain != next->src_domain) {
			return false;
		}
		if (!accel_compare_iovs(task->d.iovs, task->d.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			return false;
		}
		task->d.iovs = next->d.iovs;
		task->d.iovcnt = next->d.iovcnt;
		task->dst_domain = next->dst_domain;
		task->dst_domain_ctx = next->dst_domain_ctx;
		break;
	case SPDK_ACCEL_OPC_CRC32C:
		/* crc32 is special, because it doesn't have a dst buffer */
		if (task->src_domain != next->src_domain) {
			return false;
		}
		if (!accel_compare_iovs(task->s.iovs, task->s.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			return false;
		}
		/* We can only change crc32's buffer if we can change previous task's buffer */
		prev = TAILQ_PREV(task, accel_sequence_tasks, seq_link);
		if (prev == NULL) {
			return false;
		}
		if (!accel_task_set_dstbuf(prev, next)) {
			return false;
		}
		task->s.iovs = next->d.iovs;
		task->s.iovcnt = next->d.iovcnt;
		task->src_domain = next->dst_domain;
		task->src_domain_ctx = next->dst_domain_ctx;
		break;
	default:
		return false;
	}

	return true;
}

static void
accel_sequence_merge_tasks(struct spdk_accel_sequence *seq, struct spdk_accel_task *task,
			   struct spdk_accel_task **next_task)
{
	struct spdk_accel_task *next = *next_task;

	switch (task->op_code) {
	case SPDK_ACCEL_OPC_COPY:
		/* We only allow changing src of operations that actually have a src, e.g. we never
		 * do it for fill.  Theoretically, it is possible, but we'd have to be careful to
		 * change the src of the operation after fill (which in turn could also be a fill).
		 * So, for the sake of simplicity, skip this type of operations for now.
		 */
		if (next->op_code != SPDK_ACCEL_OPC_DECOMPRESS &&
		    next->op_code != SPDK_ACCEL_OPC_COPY &&
		    next->op_code != SPDK_ACCEL_OPC_ENCRYPT &&
		    next->op_code != SPDK_ACCEL_OPC_DECRYPT &&
		    next->op_code != SPDK_ACCEL_OPC_COPY_CRC32C) {
			break;
		}
		if (task->dst_domain != next->src_domain) {
			break;
		}
		if (!accel_compare_iovs(task->d.iovs, task->d.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			break;
		}
		next->s.iovs = task->s.iovs;
		next->s.iovcnt = task->s.iovcnt;
		next->src_domain = task->src_domain;
		next->src_domain_ctx = task->src_domain_ctx;
		accel_sequence_complete_task(seq, task);
		break;
	case SPDK_ACCEL_OPC_DECOMPRESS:
	case SPDK_ACCEL_OPC_FILL:
	case SPDK_ACCEL_OPC_ENCRYPT:
	case SPDK_ACCEL_OPC_DECRYPT:
	case SPDK_ACCEL_OPC_CRC32C:
		/* We can only merge tasks when one of them is a copy */
		if (next->op_code != SPDK_ACCEL_OPC_COPY) {
			break;
		}
		if (!accel_task_set_dstbuf(task, next)) {
			break;
		}
		/* We're removing next_task from the tasks queue, so we need to update its pointer,
		 * so that the TAILQ_FOREACH_SAFE() loop below works correctly */
		*next_task = TAILQ_NEXT(next, seq_link);
		accel_sequence_complete_task(seq, next);
		break;
	default:
		assert(0 && "bad opcode");
		break;
	}
}

void
spdk_accel_sequence_finish(struct spdk_accel_sequence *seq,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *task, *next;

	/* Try to remove any copy operations if possible */
	TAILQ_FOREACH_SAFE(task, &seq->tasks, seq_link, next) {
		if (next == NULL) {
			break;
		}
		accel_sequence_merge_tasks(seq, task, &next);
	}

	seq->cb_fn = cb_fn;
	seq->cb_arg = cb_arg;

	accel_process_sequence(seq);
}

void
spdk_accel_sequence_reverse(struct spdk_accel_sequence *seq)
{
	struct accel_sequence_tasks tasks = TAILQ_HEAD_INITIALIZER(tasks);
	struct spdk_accel_task *task;

	TAILQ_SWAP(&tasks, &seq->tasks, spdk_accel_task, seq_link);

	while (!TAILQ_EMPTY(&tasks)) {
		task = TAILQ_FIRST(&tasks);
		TAILQ_REMOVE(&tasks, task, seq_link);
		TAILQ_INSERT_HEAD(&seq->tasks, task, seq_link);
	}
}

void
spdk_accel_sequence_abort(struct spdk_accel_sequence *seq)
{
	if (seq == NULL) {
		return;
	}

	accel_sequence_complete_tasks(seq);
	accel_sequence_put(seq);
}

struct spdk_memory_domain *
spdk_accel_get_memory_domain(void)
{
	return g_accel_domain;
}

static struct spdk_accel_module_if *
_module_find_by_name(const char *name)
{
	struct spdk_accel_module_if *accel_module = NULL;

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		if (strcmp(name, accel_module->name) == 0) {
			break;
		}
	}

	return accel_module;
}

static inline struct spdk_accel_crypto_key *
_accel_crypto_key_get(const char *name)
{
	struct spdk_accel_crypto_key *key;

	assert(spdk_spin_held(&g_keyring_spin));

	TAILQ_FOREACH(key, &g_keyring, link) {
		if (strcmp(name, key->param.key_name) == 0) {
			return key;
		}
	}

	return NULL;
}

static void
accel_crypto_key_free_mem(struct spdk_accel_crypto_key *key)
{
	if (key->param.hex_key) {
		spdk_memset_s(key->param.hex_key, key->key_size * 2, 0, key->key_size * 2);
		free(key->param.hex_key);
	}
	if (key->param.hex_key2) {
		spdk_memset_s(key->param.hex_key2, key->key2_size * 2, 0, key->key2_size * 2);
		free(key->param.hex_key2);
	}
	free(key->param.tweak_mode);
	free(key->param.key_name);
	free(key->param.cipher);
	if (key->key) {
		spdk_memset_s(key->key, key->key_size, 0, key->key_size);
		free(key->key);
	}
	if (key->key2) {
		spdk_memset_s(key->key2, key->key2_size, 0, key->key2_size);
		free(key->key2);
	}
	free(key);
}

static void
accel_crypto_key_destroy_unsafe(struct spdk_accel_crypto_key *key)
{
	assert(key->module_if);
	assert(key->module_if->crypto_key_deinit);

	key->module_if->crypto_key_deinit(key);
	accel_crypto_key_free_mem(key);
}

/*
 * This function mitigates a timing side channel which could be caused by using strcmp()
 * Please refer to chapter "Mitigating Information Leakage Based on Variable Timing" in
 * the article [1] for more details
 * [1] https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/secure-coding/mitigate-timing-side-channel-crypto-implementation.html
 */
static bool
accel_aes_xts_keys_equal(const char *k1, size_t k1_len, const char *k2, size_t k2_len)
{
	size_t i;
	volatile size_t x = k1_len ^ k2_len;

	for (i = 0; ((i < k1_len) & (i < k2_len)); i++) {
		x |= k1[i] ^ k2[i];
	}

	return x == 0;
}

static const char *g_tweak_modes[] = {
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA] = "SIMPLE_LBA",
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_JOIN_NEG_LBA_WITH_LBA] = "JOIN_NEG_LBA_WITH_LBA",
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_FULL_LBA] = "INCR_512_FULL_LBA",
	[SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_UPPER_LBA] = "INCR_512_UPPER_LBA",
};

static const char *g_ciphers[] = {
	[SPDK_ACCEL_CIPHER_AES_CBC] = "AES_CBC",
	[SPDK_ACCEL_CIPHER_AES_XTS] = "AES_XTS",
};

int
spdk_accel_crypto_key_create(const struct spdk_accel_crypto_key_create_param *param)
{
	struct spdk_accel_module_if *module;
	struct spdk_accel_crypto_key *key;
	size_t hex_key_size, hex_key2_size;
	bool found = false;
	size_t i;
	int rc;

	if (!param || !param->hex_key || !param->cipher || !param->key_name) {
		return -EINVAL;
	}

	if (g_modules_opc[SPDK_ACCEL_OPC_ENCRYPT].module != g_modules_opc[SPDK_ACCEL_OPC_DECRYPT].module) {
		/* hardly ever possible, but let's check and warn the user */
		SPDK_ERRLOG("Different accel modules are used for encryption and decryption\n");
	}
	module = g_modules_opc[SPDK_ACCEL_OPC_ENCRYPT].module;

	if (!module) {
		SPDK_ERRLOG("No accel module found assigned for crypto operation\n");
		return -ENOENT;
	}

	if (!module->crypto_key_init || !module->crypto_supports_cipher) {
		SPDK_ERRLOG("Module %s doesn't support crypto operations\n", module->name);
		return -ENOTSUP;
	}

	key = calloc(1, sizeof(*key));
	if (!key) {
		return -ENOMEM;
	}

	key->param.key_name = strdup(param->key_name);
	if (!key->param.key_name) {
		rc = -ENOMEM;
		goto error;
	}

	for (i = 0; i < SPDK_COUNTOF(g_ciphers); ++i) {
		assert(g_ciphers[i]);

		if (strncmp(param->cipher, g_ciphers[i], strlen(g_ciphers[i])) == 0) {
			key->cipher = i;
			found = true;
			break;
		}
	}

	if (!found) {
		SPDK_ERRLOG("Failed to parse cipher\n");
		rc = -EINVAL;
		goto error;
	}

	key->param.cipher = strdup(param->cipher);
	if (!key->param.cipher) {
		rc = -ENOMEM;
		goto error;
	}

	hex_key_size = strnlen(param->hex_key, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
	if (hex_key_size == SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH) {
		SPDK_ERRLOG("key1 size exceeds max %d\n", SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		rc = -EINVAL;
		goto error;
	}

	if (hex_key_size == 0) {
		SPDK_ERRLOG("key1 size cannot be 0\n");
		rc = -EINVAL;
		goto error;
	}

	key->param.hex_key = strdup(param->hex_key);
	if (!key->param.hex_key) {
		rc = -ENOMEM;
		goto error;
	}

	key->key_size = hex_key_size / 2;
	key->key = spdk_unhexlify(key->param.hex_key);
	if (!key->key) {
		SPDK_ERRLOG("Failed to unhexlify key1\n");
		rc = -EINVAL;
		goto error;
	}

	if (param->hex_key2) {
		hex_key2_size = strnlen(param->hex_key2, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		if (hex_key2_size == SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH) {
			SPDK_ERRLOG("key2 size exceeds max %d\n", SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
			rc = -EINVAL;
			goto error;
		}

		if (hex_key2_size == 0) {
			SPDK_ERRLOG("key2 size cannot be 0\n");
			rc = -EINVAL;
			goto error;
		}

		key->param.hex_key2 = strdup(param->hex_key2);
		if (!key->param.hex_key2) {
			rc = -ENOMEM;
			goto error;
		}

		key->key2_size = hex_key2_size / 2;
		key->key2 = spdk_unhexlify(key->param.hex_key2);
		if (!key->key2) {
			SPDK_ERRLOG("Failed to unhexlify key2\n");
			rc = -EINVAL;
			goto error;
		}
	}

	key->tweak_mode = ACCEL_CRYPTO_TWEAK_MODE_DEFAULT;
	if (param->tweak_mode) {
		found = false;

		key->param.tweak_mode = strdup(param->tweak_mode);
		if (!key->param.tweak_mode) {
			rc = -ENOMEM;
			goto error;
		}

		for (i = 0; i < SPDK_COUNTOF(g_tweak_modes); ++i) {
			assert(g_tweak_modes[i]);

			if (strncmp(param->tweak_mode, g_tweak_modes[i], strlen(g_tweak_modes[i])) == 0) {
				key->tweak_mode = i;
				found = true;
				break;
			}
		}

		if (!found) {
			SPDK_ERRLOG("Failed to parse tweak mode\n");
			rc = -EINVAL;
			goto error;
		}
	}

	if ((!module->crypto_supports_tweak_mode && key->tweak_mode != ACCEL_CRYPTO_TWEAK_MODE_DEFAULT) ||
	    (module->crypto_supports_tweak_mode && !module->crypto_supports_tweak_mode(key->tweak_mode))) {
		SPDK_ERRLOG("Module %s doesn't support %s tweak mode\n", module->name,
			    g_tweak_modes[key->tweak_mode]);
		rc = -EINVAL;
		goto error;
	}

	if (!module->crypto_supports_cipher(key->cipher, key->key_size)) {
		SPDK_ERRLOG("Module %s doesn't support %s cipher with %zu key size\n", module->name,
			    g_ciphers[key->cipher], key->key_size);
		rc = -EINVAL;
		goto error;
	}

	if (key->cipher == SPDK_ACCEL_CIPHER_AES_XTS) {
		if (!key->key2) {
			SPDK_ERRLOG("%s key2 is missing\n", g_ciphers[key->cipher]);
			rc = -EINVAL;
			goto error;
		}

		if (key->key_size != key->key2_size) {
			SPDK_ERRLOG("%s key size %zu is not equal to key2 size %zu\n", g_ciphers[key->cipher],
				    key->key_size,
				    key->key2_size);
			rc = -EINVAL;
			goto error;
		}

		if (accel_aes_xts_keys_equal(key->key, key->key_size, key->key2, key->key2_size)) {
			SPDK_ERRLOG("%s identical keys are not secure\n", g_ciphers[key->cipher]);
			rc = -EINVAL;
			goto error;
		}
	}

	if (key->cipher == SPDK_ACCEL_CIPHER_AES_CBC) {
		if (key->key2_size) {
			SPDK_ERRLOG("%s doesn't use key2\n", g_ciphers[key->cipher]);
			rc = -EINVAL;
			goto error;
		}
	}

	key->module_if = module;

	spdk_spin_lock(&g_keyring_spin);
	if (_accel_crypto_key_get(param->key_name)) {
		rc = -EEXIST;
	} else {
		rc = module->crypto_key_init(key);
		if (rc) {
			SPDK_ERRLOG("Module %s failed to initialize crypto key\n", module->name);
		} else {
			TAILQ_INSERT_TAIL(&g_keyring, key, link);
		}
	}
	spdk_spin_unlock(&g_keyring_spin);

	if (rc) {
		goto error;
	}

	return 0;

error:
	accel_crypto_key_free_mem(key);
	return rc;
}

int
spdk_accel_crypto_key_destroy(struct spdk_accel_crypto_key *key)
{
	if (!key || !key->module_if) {
		return -EINVAL;
	}

	spdk_spin_lock(&g_keyring_spin);
	if (!_accel_crypto_key_get(key->param.key_name)) {
		spdk_spin_unlock(&g_keyring_spin);
		return -ENOENT;
	}
	TAILQ_REMOVE(&g_keyring, key, link);
	spdk_spin_unlock(&g_keyring_spin);

	accel_crypto_key_destroy_unsafe(key);

	return 0;
}

struct spdk_accel_crypto_key *
spdk_accel_crypto_key_get(const char *name)
{
	struct spdk_accel_crypto_key *key;

	spdk_spin_lock(&g_keyring_spin);
	key = _accel_crypto_key_get(name);
	spdk_spin_unlock(&g_keyring_spin);

	return key;
}

/* Helper function when accel modules register with the framework. */
void
spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module)
{
	struct spdk_accel_module_if *tmp;

	if (_module_find_by_name(accel_module->name)) {
		SPDK_NOTICELOG("Module %s already registered\n", accel_module->name);
		assert(false);
		return;
	}

	TAILQ_FOREACH(tmp, &spdk_accel_module_list, tailq) {
		if (accel_module->priority < tmp->priority) {
			break;
		}
	}

	if (tmp != NULL) {
		TAILQ_INSERT_BEFORE(tmp, accel_module, tailq);
	} else {
		TAILQ_INSERT_TAIL(&spdk_accel_module_list, accel_module, tailq);
	}
}

/* Framework level channel create callback. */
static int
accel_create_channel(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;
	struct spdk_accel_task *accel_task;
	struct spdk_accel_task_aux_data *accel_task_aux;
	struct spdk_accel_sequence *seq;
	struct accel_buffer *buf;
	size_t task_size_aligned;
	uint8_t *task_mem;
	uint32_t i = 0, j;
	int rc;

	task_size_aligned = SPDK_ALIGN_CEIL(g_max_accel_module_size, SPDK_CACHE_LINE_SIZE);
	accel_ch->task_pool_base = aligned_alloc(SPDK_CACHE_LINE_SIZE,
				   g_opts.task_count * task_size_aligned);
	if (!accel_ch->task_pool_base) {
		return -ENOMEM;
	}
	memset(accel_ch->task_pool_base, 0, g_opts.task_count * task_size_aligned);

	accel_ch->seq_pool_base = aligned_alloc(SPDK_CACHE_LINE_SIZE,
						g_opts.sequence_count * sizeof(struct spdk_accel_sequence));
	if (accel_ch->seq_pool_base == NULL) {
		goto err;
	}
	memset(accel_ch->seq_pool_base, 0, g_opts.sequence_count * sizeof(struct spdk_accel_sequence));

	accel_ch->task_aux_data_base = calloc(g_opts.task_count, sizeof(struct spdk_accel_task_aux_data));
	if (accel_ch->task_aux_data_base == NULL) {
		goto err;
	}

	accel_ch->buf_pool_base = calloc(g_opts.buf_count, sizeof(struct accel_buffer));
	if (accel_ch->buf_pool_base == NULL) {
		goto err;
	}

	STAILQ_INIT(&accel_ch->task_pool);
	SLIST_INIT(&accel_ch->task_aux_data_pool);
	SLIST_INIT(&accel_ch->seq_pool);
	SLIST_INIT(&accel_ch->buf_pool);

	task_mem = accel_ch->task_pool_base;
	for (i = 0; i < g_opts.task_count; i++) {
		accel_task = (struct spdk_accel_task *)task_mem;
		accel_task->aux = NULL;
		STAILQ_INSERT_TAIL(&accel_ch->task_pool, accel_task, link);
		task_mem += task_size_aligned;
		accel_task_aux = &accel_ch->task_aux_data_base[i];
		SLIST_INSERT_HEAD(&accel_ch->task_aux_data_pool, accel_task_aux, link);
	}
	for (i = 0; i < g_opts.sequence_count; i++) {
		seq = &accel_ch->seq_pool_base[i];
		SLIST_INSERT_HEAD(&accel_ch->seq_pool, seq, link);
	}
	for (i = 0; i < g_opts.buf_count; i++) {
		buf = &accel_ch->buf_pool_base[i];
		SLIST_INSERT_HEAD(&accel_ch->buf_pool, buf, link);
	}

	/* Assign modules and get IO channels for each */
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; i++) {
		accel_ch->module_ch[i] = g_modules_opc[i].module->get_io_channel();
		/* This can happen if idxd runs out of channels. */
		if (accel_ch->module_ch[i] == NULL) {
			SPDK_ERRLOG("Module %s failed to get io channel\n", g_modules_opc[i].module->name);
			goto err;
		}
	}

	if (g_accel_driver != NULL) {
		accel_ch->driver_channel = g_accel_driver->get_io_channel();
		if (accel_ch->driver_channel == NULL) {
			SPDK_ERRLOG("Failed to get driver's IO channel\n");
			goto err;
		}
	}

	rc = spdk_iobuf_channel_init(&accel_ch->iobuf, "accel", g_opts.small_cache_size,
				     g_opts.large_cache_size);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize iobuf accel channel\n");
		goto err;
	}

	return 0;
err:
	if (accel_ch->driver_channel != NULL) {
		spdk_put_io_channel(accel_ch->driver_channel);
	}
	for (j = 0; j < i; j++) {
		spdk_put_io_channel(accel_ch->module_ch[j]);
	}
	free(accel_ch->task_pool_base);
	free(accel_ch->task_aux_data_base);
	free(accel_ch->seq_pool_base);
	free(accel_ch->buf_pool_base);

	return -ENOMEM;
}

static void
accel_add_stats(struct accel_stats *total, struct accel_stats *stats)
{
	int i;

	total->sequence_executed += stats->sequence_executed;
	total->sequence_failed += stats->sequence_failed;
	total->retry.task += stats->retry.task;
	total->retry.sequence += stats->retry.sequence;
	total->retry.iobuf += stats->retry.iobuf;
	total->retry.bufdesc += stats->retry.bufdesc;
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; ++i) {
		total->operations[i].executed += stats->operations[i].executed;
		total->operations[i].failed += stats->operations[i].failed;
		total->operations[i].num_bytes += stats->operations[i].num_bytes;
	}
}

/* Framework level channel destroy callback. */
static void
accel_destroy_channel(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;
	int i;

	spdk_iobuf_channel_fini(&accel_ch->iobuf);

	if (accel_ch->driver_channel != NULL) {
		spdk_put_io_channel(accel_ch->driver_channel);
	}

	for (i = 0; i < SPDK_ACCEL_OPC_LAST; i++) {
		assert(accel_ch->module_ch[i] != NULL);
		spdk_put_io_channel(accel_ch->module_ch[i]);
		accel_ch->module_ch[i] = NULL;
	}

	/* Update global stats to make sure channel's stats aren't lost after a channel is gone */
	spdk_spin_lock(&g_stats_lock);
	accel_add_stats(&g_stats, &accel_ch->stats);
	spdk_spin_unlock(&g_stats_lock);

	free(accel_ch->task_pool_base);
	free(accel_ch->task_aux_data_base);
	free(accel_ch->seq_pool_base);
	free(accel_ch->buf_pool_base);
}

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&spdk_accel_module_list);
}

static int
accel_module_initialize(void)
{
	struct spdk_accel_module_if *accel_module, *tmp_module;
	int rc = 0, module_rc;

	TAILQ_FOREACH_SAFE(accel_module, &spdk_accel_module_list, tailq, tmp_module) {
		module_rc = accel_module->module_init();
		if (module_rc) {
			TAILQ_REMOVE(&spdk_accel_module_list, accel_module, tailq);
			if (module_rc == -ENODEV) {
				SPDK_NOTICELOG("No devices for module %s, skipping\n", accel_module->name);
			} else if (!rc) {
				SPDK_ERRLOG("Module %s initialization failed with %d\n", accel_module->name, module_rc);
				rc = module_rc;
			}
			continue;
		}

		SPDK_DEBUGLOG(accel, "Module %s initialized.\n", accel_module->name);
	}

	return rc;
}

static void
accel_module_init_opcode(enum spdk_accel_opcode opcode)
{
	struct accel_module *module = &g_modules_opc[opcode];
	struct spdk_accel_module_if *module_if = module->module;

	if (module_if->get_memory_domains != NULL) {
		module->supports_memory_domains = module_if->get_memory_domains(NULL, 0) > 0;
	}
}

static int
accel_memory_domain_translate(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			      struct spdk_memory_domain *dst_domain, struct spdk_memory_domain_translation_ctx *dst_domain_ctx,
			      void *addr, size_t len, struct spdk_memory_domain_translation_result *result)
{
	struct accel_buffer *buf = src_domain_ctx;

	SPDK_DEBUGLOG(accel, "translate addr %p, len %zu\n", addr, len);

	assert(g_accel_domain == src_domain);
	assert(spdk_memory_domain_get_system_domain() == dst_domain);
	assert(buf->buf == NULL);
	assert(addr == ACCEL_BUFFER_BASE);
	assert(len == buf->len);

	buf->buf = spdk_iobuf_get(&buf->ch->iobuf, buf->len, NULL, NULL);
	if (spdk_unlikely(buf->buf == NULL)) {
		return -ENOMEM;
	}

	result->iov_count = 1;
	result->iov.iov_base = buf->buf;
	result->iov.iov_len = buf->len;
	SPDK_DEBUGLOG(accel, "translated addr %p\n", result->iov.iov_base);
	return 0;
}

static void
accel_memory_domain_invalidate(struct spdk_memory_domain *domain, void *domain_ctx,
			       struct iovec *iov, uint32_t iovcnt)
{
	struct accel_buffer *buf = domain_ctx;

	SPDK_DEBUGLOG(accel, "invalidate addr %p, len %zu\n", iov[0].iov_base, iov[0].iov_len);

	assert(g_accel_domain == domain);
	assert(iovcnt == 1);
	assert(buf->buf != NULL);
	assert(iov[0].iov_base == buf->buf);
	assert(iov[0].iov_len == buf->len);

	spdk_iobuf_put(&buf->ch->iobuf, buf->buf, buf->len);
	buf->buf = NULL;
}

int
spdk_accel_initialize(void)
{
	enum spdk_accel_opcode op;
	struct spdk_accel_module_if *accel_module = NULL;
	int rc;

	/*
	 * We need a unique identifier for the accel framework, so use the
	 * spdk_accel_module_list address for this purpose.
	 */
	spdk_io_device_register(&spdk_accel_module_list, accel_create_channel, accel_destroy_channel,
				sizeof(struct accel_io_channel), "accel");

	spdk_spin_init(&g_keyring_spin);
	spdk_spin_init(&g_stats_lock);

	rc = spdk_memory_domain_create(&g_accel_domain, SPDK_DMA_DEVICE_TYPE_ACCEL, NULL,
				       "SPDK_ACCEL_DMA_DEVICE");
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create accel memory domain\n");
		return rc;
	}

	spdk_memory_domain_set_translation(g_accel_domain, accel_memory_domain_translate);
	spdk_memory_domain_set_invalidate(g_accel_domain, accel_memory_domain_invalidate);

	g_modules_started = true;
	rc = accel_module_initialize();
	if (rc) {
		return rc;
	}

	if (g_accel_driver != NULL && g_accel_driver->init != NULL) {
		rc = g_accel_driver->init();
		if (rc != 0) {
			SPDK_ERRLOG("Failed to initialize driver %s: %s\n", g_accel_driver->name,
				    spdk_strerror(-rc));
			return rc;
		}
	}

	/* The module list is order by priority, with the highest priority modules being at the end
	 * of the list.  The software module should be somewhere at the beginning of the list,
	 * before all HW modules.
	 * NOTE: all opcodes must be supported by software in the event that no HW modules are
	 * initialized to support the operation.
	 */
	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
			if (accel_module->supports_opcode(op)) {
				g_modules_opc[op].module = accel_module;
				SPDK_DEBUGLOG(accel, "OPC 0x%x now assigned to %s\n", op, accel_module->name);
			}
		}

		if (accel_module->get_ctx_size != NULL) {
			g_max_accel_module_size = spdk_max(g_max_accel_module_size,
							   accel_module->get_ctx_size());
		}
	}

	/* Now lets check for overrides and apply all that exist */
	for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
		if (g_modules_opc_override[op] != NULL) {
			accel_module = _module_find_by_name(g_modules_opc_override[op]);
			if (accel_module == NULL) {
				SPDK_ERRLOG("Invalid module name of %s\n", g_modules_opc_override[op]);
				return -EINVAL;
			}
			if (accel_module->supports_opcode(op) == false) {
				SPDK_ERRLOG("Module %s does not support op code %d\n", accel_module->name, op);
				return -EINVAL;
			}
			g_modules_opc[op].module = accel_module;
		}
	}

	if (g_modules_opc[SPDK_ACCEL_OPC_ENCRYPT].module != g_modules_opc[SPDK_ACCEL_OPC_DECRYPT].module) {
		SPDK_ERRLOG("Different accel modules are assigned to encrypt and decrypt operations");
		return -EINVAL;
	}

	for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
		assert(g_modules_opc[op].module != NULL);
		accel_module_init_opcode(op);
	}

	rc = spdk_iobuf_register_module("accel");
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register accel iobuf module\n");
		return rc;
	}

	return 0;
}

static void
accel_module_finish_cb(void)
{
	spdk_accel_fini_cb cb_fn = g_fini_cb_fn;

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
}

static void
accel_write_overridden_opc(struct spdk_json_write_ctx *w, const char *opc_str,
			   const char *module_str)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_assign_opc");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "opname", opc_str);
	spdk_json_write_named_string(w, "module", module_str);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
__accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key)
{
	spdk_json_write_named_string(w, "name", key->param.key_name);
	spdk_json_write_named_string(w, "cipher", key->param.cipher);
	spdk_json_write_named_string(w, "key", key->param.hex_key);
	if (key->param.hex_key2) {
		spdk_json_write_named_string(w, "key2", key->param.hex_key2);
	}

	if (key->param.tweak_mode) {
		spdk_json_write_named_string(w, "tweak_mode", key->param.tweak_mode);
	}
}

void
_accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key)
{
	spdk_json_write_object_begin(w);
	__accel_crypto_key_dump_param(w, key);
	spdk_json_write_object_end(w);
}

static void
_accel_crypto_key_write_config_json(struct spdk_json_write_ctx *w,
				    struct spdk_accel_crypto_key *key)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_crypto_key_create");
	spdk_json_write_named_object_begin(w, "params");
	__accel_crypto_key_dump_param(w, key);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
accel_write_options(struct spdk_json_write_ctx *w)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_set_options");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_uint32(w, "small_cache_size", g_opts.small_cache_size);
	spdk_json_write_named_uint32(w, "large_cache_size", g_opts.large_cache_size);
	spdk_json_write_named_uint32(w, "task_count", g_opts.task_count);
	spdk_json_write_named_uint32(w, "sequence_count", g_opts.sequence_count);
	spdk_json_write_named_uint32(w, "buf_count", g_opts.buf_count);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
_accel_crypto_keys_write_config_json(struct spdk_json_write_ctx *w, bool full_dump)
{
	struct spdk_accel_crypto_key *key;

	spdk_spin_lock(&g_keyring_spin);
	TAILQ_FOREACH(key, &g_keyring, link) {
		if (full_dump) {
			_accel_crypto_key_write_config_json(w, key);
		} else {
			_accel_crypto_key_dump_param(w, key);
		}
	}
	spdk_spin_unlock(&g_keyring_spin);
}

void
_accel_crypto_keys_dump_param(struct spdk_json_write_ctx *w)
{
	_accel_crypto_keys_write_config_json(w, false);
}

void
spdk_accel_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_accel_module_if *accel_module;
	int i;

	spdk_json_write_array_begin(w);
	accel_write_options(w);

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		if (accel_module->write_config_json) {
			accel_module->write_config_json(w);
		}
	}
	for (i = 0; i < SPDK_ACCEL_OPC_LAST; i++) {
		if (g_modules_opc_override[i]) {
			accel_write_overridden_opc(w, g_opcode_strings[i], g_modules_opc_override[i]);
		}
	}

	_accel_crypto_keys_write_config_json(w, true);

	spdk_json_write_array_end(w);
}

void
spdk_accel_module_finish(void)
{
	if (!g_accel_module) {
		g_accel_module = TAILQ_FIRST(&spdk_accel_module_list);
	} else {
		g_accel_module = TAILQ_NEXT(g_accel_module, tailq);
	}

	if (!g_accel_module) {
		if (g_accel_driver != NULL && g_accel_driver->fini != NULL) {
			g_accel_driver->fini();
		}

		spdk_spin_destroy(&g_keyring_spin);
		spdk_spin_destroy(&g_stats_lock);
		if (g_accel_domain) {
			spdk_memory_domain_destroy(g_accel_domain);
			g_accel_domain = NULL;
		}
		accel_module_finish_cb();
		return;
	}

	if (g_accel_module->module_fini) {
		spdk_thread_send_msg(spdk_get_thread(), g_accel_module->module_fini, NULL);
	} else {
		spdk_accel_module_finish();
	}
}

static void
accel_io_device_unregister_cb(void *io_device)
{
	struct spdk_accel_crypto_key *key, *key_tmp;
	enum spdk_accel_opcode op;

	spdk_spin_lock(&g_keyring_spin);
	TAILQ_FOREACH_SAFE(key, &g_keyring, link, key_tmp) {
		accel_crypto_key_destroy_unsafe(key);
	}
	spdk_spin_unlock(&g_keyring_spin);

	for (op = 0; op < SPDK_ACCEL_OPC_LAST; op++) {
		if (g_modules_opc_override[op] != NULL) {
			free(g_modules_opc_override[op]);
			g_modules_opc_override[op] = NULL;
		}
		g_modules_opc[op].module = NULL;
	}

	spdk_accel_module_finish();
}

void
spdk_accel_finish(spdk_accel_fini_cb cb_fn, void *cb_arg)
{
	assert(cb_fn != NULL);

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	spdk_io_device_unregister(&spdk_accel_module_list, accel_io_device_unregister_cb);
}

static struct spdk_accel_driver *
accel_find_driver(const char *name)
{
	struct spdk_accel_driver *driver;

	TAILQ_FOREACH(driver, &g_accel_drivers, tailq) {
		if (strcmp(driver->name, name) == 0) {
			return driver;
		}
	}

	return NULL;
}

int
spdk_accel_set_driver(const char *name)
{
	struct spdk_accel_driver *driver;

	driver = accel_find_driver(name);
	if (driver == NULL) {
		SPDK_ERRLOG("Couldn't find driver named '%s'\n", name);
		return -ENODEV;
	}

	g_accel_driver = driver;

	return 0;
}

void
spdk_accel_driver_register(struct spdk_accel_driver *driver)
{
	if (accel_find_driver(driver->name)) {
		SPDK_ERRLOG("Driver named '%s' has already been registered\n", driver->name);
		assert(0);
		return;
	}

	TAILQ_INSERT_TAIL(&g_accel_drivers, driver, tailq);
}

int
spdk_accel_set_opts(const struct spdk_accel_opts *opts)
{
	if (!opts) {
		SPDK_ERRLOG("opts cannot be NULL\n");
		return -1;
	}

	if (!opts->opts_size) {
		SPDK_ERRLOG("opts_size inside opts cannot be zero value\n");
		return -1;
	}

#define SET_FIELD(field) \
        if (offsetof(struct spdk_accel_opts, field) + sizeof(opts->field) <= opts->opts_size) { \
                g_opts.field = opts->field; \
        } \

	SET_FIELD(small_cache_size);
	SET_FIELD(large_cache_size);
	SET_FIELD(task_count);
	SET_FIELD(sequence_count);
	SET_FIELD(buf_count);

	g_opts.opts_size = opts->opts_size;

#undef SET_FIELD

	return 0;
}

void
spdk_accel_get_opts(struct spdk_accel_opts *opts, size_t opts_size)
{
	if (!opts) {
		SPDK_ERRLOG("opts should not be NULL\n");
		return;
	}

	if (!opts_size) {
		SPDK_ERRLOG("opts_size should not be zero value\n");
		return;
	}

	opts->opts_size = opts_size;

#define SET_FIELD(field) \
	if (offsetof(struct spdk_accel_opts, field) + sizeof(opts->field) <= opts_size) { \
		opts->field = g_opts.field; \
	} \

	SET_FIELD(small_cache_size);
	SET_FIELD(large_cache_size);
	SET_FIELD(task_count);
	SET_FIELD(sequence_count);
	SET_FIELD(buf_count);

#undef SET_FIELD

	/* Do not remove this statement, you should always update this statement when you adding a new field,
	 * and do not forget to add the SET_FIELD statement for your added field. */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_accel_opts) == 28, "Incorrect size");
}

struct accel_get_stats_ctx {
	struct accel_stats	stats;
	accel_get_stats_cb	cb_fn;
	void			*cb_arg;
};

static void
accel_get_channel_stats_done(struct spdk_io_channel_iter *iter, int status)
{
	struct accel_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter);

	ctx->cb_fn(&ctx->stats, ctx->cb_arg);
	free(ctx);
}

static void
accel_get_channel_stats(struct spdk_io_channel_iter *iter)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(iter);
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_get_stats_ctx *ctx = spdk_io_channel_iter_get_ctx(iter);

	accel_add_stats(&ctx->stats, &accel_ch->stats);
	spdk_for_each_channel_continue(iter, 0);
}

int
accel_get_stats(accel_get_stats_cb cb_fn, void *cb_arg)
{
	struct accel_get_stats_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}

	spdk_spin_lock(&g_stats_lock);
	accel_add_stats(&ctx->stats, &g_stats);
	spdk_spin_unlock(&g_stats_lock);

	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;

	spdk_for_each_channel(&spdk_accel_module_list, accel_get_channel_stats, ctx,
			      accel_get_channel_stats_done);

	return 0;
}

void
spdk_accel_get_opcode_stats(struct spdk_io_channel *ch, enum spdk_accel_opcode opcode,
			    struct spdk_accel_opcode_stats *stats, size_t size)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);

#define FIELD_OK(field) \
	offsetof(struct spdk_accel_opcode_stats, field) + sizeof(stats->field) <= size

#define SET_FIELD(field, value) \
	if (FIELD_OK(field)) { \
		stats->field = value; \
	}

	SET_FIELD(executed, accel_ch->stats.operations[opcode].executed);
	SET_FIELD(failed, accel_ch->stats.operations[opcode].failed);
	SET_FIELD(num_bytes, accel_ch->stats.operations[opcode].num_bytes);

#undef FIELD_OK
#undef SET_FIELD
}

uint8_t
spdk_accel_get_buf_align(enum spdk_accel_opcode opcode,
			 const struct spdk_accel_operation_exec_ctx *ctx)
{
	struct spdk_accel_module_if *module = g_modules_opc[opcode].module;
	struct spdk_accel_opcode_info modinfo = {}, drvinfo = {};

	if (g_accel_driver != NULL && g_accel_driver->get_operation_info != NULL) {
		g_accel_driver->get_operation_info(opcode, ctx, &drvinfo);
	}

	if (module->get_operation_info != NULL) {
		module->get_operation_info(opcode, ctx, &modinfo);
	}

	/* If a driver is set, it'll execute most of the operations, while the rest will usually
	 * fall back to accel_sw, which doesn't have any alignment requiremenets.  However, to be
	 * extra safe, return the max(driver, module) if a driver delegates some operations to a
	 * hardware module. */
	return spdk_max(modinfo.required_alignment, drvinfo.required_alignment);
}

struct spdk_accel_module_if *
spdk_accel_get_module(const char *name)
{
	struct spdk_accel_module_if *module;

	TAILQ_FOREACH(module, &spdk_accel_module_list, tailq) {
		if (strcmp(module->name, name) == 0) {
			return module;
		}
	}

	return NULL;
}

SPDK_LOG_REGISTER_COMPONENT(accel)
