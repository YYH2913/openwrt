/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_SWITCH_TRANSACTION_H_
#define _AIROHA_XPON_SWITCH_TRANSACTION_H_

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

#include "airoha-xpon-mode.h"

enum airoha_xpon_switch_stage {
	AIROHA_XPON_SWITCH_IDLE = 0,
	AIROHA_XPON_SWITCH_TRAFFIC_BLOCKED,
	AIROHA_XPON_SWITCH_TX_DISABLED,
	AIROHA_XPON_SWITCH_IRQS_MASKED,
	AIROHA_XPON_SWITCH_SESSION_CLEARED,
	AIROHA_XPON_SWITCH_DATAPATH_STOPPED,
	AIROHA_XPON_SWITCH_MAC_STOPPED,
	AIROHA_XPON_SWITCH_PCS_QUIESCED,
	AIROHA_XPON_SWITCH_WAN_SELECTED,
	AIROHA_XPON_SWITCH_PMA_CONFIGURED,
	AIROHA_XPON_SWITCH_CALIBRATION_LOADED,
	AIROHA_XPON_SWITCH_PCS_RECOVERED,
	AIROHA_XPON_SWITCH_MAC_STARTED,
	AIROHA_XPON_SWITCH_DATAPATH_STARTED,
	AIROHA_XPON_SWITCH_HARDWARE_VALIDATED,
	AIROHA_XPON_SWITCH_OWNER_COMMITTED,
	AIROHA_XPON_SWITCH_IRQS_UNMASKED,
	AIROHA_XPON_SWITCH_COMMITTED,
};

struct airoha_xpon_switch_state {
	enum airoha_xpon_mode previous_mode;
	enum airoha_xpon_mode target_mode;
	enum airoha_xpon_switch_stage stage;
	enum airoha_xpon_switch_stage failed_stage;
	bool tx_disabled;
	bool rollback_failed;
	bool committed;
};

struct airoha_xpon_switch_ops {
	int (*block_traffic)(void *context);
	void (*disable_tx)(void *context);
	bool (*tx_is_disabled)(void *context);
	int (*clear_protocol_session)(void *context);
	int (*mask_irqs)(void *context);
	void (*synchronize_irqs)(void *context);
	int (*stop_datapath)(void *context);
	int (*stop_mac)(void *context);
	int (*quiesce_pcs)(void *context);
	int (*select_wan)(void *context, enum airoha_xpon_mode mode);
	int (*configure_pma)(void *context, enum airoha_xpon_mode mode);
	int (*load_calibration)(void *context, enum airoha_xpon_mode mode);
	int (*recover_pcs)(void *context);
	int (*start_mac)(void *context, enum airoha_xpon_mode mode);
	int (*start_datapath)(void *context, enum airoha_xpon_mode mode);
	int (*validate_mode)(void *context, enum airoha_xpon_mode mode);
	void (*commit_owner)(void *context, enum airoha_xpon_mode mode);
	int (*unmask_irqs)(void *context, enum airoha_xpon_mode mode);
	int (*activate_mode)(void *context, enum airoha_xpon_mode mode);
	int (*rollback)(void *context, enum airoha_xpon_mode previous_mode,
			enum airoha_xpon_switch_stage failed_stage);
	void (*fault_lock)(void *context);
};

static inline bool
airoha_xpon_switch_ops_valid(const struct airoha_xpon_switch_ops *ops)
{
	return ops && ops->block_traffic && ops->disable_tx &&
	       ops->tx_is_disabled && ops->clear_protocol_session &&
	       ops->mask_irqs && ops->synchronize_irqs && ops->stop_datapath &&
	       ops->stop_mac && ops->quiesce_pcs && ops->select_wan &&
	       ops->configure_pma && ops->load_calibration && ops->recover_pcs &&
	       ops->start_mac && ops->start_datapath && ops->validate_mode &&
	       ops->commit_owner && ops->unmask_irqs && ops->activate_mode &&
	       ops->rollback && ops->fault_lock;
}

static inline int
airoha_xpon_switch_fail(const struct airoha_xpon_switch_ops *ops,
			void *context,
			struct airoha_xpon_switch_state *state,
			int original_error,
			enum airoha_xpon_switch_stage failed_stage)
{
	int rollback_error;

	state->failed_stage = failed_stage;
	/* Every error path reasserts the hardware TX-disable line first. */
	ops->disable_tx(context);
	state->tx_disabled = ops->tx_is_disabled(context);
	rollback_error = ops->rollback(context, state->previous_mode,
				       failed_stage);
	ops->disable_tx(context);
	state->tx_disabled = ops->tx_is_disabled(context);
	if (rollback_error || !state->tx_disabled) {
		state->rollback_failed = true;
		ops->fault_lock(context);
		return rollback_error ? rollback_error : -EIO;
	}

	return original_error;
}

#define AIROHA_XPON_SWITCH_STEP(_callback, _stage) do { \
	ret = (_callback); \
	if (ret) \
		return airoha_xpon_switch_fail(ops, context, state, ret, (_stage)); \
	state->stage = (_stage); \
} while (0)

