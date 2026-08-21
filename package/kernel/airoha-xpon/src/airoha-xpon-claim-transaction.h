/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_CLAIM_TRANSACTION_H_
#define _AIROHA_XPON_CLAIM_TRANSACTION_H_

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

struct airoha_xpon_claim_ops {
	bool (*tx_is_disabled)(void *context);
	int (*start_mac)(void *context);
	int (*start_datapath)(void *context);
	int (*validate_mode)(void *context);
	void (*commit_owner)(void *context);
	int (*unmask_irqs)(void *context);
	int (*activate_mode)(void *context);
	void (*disable_tx)(void *context);
	int (*quiesce_backend)(void *context);
	void (*clear_owner)(void *context);
};

static inline bool
airoha_xpon_claim_ops_valid(const struct airoha_xpon_claim_ops *ops)
{
	return ops && ops->tx_is_disabled && ops->start_mac &&
	       ops->start_datapath && ops->validate_mode && ops->commit_owner &&
	       ops->unmask_irqs && ops->activate_mode && ops->disable_tx &&
	       ops->quiesce_backend && ops->clear_owner;
}

static inline int
airoha_xpon_claim_fail(const struct airoha_xpon_claim_ops *ops,
		       void *context, int error, bool owner_committed)
{
	ops->disable_tx(context);
	ops->quiesce_backend(context);
	if (owner_committed)
		ops->clear_owner(context);
	return error;
}

/*
 * A backend may claim the already-selected PCS/PMA mode only after its probe
 * has installed every IRQ and userspace endpoint. Optical TX remains disabled
 * throughout the claim, and the owner is visible before IRQs are unmasked.
 */
static inline int
airoha_xpon_claim_transaction(const struct airoha_xpon_claim_ops *ops,
			      void *context)
{
	bool owner_committed = false;
	int ret;

	if (!airoha_xpon_claim_ops_valid(ops))
		return -EINVAL;
	if (!ops->tx_is_disabled(context))
		return airoha_xpon_claim_fail(ops, context, -EIO, false);

	ret = ops->start_mac(context);
	if (ret)
		return airoha_xpon_claim_fail(ops, context, ret, false);
	ret = ops->start_datapath(context);
	if (ret)
		return airoha_xpon_claim_fail(ops, context, ret, false);
	if (!ops->tx_is_disabled(context))
		return airoha_xpon_claim_fail(ops, context, -EIO, false);
	ret = ops->validate_mode(context);
	if (ret)
		return airoha_xpon_claim_fail(ops, context, ret, false);

	ops->commit_owner(context);
	owner_committed = true;
	ret = ops->unmask_irqs(context);
	if (ret)
		return airoha_xpon_claim_fail(ops, context, ret,
					       owner_committed);
	ret = ops->activate_mode(context);
	if (ret)
		return airoha_xpon_claim_fail(ops, context, ret,
					       owner_committed);
	if (!ops->tx_is_disabled(context))
		return airoha_xpon_claim_fail(ops, context, -EIO,
					       owner_committed);

	return 0;
}

#endif
