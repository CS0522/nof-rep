/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_ACCEL_INTERNAL_H
#define SPDK_INTERNAL_ACCEL_INTERNAL_H

#include "spdk/stdinc.h"

#include "spdk/accel.h"
#include "spdk/queue.h"
#include "spdk/config.h"

#define ACCEL_AES_XTS "AES_XTS"

struct module_info {
	struct spdk_json_write_ctx *w;
	const char *name;
	enum spdk_accel_opcode ops[SPDK_ACCEL_OPC_LAST];
	uint32_t num_ops;
};

struct accel_operation_stats {
	uint64_t executed;
	uint64_t failed;
	uint64_t num_bytes;
};

struct accel_stats {
	struct accel_operation_stats	operations[SPDK_ACCEL_OPC_LAST];
	uint64_t			sequence_executed;
	uint64_t			sequence_failed;

	struct {
		uint64_t task;
		uint64_t sequence;
		uint64_t iobuf;
		uint64_t bufdesc;
	} retry;
};

typedef void (*_accel_for_each_module_fn)(struct module_info *info);
void _accel_for_each_module(struct module_info *info, _accel_for_each_module_fn fn);
void _accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key);
void _accel_crypto_keys_dump_param(struct spdk_json_write_ctx *w);
typedef void (*accel_get_stats_cb)(struct accel_stats *stats, void *cb_arg);
int accel_get_stats(accel_get_stats_cb cb_fn, void *cb_arg);

#endif
