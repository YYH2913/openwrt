/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_CORE_H_
#define _AIROHA_XPON_CORE_H_

#include <linux/types.h>

#include "airoha-xpon-mode.h"

struct device;
struct device_node;
struct airoha_xpon_backend;

/* All callbacks must be idempotent and must never enable optical TX. */
struct airoha_xpon_backend_ops {
	int (*block_traffic)(void *context);
	int (*clear_session)(void *context);
	int (*mask_irqs)(void *context);
	void (*synchronize_irqs)(void *context);
	int (*stop_datapath)(void *context);
	int (*stop_mac)(void *context);
	int (*start_mac)(void *context);
	int (*start_datapath)(void *context);
	int (*unmask_irqs)(void *context);
	int (*mode_committed)(void *context);
};

struct airoha_xpon_backend *
airoha_xpon_backend_register(struct device *dev,
			     struct device_node *controller,
			     enum airoha_xpon_mode mode,
			     const struct airoha_xpon_backend_ops *ops,
			     void *context);
int airoha_xpon_backend_ready(struct airoha_xpon_backend *backend);
void airoha_xpon_backend_unregister(struct airoha_xpon_backend *backend);
bool airoha_xpon_backend_is_active(struct airoha_xpon_backend *backend);

#endif
