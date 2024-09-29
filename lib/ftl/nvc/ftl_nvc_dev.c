/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 */

#include "spdk/stdinc.h"
#include "spdk/queue.h"
#include "spdk/log.h"

#include "ftl_nvc_dev.h"
#include "utils/ftl_defs.h"

static TAILQ_HEAD(, ftl_nv_cache_device_type) g_devs = TAILQ_HEAD_INITIALIZER(g_devs);
static pthread_mutex_t g_devs_mutex = PTHREAD_MUTEX_INITIALIZER;

static const struct ftl_nv_cache_device_type *
ftl_nv_cache_device_type_get_type(const char *name)
{
	struct ftl_nv_cache_device_type *entry;

	TAILQ_FOREACH(entry, &g_devs, internal.entry) {
		if (0 == strcmp(entry->name, name)) {
			return entry;
		}
	}

	return NULL;
}

static bool
ftl_nv_cache_device_valid(const struct ftl_nv_cache_device_type *type)
{
	return type && type->name && strlen(type->name) > 0;
}

void
ftl_nv_cache_device_register(struct ftl_nv_cache_device_type *type)
{
	if (!ftl_nv_cache_device_valid(type)) {
		SPDK_ERRLOG("NV cache device descriptor is invalid\n");
		ftl_abort();
	}

	pthread_mutex_lock(&g_devs_mutex);
	if (!ftl_nv_cache_device_type_get_type(type->name)) {
		TAILQ_INSERT_TAIL(&g_devs, type, internal.entry);
		SPDK_NOTICELOG("Registered NV cache device, name: %s\n", type->name);
	} else {
		SPDK_ERRLOG("Cannot register NV cache device, already exists, name: %s\n", type->name);
		ftl_abort();
	}

	pthread_mutex_unlock(&g_devs_mutex);
}

const struct ftl_nv_cache_device_type *
ftl_nv_cache_device_get_type_by_bdev(struct spdk_ftl_dev *dev, struct spdk_bdev *bdev)
{
	struct ftl_nv_cache_device_type *entry;
	const struct ftl_nv_cache_device_type *type = NULL;

	pthread_mutex_lock(&g_devs_mutex);
	TAILQ_FOREACH(entry, &g_devs, internal.entry) {
		if (entry->ops.is_bdev_compatible) {
			if (entry->ops.is_bdev_compatible(dev, bdev)) {
				type = entry;
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_devs_mutex);

	return type;
}
