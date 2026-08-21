// SPDX-License-Identifier: GPL-2.0-only
/* EN7581 runtime XPON mode owner. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pcs/pcs-airoha.h>
#include <linux/pcs/pcs.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "airoha-en7572.h"
#include "airoha-xpon-claim-transaction.h"
#include "airoha-xpon-core.h"
#include "airoha-xpon-rollback-transaction.h"
#include "airoha-xpon-switch-transaction.h"

#define EN7581_SCU_DMTC			0x84
#define EN7581_SCU_DYING_GASP_STATUS	BIT(16)

struct airoha_xpon_core {
	struct device *dev;
	struct list_head node;
	struct list_head backends;
	struct mutex switch_lock;
	struct regmap *scu;
	struct phylink_pcs *pcs;
	struct airoha_en7572 *bosa;
	struct airoha_xpon_backend *current_backend;
	enum airoha_xpon_mode current_mode;
	struct airoha_xpon_switch_state state;
	atomic64_t dying_gasp_events;
	atomic64_t dying_gasp_clear_errors;
	atomic_t dying_gasp_last_mode;
	int dying_gasp_irq;
	int last_error;
	bool switching;
	bool removing;
};

struct airoha_xpon_backend {
	struct list_head node;
	struct airoha_xpon_core *core;
	struct device *dev;
	enum airoha_xpon_mode mode;
	const struct airoha_xpon_backend_ops *ops;
	void *context;
	bool ready;
};

struct airoha_xpon_switch_context {
	struct airoha_xpon_core *core;
	struct airoha_xpon_backend *previous;
	struct airoha_xpon_backend *target;
};

static LIST_HEAD(airoha_xpon_cores);
static DEFINE_MUTEX(airoha_xpon_cores_lock);

static int airoha_xpon_clear_dying_gasp(struct airoha_xpon_core *core)
{
	/* Force the write even when the W1C status bit reads as already set. */
	return regmap_write_bits(core->scu, EN7581_SCU_DMTC,
				 EN7581_SCU_DYING_GASP_STATUS,
				 EN7581_SCU_DYING_GASP_STATUS);
}

static irqreturn_t airoha_xpon_dying_gasp_irq(int irq, void *data)
{
	struct airoha_xpon_core *core = data;

	if (airoha_xpon_clear_dying_gasp(core))
		atomic64_inc(&core->dying_gasp_clear_errors);
	atomic_set(&core->dying_gasp_last_mode, READ_ONCE(core->current_mode));
	atomic64_inc(&core->dying_gasp_events);

	/* The active MAC sends Dying Gasp in hardware while hold-up power lasts. */
	return IRQ_HANDLED;
}

static bool
airoha_xpon_backend_ops_valid(const struct airoha_xpon_backend_ops *ops)
{
	return ops && ops->block_traffic && ops->clear_session &&
	       ops->mask_irqs && ops->synchronize_irqs &&
	       ops->stop_datapath && ops->stop_mac && ops->start_mac &&
	       ops->start_datapath && ops->unmask_irqs;
}

