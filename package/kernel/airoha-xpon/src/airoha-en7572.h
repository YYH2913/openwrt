/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EN7572_H_
#define _AIROHA_EN7572_H_

#include <linux/types.h>

#include "airoha-xpon-mode.h"

struct device_node;
struct airoha_en7572;

struct airoha_en7572 *airoha_en7572_get(struct device_node *np);
void airoha_en7572_put(struct airoha_en7572 *bosa);
bool airoha_en7572_is_ready(struct airoha_en7572 *bosa);
bool airoha_en7572_is_xgspon(struct airoha_en7572 *bosa);
bool airoha_en7572_tx_is_disabled(struct airoha_en7572 *bosa);
bool airoha_en7572_fault_locked(struct airoha_en7572 *bosa);
enum airoha_xpon_mode airoha_en7572_get_mode(struct airoha_en7572 *bosa);
int airoha_en7572_validate_mode(struct airoha_en7572 *bosa,
			       enum airoha_xpon_mode mode);
int airoha_en7572_set_mode(struct airoha_en7572 *bosa,
			  enum airoha_xpon_mode mode);
int airoha_en7572_set_tx_enabled(struct airoha_en7572 *bosa, bool enabled);
int airoha_en7572_factory_tx_enable(struct airoha_en7572 *bosa);
void airoha_en7572_factory_tx_disable(struct airoha_en7572 *bosa);
int airoha_en7572_clear_fault(struct airoha_en7572 *bosa);
void airoha_en7572_emergency_disable(struct airoha_en7572 *bosa);

#endif
