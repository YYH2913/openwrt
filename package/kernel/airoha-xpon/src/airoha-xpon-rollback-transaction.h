/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_ROLLBACK_TRANSACTION_H_
#define _AIROHA_XPON_ROLLBACK_TRANSACTION_H_

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

#include "airoha-xpon-mode.h"

struct airoha_xpon_rollback_ops {
	void (*disable_tx)(void *context);
	int (*quiesce_target)(void *context);
	int (*quiesce_pcs)(void *context);
	int (*select_previous)(void *context, enum airoha_xpon_mode mode);
	int (*configure_previous)(void *context, enum airoha_xpon_mode mode);
	int (*load_previous_calibration)(void *context,
					 enum airoha_xpon_mode mode);
	int (*recover_pcs)(void *context);
	int (*start_previous_mac)(void *context);
	int (*start_previous_datapath)(void *context);
	int (*validate_previous)(void *context, enum airoha_xpon_mode mode);
	void (*commit_previous_owner)(void *context,
				      enum airoha_xpon_mode mode);
	int (*unmask_previous_irqs)(void *context);
	int (*resume_previous)(void *context);
	int (*quiesce_previous)(void *context);
};

static inline bool airoha_xpon_rollback_ops_valid(
		const struct airoha_xpon_rollback_ops *ops)
{
	return ops && ops->disable_tx && ops->quiesce_target &&
	       ops->quiesce_pcs && ops->select_previous &&
	       ops->configure_previous && ops->load_previous_calibration &&
	       ops->recover_pcs && ops->start_previous_mac &&
	       ops->start_previous_datapath && ops->validate_previous &&
	       ops->commit_previous_owner && ops->unmask_previous_irqs &&
	       ops->resume_previous &&
	       ops->quiesce_previous;
}

static inline int airoha_xpon_rollback_transaction(
		const struct airoha_xpon_rollback_ops *ops, void *context,
		enum airoha_xpon_mode previous_mode, bool target_start_attempted)
{
	int cleanup_error, ret;

	if (!airoha_xpon_rollback_ops_valid(ops) ||
	    !airoha_xpon_mode_valid(previous_mode))
		return -EINVAL;

	ops->disable_tx(context);
	if (target_start_attempted) {
		ret = ops->quiesce_target(context);
	} else {
		/* Early failures may leave the old MAC, data path or IRQs live. */
		ret = ops->quiesce_previous(context);
	}
	if (ret)
		return ret;
	ret = ops->quiesce_pcs(context);
	if (ret)
		return ret;
	ret = ops->select_previous(context, previous_mode);
	if (ret)
		return ret;
	ret = ops->configure_previous(context, previous_mode);
	if (ret)
		return ret;
	ret = ops->load_previous_calibration(context, previous_mode);
	if (ret)
		return ret;
	ret = ops->recover_pcs(context);
	if (ret)
		return ret;

	/* A failed start callback may already have modified the old MAC. */
	ret = ops->start_previous_mac(context);
	if (ret)
		goto previous_failed;
	ret = ops->start_previous_datapath(context);
	if (ret)
		goto previous_failed;
	ret = ops->validate_previous(context, previous_mode);
	if (ret)
		goto previous_failed;

	/* Shared IRQ dispatch must see the restored owner before unmasking. */
	ops->commit_previous_owner(context, previous_mode);
	ret = ops->unmask_previous_irqs(context);
	if (ret)
		goto previous_failed;
	ret = ops->resume_previous(context);
	if (ret)
		goto previous_failed;

	return 0;

previous_failed:
	ops->disable_tx(context);
	cleanup_error = ops->quiesce_previous(context);
	return ret ? ret : cleanup_error;
}

#endif