static int airoha_xpon_to_pcs_mode(enum airoha_xpon_mode mode,
				   enum airoha_pcs_xpon_mode *pcs_mode)
{
	switch (mode) {
	case AIROHA_XPON_MODE_GPON:
		*pcs_mode = AIROHA_PCS_XPON_MODE_GPON;
		break;
	case AIROHA_XPON_MODE_XGPON:
		*pcs_mode = AIROHA_PCS_XPON_MODE_XGPON;
		break;
	case AIROHA_XPON_MODE_XGSPON:
		*pcs_mode = AIROHA_PCS_XPON_MODE_XGSPON;
		break;
	case AIROHA_XPON_MODE_EPON_10G_1G:
		*pcs_mode = AIROHA_PCS_XPON_MODE_EPON_10G_1G;
		break;
	case AIROHA_XPON_MODE_EPON_10G_10G:
		*pcs_mode = AIROHA_PCS_XPON_MODE_EPON_10G_10G;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int airoha_xpon_from_pcs_mode(enum airoha_pcs_xpon_mode pcs_mode,
				     enum airoha_xpon_mode *mode)
{
	switch (pcs_mode) {
	case AIROHA_PCS_XPON_MODE_GPON:
		*mode = AIROHA_XPON_MODE_GPON;
		break;
	case AIROHA_PCS_XPON_MODE_XGPON:
		*mode = AIROHA_XPON_MODE_XGPON;
		break;
	case AIROHA_PCS_XPON_MODE_XGSPON:
		*mode = AIROHA_XPON_MODE_XGSPON;
		break;
	case AIROHA_PCS_XPON_MODE_EPON_10G_1G:
		*mode = AIROHA_XPON_MODE_EPON_10G_1G;
		break;
	case AIROHA_PCS_XPON_MODE_EPON_10G_10G:
		*mode = AIROHA_XPON_MODE_EPON_10G_10G;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int airoha_xpon_validate_bosa_mode(struct airoha_xpon_core *core,
					   enum airoha_xpon_mode mode)
{
	return airoha_en7572_validate_mode(core->bosa, mode);
}

static int airoha_xpon_validate_runtime_mode(struct airoha_xpon_core *core,
					      enum airoha_xpon_mode mode)
{
	enum airoha_pcs_xpon_mode pcs_mode;
	int ret;

	ret = airoha_xpon_to_pcs_mode(mode, &pcs_mode);
	if (ret)
		return ret;
	ret = airoha_pcs_xpon_validate_mode(core->pcs, pcs_mode);
	if (ret)
		return ret;

	return airoha_xpon_validate_bosa_mode(core, mode);
}

static struct airoha_xpon_backend *
airoha_xpon_find_backend(struct airoha_xpon_core *core,
			 enum airoha_xpon_mode mode)
{
	struct airoha_xpon_backend *backend;

	list_for_each_entry(backend, &core->backends, node)
		if (backend->mode == mode)
			return backend;

	return NULL;
}

static int airoha_xpon_backend_plain(
	struct airoha_xpon_backend *backend,
	int (*callback)(void *context))
{
	return backend && callback ? callback(backend->context) : -ENODEV;
}

static int airoha_xpon_switch_block_traffic(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_xpon_backend_plain(context->previous,
					 context->previous->ops->block_traffic);
}

static void airoha_xpon_switch_disable_tx(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	if (airoha_en7572_set_tx_enabled(context->core->bosa, false))
		airoha_en7572_emergency_disable(context->core->bosa);
}

static bool airoha_xpon_switch_tx_is_disabled(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_en7572_tx_is_disabled(context->core->bosa);
}

static int airoha_xpon_switch_clear_session(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_xpon_backend_plain(context->previous,
					 context->previous->ops->clear_session);
}

static int airoha_xpon_switch_mask_irqs(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_xpon_backend_plain(context->previous,
					 context->previous->ops->mask_irqs);
}

static void airoha_xpon_switch_synchronize_irqs(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	context->previous->ops->synchronize_irqs(context->previous->context);
}

static int airoha_xpon_switch_stop_datapath(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_xpon_backend_plain(context->previous,
					 context->previous->ops->stop_datapath);
}

static int airoha_xpon_switch_stop_mac(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_xpon_backend_plain(context->previous,
					 context->previous->ops->stop_mac);
}

static int airoha_xpon_switch_quiesce_pcs(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_pcs_xpon_quiesce(context->core->pcs);
}

static int airoha_xpon_switch_select_wan(void *data,
					enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;
	enum airoha_pcs_xpon_mode pcs_mode;
	int ret;

	if (context->target->mode != mode)
		return -EINVAL;
	ret = airoha_xpon_to_pcs_mode(mode, &pcs_mode);
	return ret ? ret : airoha_pcs_xpon_select_wan(context->core->pcs,
						       pcs_mode);
}

static int airoha_xpon_switch_configure_pma(void *data,
					 enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;
	enum airoha_pcs_xpon_mode pcs_mode;
	int ret;

	ret = airoha_xpon_to_pcs_mode(mode, &pcs_mode);
	return ret ? ret : airoha_pcs_xpon_set_mode(context->core->pcs,
						    pcs_mode);
}

static int airoha_xpon_switch_load_calibration(void *data,
					      enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;
	int ret;

	ret = airoha_en7572_set_mode(context->core->bosa, mode);
	if (ret)
		return ret;

	return airoha_xpon_validate_bosa_mode(context->core, mode);
}

static int airoha_xpon_switch_recover_pcs(void *data)
{
	struct airoha_xpon_switch_context *context = data;
	int ret;

	ret = airoha_pcs_xpon_recover(context->core->pcs);
	return ret ? ret : airoha_xpon_validate_runtime_mode(context->core,
							       context->target->mode);
}

static int airoha_xpon_switch_start_mac(void *data,
				       enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	if (context->target->mode != mode)
		return -EINVAL;
	return airoha_xpon_backend_plain(context->target,
					 context->target->ops->start_mac);
}

static int airoha_xpon_switch_start_datapath(void *data,
					    enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	if (context->target->mode != mode)
		return -EINVAL;
	return airoha_xpon_backend_plain(context->target,
					 context->target->ops->start_datapath);
}

static int airoha_xpon_switch_validate_mode(void *data,
					   enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	if (context->target->mode != mode)
		return -EINVAL;

	return airoha_xpon_validate_runtime_mode(context->core, mode);
}

static int airoha_xpon_switch_unmask_irqs(void *data,
					 enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	if (context->target->mode != mode)
		return -EINVAL;
	return airoha_xpon_backend_plain(context->target,
					 context->target->ops->unmask_irqs);
}

static void airoha_xpon_switch_commit_owner(void *data,
				   enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	context->core->current_mode = mode;
	WRITE_ONCE(context->core->current_backend, context->target);
}

static int airoha_xpon_switch_activate_mode(void *data,
				    enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	if (context->target->mode != mode)
		return -EINVAL;
	if (context->target->ops->mode_committed)
		return context->target->ops->mode_committed(
			context->target->context);
	return 0;
}

static int airoha_xpon_record_error(int recorded, int error)
{
	return recorded ? recorded : error;
}

static int airoha_xpon_backend_quiesce(
		struct airoha_xpon_backend *backend)
{
	int error = 0, ret;

	ret = backend->ops->mask_irqs(backend->context);
	if (ret)
		error = airoha_xpon_record_error(error, ret);
	backend->ops->synchronize_irqs(backend->context);
	ret = backend->ops->stop_datapath(backend->context);
	if (ret)
		error = airoha_xpon_record_error(error, ret);
	ret = backend->ops->clear_session(backend->context);
	if (ret)
		error = airoha_xpon_record_error(error, ret);
	ret = backend->ops->stop_mac(backend->context);
	if (ret)
		error = airoha_xpon_record_error(error, ret);

	return error;
}

static bool airoha_xpon_claim_tx_is_disabled(void *data)
{
	struct airoha_xpon_backend *backend = data;

	return airoha_en7572_tx_is_disabled(backend->core->bosa);
}

static int airoha_xpon_claim_start_mac(void *data)
{
	struct airoha_xpon_backend *backend = data;

	return backend->ops->start_mac(backend->context);
}

static int airoha_xpon_claim_start_datapath(void *data)
{
	struct airoha_xpon_backend *backend = data;

	return backend->ops->start_datapath(backend->context);
}

static int airoha_xpon_claim_validate_mode(void *data)
{
	struct airoha_xpon_backend *backend = data;

	return airoha_xpon_validate_runtime_mode(backend->core, backend->mode);
}

static void airoha_xpon_claim_commit_owner(void *data)
{
	struct airoha_xpon_backend *backend = data;

	WRITE_ONCE(backend->core->current_backend, backend);
}

static int airoha_xpon_claim_unmask_irqs(void *data)
{
	struct airoha_xpon_backend *backend = data;

	return backend->ops->unmask_irqs(backend->context);
}

static int airoha_xpon_claim_activate_mode(void *data)
{
	struct airoha_xpon_backend *backend = data;

	return backend->ops->mode_committed ?
		backend->ops->mode_committed(backend->context) : 0;
}

static void airoha_xpon_claim_disable_tx(void *data)
{
	struct airoha_xpon_backend *backend = data;

	if (airoha_en7572_set_tx_enabled(backend->core->bosa, false))
		airoha_en7572_emergency_disable(backend->core->bosa);
}

static int airoha_xpon_claim_quiesce_backend(void *data)
{
	return airoha_xpon_backend_quiesce(data);
}

static void airoha_xpon_claim_clear_owner(void *data)
{
	struct airoha_xpon_backend *backend = data;

	if (backend->core->current_backend == backend)
		WRITE_ONCE(backend->core->current_backend, NULL);
}

static const struct airoha_xpon_claim_ops airoha_xpon_core_claim_ops = {
	.tx_is_disabled = airoha_xpon_claim_tx_is_disabled,
	.start_mac = airoha_xpon_claim_start_mac,
	.start_datapath = airoha_xpon_claim_start_datapath,
	.validate_mode = airoha_xpon_claim_validate_mode,
	.commit_owner = airoha_xpon_claim_commit_owner,
	.unmask_irqs = airoha_xpon_claim_unmask_irqs,
	.activate_mode = airoha_xpon_claim_activate_mode,
	.disable_tx = airoha_xpon_claim_disable_tx,
	.quiesce_backend = airoha_xpon_claim_quiesce_backend,
	.clear_owner = airoha_xpon_claim_clear_owner,
};

static int airoha_xpon_core_claim_backend(
		struct airoha_xpon_backend *backend)
{
	struct airoha_xpon_core *core = backend->core;
	int ret;

	if (core->removing || core->switching)
		return -EBUSY;
	if (!backend->ready || backend->mode != core->current_mode)
		return 0;
	if (core->current_backend)
		return core->current_backend == backend ? 0 : -EBUSY;
	if (airoha_en7572_fault_locked(core->bosa))
		return -EIO;
	ret = airoha_xpon_validate_runtime_mode(core, backend->mode);
	if (ret) {
		core->last_error = ret;
		return ret;
	}

	core->switching = true;
	ret = airoha_xpon_claim_transaction(&airoha_xpon_core_claim_ops,
					     backend);
	core->last_error = ret;
	core->switching = false;
	return ret;
}

static int airoha_xpon_rollback_quiesce_target(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_xpon_backend_quiesce(context->target);
}

static int airoha_xpon_rollback_quiesce_pcs(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_pcs_xpon_quiesce(context->core->pcs);
}

static int airoha_xpon_rollback_select_previous(
		void *data, enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;
	enum airoha_pcs_xpon_mode pcs_mode;
	int ret;

	ret = airoha_xpon_to_pcs_mode(mode, &pcs_mode);
	return ret ? ret : airoha_pcs_xpon_select_wan(context->core->pcs,
						       pcs_mode);
}

static int airoha_xpon_rollback_configure_previous(
		void *data, enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;
	enum airoha_pcs_xpon_mode pcs_mode;
	int ret;

	ret = airoha_xpon_to_pcs_mode(mode, &pcs_mode);
	return ret ? ret : airoha_pcs_xpon_set_mode(context->core->pcs,
						     pcs_mode);
}

static int airoha_xpon_rollback_load_previous_calibration(
		void *data, enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;
	int ret;

	ret = airoha_en7572_set_mode(context->core->bosa, mode);
	return ret ? ret : airoha_xpon_validate_bosa_mode(context->core, mode);
}

static int airoha_xpon_rollback_recover_pcs(void *data)
{
	struct airoha_xpon_switch_context *context = data;
	int ret;

	ret = airoha_pcs_xpon_recover(context->core->pcs);
	return ret ? ret : airoha_xpon_validate_runtime_mode(context->core,
							       context->previous->mode);
}

static int airoha_xpon_rollback_start_previous_mac(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return context->previous->ops->start_mac(context->previous->context);
}

static int airoha_xpon_rollback_start_previous_datapath(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return context->previous->ops->start_datapath(
		context->previous->context);
}

static int airoha_xpon_rollback_validate_previous(
		void *data, enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	if (context->previous->mode != mode)
		return -EINVAL;

	return airoha_xpon_validate_runtime_mode(context->core, mode);
}

static void airoha_xpon_rollback_commit_previous_owner(
		void *data, enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context *context = data;

	context->core->current_mode = mode;
	WRITE_ONCE(context->core->current_backend, context->previous);
}

static int airoha_xpon_rollback_unmask_previous_irqs(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return context->previous->ops->unmask_irqs(context->previous->context);
}

static int airoha_xpon_rollback_resume_previous(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return context->previous->ops->mode_committed ?
		context->previous->ops->mode_committed(
			context->previous->context) : 0;
}

static int airoha_xpon_rollback_quiesce_previous(void *data)
{
	struct airoha_xpon_switch_context *context = data;

	return airoha_xpon_backend_quiesce(context->previous);
}

static const struct airoha_xpon_rollback_ops airoha_xpon_core_rollback_ops = {
	.disable_tx = airoha_xpon_switch_disable_tx,
	.quiesce_target = airoha_xpon_rollback_quiesce_target,
	.quiesce_pcs = airoha_xpon_rollback_quiesce_pcs,
	.select_previous = airoha_xpon_rollback_select_previous,
	.configure_previous = airoha_xpon_rollback_configure_previous,
	.load_previous_calibration =
		airoha_xpon_rollback_load_previous_calibration,
	.recover_pcs = airoha_xpon_rollback_recover_pcs,
	.start_previous_mac = airoha_xpon_rollback_start_previous_mac,
	.start_previous_datapath = airoha_xpon_rollback_start_previous_datapath,
	.validate_previous = airoha_xpon_rollback_validate_previous,
	.commit_previous_owner = airoha_xpon_rollback_commit_previous_owner,
	.unmask_previous_irqs = airoha_xpon_rollback_unmask_previous_irqs,
	.resume_previous = airoha_xpon_rollback_resume_previous,
	.quiesce_previous = airoha_xpon_rollback_quiesce_previous,
};

static int airoha_xpon_switch_rollback(void *data,
				      enum airoha_xpon_mode previous_mode,
				      enum airoha_xpon_switch_stage failed_stage)
{
	struct airoha_xpon_switch_context *context = data;
	bool target_start_attempted;

	target_start_attempted =
		airoha_xpon_switch_target_start_attempted(failed_stage);
	return airoha_xpon_rollback_transaction(&airoha_xpon_core_rollback_ops,
		context, previous_mode, target_start_attempted);
}

static void airoha_xpon_switch_fault_lock(void *data)
{
	struct airoha_xpon_switch_context *context = data;
	int ret;

	airoha_en7572_emergency_disable(context->core->bosa);
	ret = airoha_xpon_backend_quiesce(context->target);
	if (ret)
		dev_err(context->target->dev,
			"failed to quiesce target after rollback failure: %d\n", ret);
	if (context->previous->context != context->target->context) {
		ret = airoha_xpon_backend_quiesce(context->previous);
		if (ret)
			dev_err(context->previous->dev,
				"failed to quiesce previous backend after rollback failure: %d\n",
				ret);
	}
	ret = airoha_pcs_xpon_quiesce(context->core->pcs);
	if (ret)
		dev_err(context->core->dev,
			"failed to quiesce PCS after rollback failure: %d\n", ret);
	WRITE_ONCE(context->core->current_backend, NULL);
	context->core->current_mode = AIROHA_XPON_MODE_INVALID;
}

static const struct airoha_xpon_switch_ops airoha_xpon_core_switch_ops = {
	.block_traffic = airoha_xpon_switch_block_traffic,
	.disable_tx = airoha_xpon_switch_disable_tx,
	.tx_is_disabled = airoha_xpon_switch_tx_is_disabled,
	.clear_protocol_session = airoha_xpon_switch_clear_session,
	.mask_irqs = airoha_xpon_switch_mask_irqs,
	.synchronize_irqs = airoha_xpon_switch_synchronize_irqs,
	.stop_datapath = airoha_xpon_switch_stop_datapath,
	.stop_mac = airoha_xpon_switch_stop_mac,
	.quiesce_pcs = airoha_xpon_switch_quiesce_pcs,
	.select_wan = airoha_xpon_switch_select_wan,
	.configure_pma = airoha_xpon_switch_configure_pma,
	.load_calibration = airoha_xpon_switch_load_calibration,
	.recover_pcs = airoha_xpon_switch_recover_pcs,
	.start_mac = airoha_xpon_switch_start_mac,
	.start_datapath = airoha_xpon_switch_start_datapath,
	.validate_mode = airoha_xpon_switch_validate_mode,
	.commit_owner = airoha_xpon_switch_commit_owner,
	.unmask_irqs = airoha_xpon_switch_unmask_irqs,
	.activate_mode = airoha_xpon_switch_activate_mode,
	.rollback = airoha_xpon_switch_rollback,
	.fault_lock = airoha_xpon_switch_fault_lock,
};

static int airoha_xpon_parse_mode(const char *value,
				  enum airoha_xpon_mode *mode)
{
	unsigned int candidate;

	for (candidate = 0; candidate < AIROHA_XPON_MODE_COUNT; candidate++) {
		const struct airoha_xpon_mode_descriptor *descriptor;

		descriptor = airoha_xpon_mode_descriptor(candidate);
		if (sysfs_streq(value, descriptor->name)) {
			*mode = candidate;
			return 0;
		}
	}

	return -EINVAL;
}

static int airoha_xpon_core_switch(struct airoha_xpon_core *core,
				   enum airoha_xpon_mode mode)
{
	struct airoha_xpon_switch_context context;
	struct airoha_xpon_backend *target;
	int ret;

	if (core->removing || core->switching)
		return -EBUSY;
	if (airoha_en7572_fault_locked(core->bosa))
		return -EIO;
	target = airoha_xpon_find_backend(core, mode);
	if (!target || !target->ready)
		return -EOPNOTSUPP;
	if (mode == core->current_mode)
		return core->current_backend ? 0 :
			airoha_xpon_core_claim_backend(target);
	if (!core->current_backend)
		return -EHOSTDOWN;

	context.core = core;
	context.previous = core->current_backend;
	context.target = target;
	core->switching = true;
	ret = airoha_xpon_switch_transaction(&airoha_xpon_core_switch_ops,
					      &context, core->current_mode,
					      mode, &core->state);
	core->last_error = ret;
	core->switching = false;
	return ret;
}

static ssize_t mode_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct airoha_xpon_core *core = dev_get_drvdata(dev);
	const struct airoha_xpon_mode_descriptor *descriptor;

	mutex_lock(&core->switch_lock);
	descriptor = airoha_xpon_mode_descriptor(core->current_mode);
	mutex_unlock(&core->switch_lock);
	return sysfs_emit(buf, "%s\n", descriptor ? descriptor->name : "invalid");
}

static ssize_t mode_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct airoha_xpon_core *core = dev_get_drvdata(dev);
	enum airoha_xpon_mode mode;
	int ret;

	ret = airoha_xpon_parse_mode(buf, &mode);
	if (ret)
		return ret;
	mutex_lock(&core->switch_lock);
	ret = airoha_xpon_core_switch(core, mode);
	mutex_unlock(&core->switch_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(mode);

static ssize_t available_modes_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct airoha_xpon_core *core = dev_get_drvdata(dev);
	struct airoha_xpon_backend *backend;
	ssize_t length = 0;

	mutex_lock(&core->switch_lock);
	list_for_each_entry(backend, &core->backends, node) {
		const struct airoha_xpon_mode_descriptor *descriptor;

		if (!backend->ready)
			continue;

		descriptor = airoha_xpon_mode_descriptor(backend->mode);
		length += sysfs_emit_at(buf, length, "%s%s",
					length ? " " : "", descriptor->name);
	}
	length += sysfs_emit_at(buf, length, "\n");
	mutex_unlock(&core->switch_lock);
	return length;
}
static DEVICE_ATTR_RO(available_modes);

static ssize_t switch_state_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct airoha_xpon_core *core = dev_get_drvdata(dev);
	ssize_t length;

	mutex_lock(&core->switch_lock);
	length = sysfs_emit(buf,
		"switching=%u stage=%u failed_stage=%u error=%d committed=%u rollback_failed=%u tx_disabled=%u fault_locked=%u\n",
		core->switching, core->state.stage, core->state.failed_stage,
		core->last_error,
		core->state.committed, core->state.rollback_failed,
		airoha_en7572_tx_is_disabled(core->bosa),
		airoha_en7572_fault_locked(core->bosa));
	mutex_unlock(&core->switch_lock);
	return length;
}
static DEVICE_ATTR_RO(switch_state);

static ssize_t dying_gasp_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct airoha_xpon_core *core = dev_get_drvdata(dev);
	const struct airoha_xpon_mode_descriptor *descriptor;
	int mode = atomic_read(&core->dying_gasp_last_mode);

	descriptor = mode < 0 ? NULL : airoha_xpon_mode_descriptor(mode);
	return sysfs_emit(buf, "events=%lld clear_errors=%lld last_mode=%s\n",
			  (long long)atomic64_read(&core->dying_gasp_events),
			  (long long)atomic64_read(
				  &core->dying_gasp_clear_errors),
			  descriptor ? descriptor->name : "none");
}
static DEVICE_ATTR_RO(dying_gasp);

static struct attribute *airoha_xpon_core_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_available_modes.attr,
	&dev_attr_switch_state.attr,
	&dev_attr_dying_gasp.attr,
	NULL,
};
ATTRIBUTE_GROUPS(airoha_xpon_core);