static inline bool airoha_xpon_switch_target_start_attempted(
		enum airoha_xpon_switch_stage failed_stage)
{
	return failed_stage >= AIROHA_XPON_SWITCH_MAC_STARTED &&
	       failed_stage <= AIROHA_XPON_SWITCH_COMMITTED;
}

static inline bool airoha_xpon_switch_previous_cleanup_required(
		enum airoha_xpon_switch_stage failed_stage)
{
	/* A failed stop callback can leave the previous backend partly live. */
	return failed_stage < AIROHA_XPON_SWITCH_PCS_QUIESCED;
}

/*
 * Hardware reconfiguration keeps optical TX disabled through the ownership
 * commit. Shared IRQ handlers gate on that owner, so commit it before unmasking
 * the target IRQs. activate_mode() may then let the new protocol owner start
 * activation, subject to its registration and rogue-ONU gates.
 */
static inline int
airoha_xpon_switch_transaction(const struct airoha_xpon_switch_ops *ops,
			       void *context,
			       enum airoha_xpon_mode previous_mode,
			       enum airoha_xpon_mode target_mode,
			       struct airoha_xpon_switch_state *state)
{
	int ret;

	if (!state || !airoha_xpon_switch_ops_valid(ops) ||
	    !airoha_xpon_mode_valid(previous_mode) ||
	    !airoha_xpon_mode_valid(target_mode))
		return -EINVAL;
	if (previous_mode == target_mode)
		return -EALREADY;

	state->previous_mode = previous_mode;
	state->target_mode = target_mode;
	state->stage = AIROHA_XPON_SWITCH_IDLE;
	state->failed_stage = AIROHA_XPON_SWITCH_IDLE;
	state->tx_disabled = false;
	state->rollback_failed = false;
	state->committed = false;

	ret = ops->block_traffic(context);
	/* TX must become safe even when blocking new traffic itself fails. */
	ops->disable_tx(context);
	state->tx_disabled = ops->tx_is_disabled(context);
	if (ret)
		return airoha_xpon_switch_fail(ops, context, state, ret,
			AIROHA_XPON_SWITCH_TRAFFIC_BLOCKED);
	state->stage = AIROHA_XPON_SWITCH_TRAFFIC_BLOCKED;
	if (!state->tx_disabled)
		return airoha_xpon_switch_fail(ops, context, state, -EIO,
			AIROHA_XPON_SWITCH_TX_DISABLED);
	state->stage = AIROHA_XPON_SWITCH_TX_DISABLED;

	AIROHA_XPON_SWITCH_STEP(ops->mask_irqs(context),
				AIROHA_XPON_SWITCH_IRQS_MASKED);
	ops->synchronize_irqs(context);
	/* No old-protocol IRQ may repopulate state after it is cleared. */
	AIROHA_XPON_SWITCH_STEP(ops->clear_protocol_session(context),
				AIROHA_XPON_SWITCH_SESSION_CLEARED);
	AIROHA_XPON_SWITCH_STEP(ops->stop_datapath(context),
				AIROHA_XPON_SWITCH_DATAPATH_STOPPED);
	AIROHA_XPON_SWITCH_STEP(ops->stop_mac(context),
				AIROHA_XPON_SWITCH_MAC_STOPPED);
	AIROHA_XPON_SWITCH_STEP(ops->quiesce_pcs(context),
				AIROHA_XPON_SWITCH_PCS_QUIESCED);
	AIROHA_XPON_SWITCH_STEP(ops->select_wan(context, target_mode),
				AIROHA_XPON_SWITCH_WAN_SELECTED);
	AIROHA_XPON_SWITCH_STEP(ops->configure_pma(context, target_mode),
				AIROHA_XPON_SWITCH_PMA_CONFIGURED);
	AIROHA_XPON_SWITCH_STEP(ops->load_calibration(context, target_mode),
				AIROHA_XPON_SWITCH_CALIBRATION_LOADED);
	AIROHA_XPON_SWITCH_STEP(ops->recover_pcs(context),
				AIROHA_XPON_SWITCH_PCS_RECOVERED);
	AIROHA_XPON_SWITCH_STEP(ops->start_mac(context, target_mode),
				AIROHA_XPON_SWITCH_MAC_STARTED);
	AIROHA_XPON_SWITCH_STEP(ops->start_datapath(context, target_mode),
				AIROHA_XPON_SWITCH_DATAPATH_STARTED);

	if (!ops->tx_is_disabled(context))
		return airoha_xpon_switch_fail(ops, context, state, -EIO,
			state->stage);
	AIROHA_XPON_SWITCH_STEP(ops->validate_mode(context, target_mode),
				AIROHA_XPON_SWITCH_HARDWARE_VALIDATED);
	ops->commit_owner(context, target_mode);
	state->stage = AIROHA_XPON_SWITCH_OWNER_COMMITTED;
	AIROHA_XPON_SWITCH_STEP(ops->unmask_irqs(context, target_mode),
				AIROHA_XPON_SWITCH_IRQS_UNMASKED);
	AIROHA_XPON_SWITCH_STEP(ops->activate_mode(context, target_mode),
					AIROHA_XPON_SWITCH_COMMITTED);
	state->tx_disabled = ops->tx_is_disabled(context);
	state->committed = true;
	return 0;
}

#undef AIROHA_XPON_SWITCH_STEP

#endif