struct airoha_xpon_backend *
airoha_xpon_backend_register(struct device *dev,
			     struct device_node *controller,
			     enum airoha_xpon_mode mode,
			     const struct airoha_xpon_backend_ops *ops,
			     void *context)
{
	struct airoha_xpon_backend *backend, *existing;
	struct airoha_xpon_core *core, *found = NULL;

	if (!dev || !controller || !airoha_xpon_mode_valid(mode) ||
	    !airoha_xpon_backend_ops_valid(ops))
		return ERR_PTR(-EINVAL);

	mutex_lock(&airoha_xpon_cores_lock);
	list_for_each_entry(core, &airoha_xpon_cores, node) {
		if (core->dev->of_node == controller && !core->removing) {
			found = core;
			break;
		}
	}
	if (!found) {
		mutex_unlock(&airoha_xpon_cores_lock);
		return ERR_PTR(-EPROBE_DEFER);
	}
	core = found;
	get_device(core->dev);
	mutex_unlock(&airoha_xpon_cores_lock);

	backend = kzalloc(sizeof(*backend), GFP_KERNEL);
	if (!backend) {
		put_device(core->dev);
		return ERR_PTR(-ENOMEM);
	}
	backend->core = core;
	backend->dev = dev;
	backend->mode = mode;
	backend->ops = ops;
	backend->context = context;

	mutex_lock(&core->switch_lock);
	existing = airoha_xpon_find_backend(core, mode);
	if (existing || core->removing) {
		mutex_unlock(&core->switch_lock);
		put_device(core->dev);
		kfree(backend);
		return ERR_PTR(existing ? -EEXIST : -ENODEV);
	}
	list_add_tail(&backend->node, &core->backends);
	mutex_unlock(&core->switch_lock);

	dev_info(dev, "registered %s backend with XPON runtime owner\n",
		 airoha_xpon_mode_descriptor(mode)->name);
	return backend;
}
EXPORT_SYMBOL_GPL(airoha_xpon_backend_register);

int airoha_xpon_backend_ready(struct airoha_xpon_backend *backend)
{
	struct airoha_xpon_core *core;
	int ret;

	if (!backend)
		return -EINVAL;
	core = backend->core;
	mutex_lock(&core->switch_lock);
	backend->ready = true;
	ret = airoha_xpon_core_claim_backend(backend);
	mutex_unlock(&core->switch_lock);
	if (!ret && airoha_xpon_backend_is_active(backend))
		dev_info(backend->dev, "claimed active %s mode\n",
			 airoha_xpon_mode_descriptor(backend->mode)->name);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xpon_backend_ready);

void airoha_xpon_backend_unregister(struct airoha_xpon_backend *backend)
{
	struct airoha_xpon_core *core;

	if (!backend)
		return;
	core = backend->core;
	mutex_lock(&core->switch_lock);
	backend->ready = false;
	if (core->current_backend == backend) {
		airoha_en7572_emergency_disable(core->bosa);
		if (airoha_xpon_backend_quiesce(backend))
			dev_err(backend->dev,
				"failed to quiesce active backend during unregister\n");
		WRITE_ONCE(core->current_backend, NULL);
	}
	list_del(&backend->node);
	mutex_unlock(&core->switch_lock);
	put_device(core->dev);
	kfree(backend);
}
EXPORT_SYMBOL_GPL(airoha_xpon_backend_unregister);

bool airoha_xpon_backend_is_active(struct airoha_xpon_backend *backend)
{
	return backend && READ_ONCE(backend->core->current_backend) == backend;
}
EXPORT_SYMBOL_GPL(airoha_xpon_backend_is_active);

static void airoha_xpon_bosa_put(void *data)
{
	airoha_en7572_put(data);
}

static int airoha_xpon_core_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_xpon_core *core;
	struct device_node *bosa_node;
	enum airoha_pcs_xpon_mode pcs_mode;
	int ret;

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;
	core->dev = dev;
	INIT_LIST_HEAD(&core->backends);
	mutex_init(&core->switch_lock);
	core->state.stage = AIROHA_XPON_SWITCH_IDLE;
	atomic_set(&core->dying_gasp_last_mode, -1);
	platform_set_drvdata(pdev, core);

	core->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(core->scu))
		return dev_err_probe(dev, PTR_ERR(core->scu),
				     "failed to map XPON SCU\n");
	core->dying_gasp_irq = platform_get_irq_byname(pdev, "dying-gasp");
	if (core->dying_gasp_irq < 0)
		return core->dying_gasp_irq;

	core->pcs = fwnode_pcs_get(dev_fwnode(dev), 0);
	if (IS_ERR(core->pcs))
		return dev_err_probe(dev, PTR_ERR(core->pcs),
				     "PON PCS is not ready\n");
	bosa_node = of_parse_phandle(dev->of_node, "bosa-controller", 0);
	if (!bosa_node)
		return dev_err_probe(dev, -EINVAL,
				     "missing bosa-controller phandle\n");
	core->bosa = airoha_en7572_get(bosa_node);
	of_node_put(bosa_node);
	if (IS_ERR(core->bosa))
		return dev_err_probe(dev, PTR_ERR(core->bosa),
				     "BOSA controller is not ready\n");
	ret = devm_add_action_or_reset(dev, airoha_xpon_bosa_put, core->bosa);
	if (ret)
		return ret;

	ret = airoha_pcs_xpon_get_mode(core->pcs, &pcs_mode);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read PON PCS mode\n");
	ret = airoha_xpon_from_pcs_mode(pcs_mode, &core->current_mode);
	if (ret)
		return dev_err_probe(dev, ret, "invalid PON PCS mode\n");
	if (!airoha_en7572_is_ready(core->bosa))
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "BOSA calibration is not ready\n");
	if (airoha_en7572_get_mode(core->bosa) != core->current_mode)
		return dev_err_probe(dev, -EINVAL,
				     "PCS and BOSA modes disagree\n");
	ret = airoha_en7572_set_tx_enabled(core->bosa, false);
	if (ret)
		return dev_err_probe(dev, ret, "failed to force TX disabled\n");
	ret = airoha_xpon_validate_runtime_mode(core, core->current_mode);
	if (ret)
		return dev_err_probe(dev, ret,
				     "PON hardware mode validation failed\n");
	ret = airoha_xpon_clear_dying_gasp(core);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to clear pending Dying Gasp status\n");
	ret = devm_request_irq(dev, core->dying_gasp_irq,
			       airoha_xpon_dying_gasp_irq, 0,
			       "airoha-xpon-dying-gasp", core);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request Dying Gasp IRQ\n");

	mutex_lock(&airoha_xpon_cores_lock);
	list_add_tail(&core->node, &airoha_xpon_cores);
	mutex_unlock(&airoha_xpon_cores_lock);
	dev_info(dev,
		 "runtime XPON owner initialized in %s mode; Dying Gasp IRQ %d armed, TX disabled\n",
		 airoha_xpon_mode_descriptor(core->current_mode)->name,
		 core->dying_gasp_irq);
	return 0;
}

static void airoha_xpon_core_remove(struct platform_device *pdev)
{
	struct airoha_xpon_core *core = platform_get_drvdata(pdev);

	mutex_lock(&airoha_xpon_cores_lock);
	core->removing = true;
	list_del(&core->node);
	mutex_unlock(&airoha_xpon_cores_lock);
	airoha_en7572_emergency_disable(core->bosa);
	WARN_ON(!list_empty(&core->backends));
}

static void airoha_xpon_core_shutdown(struct platform_device *pdev)
{
	struct airoha_xpon_core *core = platform_get_drvdata(pdev);

	airoha_en7572_emergency_disable(core->bosa);
}

static const struct of_device_id airoha_xpon_core_of_match[] = {
	{ .compatible = "airoha,en7581-xpon-core" },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_xpon_core_of_match);

static struct platform_driver airoha_xpon_core_driver = {
	.probe = airoha_xpon_core_probe,
	.remove = airoha_xpon_core_remove,
	.shutdown = airoha_xpon_core_shutdown,
	.driver = {
		.name = "airoha-xpon-core",
		.of_match_table = airoha_xpon_core_of_match,
		.dev_groups = airoha_xpon_core_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(airoha_xpon_core_driver);

MODULE_AUTHOR("OpenWrt project");
MODULE_DESCRIPTION("Airoha EN7581 runtime XPON mode owner");
MODULE_LICENSE("GPL");
