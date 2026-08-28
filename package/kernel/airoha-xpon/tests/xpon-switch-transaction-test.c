// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-xpon-switch-transaction.h"

#define TEST_ERROR (-123)
#define ROLLBACK_ERROR (-124)

static const enum airoha_xpon_mode switch_modes[] = {
	AIROHA_XPON_MODE_GPON,
	AIROHA_XPON_MODE_XGPON,
	AIROHA_XPON_MODE_XGSPON,
	AIROHA_XPON_MODE_EPON_10G_1G,
	AIROHA_XPON_MODE_EPON_10G_10G,
};

struct simulation {
	unsigned int operation;
	unsigned int fail_at;
	bool fail_rollback;
	bool defeat_tx_disable;
	bool tx_disabled;
	bool fault_locked;
	bool committed;
	bool owner_committed;
	bool mode_validated;
	bool owner_commit_order_violation;
	bool irq_unmask_saw_target_owner;
	bool irqs_masked;
	bool irqs_synchronized;
	bool session_clear_order_violation;
	bool enable_tx_on_commit;
	unsigned int owner_commit_count;
	enum airoha_xpon_mode owner_mode;
	enum airoha_xpon_mode mode;
	enum airoha_xpon_switch_stage rollback_stage;
};

static int operation(struct simulation *simulation)
{
	simulation->operation++;
	return simulation->operation == simulation->fail_at ? TEST_ERROR : 0;
}

static int plain_operation(void *context)
{
	return operation(context);
}

static int mask_irqs(void *context)
{
	struct simulation *simulation = context;
	int ret = operation(simulation);

	if (!ret)
		simulation->irqs_masked = true;
	return ret;
}

static int clear_protocol_session(void *context)
{
	struct simulation *simulation = context;

	if (!simulation->irqs_masked || !simulation->irqs_synchronized)
		simulation->session_clear_order_violation = true;
	return operation(simulation);
}

static int mode_operation(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;
	int ret = operation(simulation);

	if (!ret)
		simulation->mode = mode;
	return ret;
}

static int validate_mode(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;
	int ret = operation(simulation);

	if (ret)
		return ret;
	if (simulation->mode != mode)
		return -EINVAL;
	simulation->mode_validated = true;
	return 0;
}

static void disable_tx(void *context)
{
	struct simulation *simulation = context;

	if (!simulation->defeat_tx_disable)
		simulation->tx_disabled = true;
}

static bool tx_is_disabled(void *context)
{
	struct simulation *simulation = context;

	return simulation->tx_disabled;
}

static void synchronize_irqs(void *context)
{
	struct simulation *simulation = context;

	if (simulation->irqs_masked)
		simulation->irqs_synchronized = true;
}

static void commit_owner(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;

	if (!simulation->mode_validated)
		simulation->owner_commit_order_violation = true;
	simulation->owner_committed = true;
	simulation->owner_commit_count++;
	simulation->owner_mode = mode;
}

static int unmask_irqs(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;

	simulation->irq_unmask_saw_target_owner =
		simulation->owner_committed && simulation->owner_mode == mode;
	return operation(simulation);
}

static int activate_mode(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;
	int ret = operation(simulation);

	if (ret || !simulation->owner_committed ||
	    simulation->owner_mode != mode)
		return ret ? ret : -EINVAL;

	simulation->committed = true;
	simulation->mode = mode;
	if (simulation->enable_tx_on_commit)
		simulation->tx_disabled = false;
	return 0;
}

static int rollback(void *context, enum airoha_xpon_mode mode,
		    enum airoha_xpon_switch_stage stage)
{
	struct simulation *simulation = context;

	simulation->rollback_stage = stage;
	simulation->mode = mode;
	simulation->owner_committed = false;
	simulation->mode_validated = false;
	simulation->owner_mode = mode;
	return simulation->fail_rollback ? ROLLBACK_ERROR : 0;
}

static void fault_lock(void *context)
{
	struct simulation *simulation = context;

	simulation->fault_locked = true;
	simulation->owner_committed = false;
	simulation->owner_mode = AIROHA_XPON_MODE_INVALID;
}

static const struct airoha_xpon_switch_ops operations = {
	.block_traffic = plain_operation,
	.disable_tx = disable_tx,
	.tx_is_disabled = tx_is_disabled,
	.clear_protocol_session = clear_protocol_session,
	.mask_irqs = mask_irqs,
	.synchronize_irqs = synchronize_irqs,
	.stop_datapath = plain_operation,
	.stop_mac = plain_operation,
	.quiesce_pcs = plain_operation,
	.select_wan = mode_operation,
	.configure_pma = mode_operation,
	.load_calibration = mode_operation,
	.recover_pcs = plain_operation,
	.start_mac = mode_operation,
	.start_datapath = mode_operation,
	.validate_mode = validate_mode,
	.commit_owner = commit_owner,
	.unmask_irqs = unmask_irqs,
	.activate_mode = activate_mode,
	.rollback = rollback,
	.fault_lock = fault_lock,
};

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static int check_descriptors(void)
{
	static const unsigned int wan_sel[] = { 0, 9, 10, 6, 7 };
	static const char * const names[] = {
		"gpon", "xgpon", "xgspon", "epon-10g-1g", "epon-10g-10g",
	};
	unsigned int mode;

	for (mode = 0; mode < AIROHA_XPON_MODE_COUNT; mode++) {
		const struct airoha_xpon_mode_descriptor *descriptor;

		descriptor = airoha_xpon_mode_descriptor(mode);
		CHECK(descriptor != NULL);
		CHECK(descriptor->wan_sel == wan_sel[mode]);
		CHECK(!strcmp(descriptor->name, names[mode]));
		CHECK(descriptor->downstream_kbps > 0);
		CHECK(descriptor->upstream_kbps > 0);
	}
	CHECK(airoha_xpon_mode_uses_omci(AIROHA_XPON_MODE_XGPON));
	CHECK(airoha_xpon_mode_uses_omci(AIROHA_XPON_MODE_XGSPON));
	CHECK(!airoha_xpon_mode_uses_omci(AIROHA_XPON_MODE_EPON_10G_1G));
	CHECK(!airoha_xpon_mode_descriptor(AIROHA_XPON_MODE_INVALID));
	return 0;
}

static int check_success_matrix(void)
{
	unsigned int previous, target;

	for (previous = 0; previous < sizeof(switch_modes) /
					     sizeof(switch_modes[0]); previous++) {
		for (target = 0; target < sizeof(switch_modes) /
					 sizeof(switch_modes[0]); target++) {
			struct airoha_xpon_switch_state state;
			struct simulation simulation = {
				.mode = switch_modes[previous],
			};
			int ret;

			if (previous == target)
				continue;
			ret = airoha_xpon_switch_transaction(&operations,
				&simulation, switch_modes[previous],
				switch_modes[target], &state);
			CHECK(!ret);
			CHECK(simulation.operation == 15);
			CHECK(simulation.tx_disabled);
			CHECK(!simulation.fault_locked);
			CHECK(simulation.owner_commit_count == 1);
			CHECK(!simulation.owner_commit_order_violation);
			CHECK(simulation.irq_unmask_saw_target_owner);
			CHECK(!simulation.session_clear_order_violation);
			CHECK(simulation.committed && state.committed);
			CHECK(state.stage == AIROHA_XPON_SWITCH_COMMITTED);
			CHECK(state.failed_stage == AIROHA_XPON_SWITCH_IDLE);
			CHECK(simulation.mode == switch_modes[target]);
		}
	}
	return 0;
}

static int check_every_operation_failure(void)
{
	static const enum airoha_xpon_switch_stage last_completed_stage[] = {
		AIROHA_XPON_SWITCH_IDLE,
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
		AIROHA_XPON_SWITCH_OWNER_COMMITTED,
		AIROHA_XPON_SWITCH_IRQS_UNMASKED,
	};
	static const enum airoha_xpon_switch_stage failed_stage[] = {
		AIROHA_XPON_SWITCH_TRAFFIC_BLOCKED,
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
		AIROHA_XPON_SWITCH_IRQS_UNMASKED,
		AIROHA_XPON_SWITCH_COMMITTED,
	};
	unsigned int previous, target, fail_at;

	for (previous = 0; previous < sizeof(switch_modes) /
					     sizeof(switch_modes[0]); previous++) {
		for (target = 0; target < sizeof(switch_modes) /
					 sizeof(switch_modes[0]); target++) {
			if (previous == target)
				continue;
			for (fail_at = 1; fail_at <= 15; fail_at++) {
				struct airoha_xpon_switch_state state;
				struct simulation simulation = {
					.fail_at = fail_at,
					.mode = switch_modes[previous],
				};
				int ret;

				ret = airoha_xpon_switch_transaction(&operations,
					&simulation, switch_modes[previous],
					switch_modes[target], &state);
				CHECK(ret == TEST_ERROR);
				CHECK(state.stage == last_completed_stage[fail_at - 1]);
				CHECK(state.failed_stage == failed_stage[fail_at - 1]);
				CHECK(simulation.rollback_stage ==
				      failed_stage[fail_at - 1]);
				CHECK(simulation.tx_disabled && state.tx_disabled);
				CHECK(!simulation.committed && !state.committed);
				CHECK(!simulation.session_clear_order_violation);
				CHECK(simulation.owner_commit_count ==
				      (fail_at >= 14 ? 1U : 0U));
				CHECK(simulation.irq_unmask_saw_target_owner ==
				      (fail_at >= 14));
				CHECK(!simulation.owner_commit_order_violation);
				CHECK(!simulation.fault_locked &&
				      !state.rollback_failed);
				CHECK(simulation.mode == switch_modes[previous]);
			}
		}
	}
	return 0;
}

static int check_rollback_cleanup_policy(void)
{
	enum airoha_xpon_switch_stage stage;

	for (stage = AIROHA_XPON_SWITCH_IDLE;
	     stage <= AIROHA_XPON_SWITCH_COMMITTED; stage++) {
		CHECK(airoha_xpon_switch_target_start_attempted(stage) ==
		      (stage >= AIROHA_XPON_SWITCH_MAC_STARTED));
		CHECK(airoha_xpon_switch_previous_cleanup_required(stage) ==
		      (stage < AIROHA_XPON_SWITCH_PCS_QUIESCED));
		CHECK(!(airoha_xpon_switch_target_start_attempted(stage) &&
			airoha_xpon_switch_previous_cleanup_required(stage)));
	}
	return 0;
}

static int check_same_mode_rejected(void)
{
	unsigned int mode;

	for (mode = 0; mode < sizeof(switch_modes) /
				  sizeof(switch_modes[0]); mode++) {
		struct airoha_xpon_switch_state state;
		struct simulation simulation = {
			.mode = switch_modes[mode],
		};

		CHECK(airoha_xpon_switch_transaction(&operations, &simulation,
			switch_modes[mode], switch_modes[mode], &state) ==
		      -EALREADY);
		CHECK(simulation.operation == 0);
	}
	return 0;
}

static int check_protocol_activation_after_commit(void)
{
	struct airoha_xpon_switch_state state;
	struct simulation simulation = {
		.enable_tx_on_commit = true,
		.mode = AIROHA_XPON_MODE_XGPON,
	};

	CHECK(!airoha_xpon_switch_transaction(&operations, &simulation,
		AIROHA_XPON_MODE_XGPON, AIROHA_XPON_MODE_XGSPON, &state));
	CHECK(simulation.committed && state.committed);
	CHECK(!simulation.tx_disabled && !state.tx_disabled);
	CHECK(!simulation.fault_locked && !state.rollback_failed);
	return 0;
}

static int check_rollback_failure(void)
{
	struct airoha_xpon_switch_state state;
	struct simulation simulation = {
		.fail_at = 8,
		.fail_rollback = true,
		.mode = AIROHA_XPON_MODE_XGPON,
	};
	int ret;

	ret = airoha_xpon_switch_transaction(&operations, &simulation,
		AIROHA_XPON_MODE_XGPON, AIROHA_XPON_MODE_EPON_10G_1G, &state);
	CHECK(ret == ROLLBACK_ERROR);
	CHECK(simulation.tx_disabled && state.tx_disabled);
	CHECK(simulation.fault_locked && state.rollback_failed);
	CHECK(!simulation.owner_committed);
	CHECK(simulation.owner_mode == AIROHA_XPON_MODE_INVALID);
	CHECK(!simulation.committed);
	CHECK(state.failed_stage == AIROHA_XPON_SWITCH_PMA_CONFIGURED);
	CHECK(simulation.rollback_stage == AIROHA_XPON_SWITCH_PMA_CONFIGURED);
	return 0;
}

static int check_broken_tx_disable(void)
{
	struct airoha_xpon_switch_state state;
	struct simulation simulation = {
		.defeat_tx_disable = true,
		.mode = AIROHA_XPON_MODE_XGSPON,
	};
	int ret;

	ret = airoha_xpon_switch_transaction(&operations, &simulation,
		AIROHA_XPON_MODE_XGSPON, AIROHA_XPON_MODE_XGPON, &state);
	CHECK(ret == -EIO);
	CHECK(!simulation.tx_disabled && !state.tx_disabled);
	CHECK(simulation.fault_locked && state.rollback_failed);
	CHECK(!simulation.committed);
	CHECK(state.failed_stage == AIROHA_XPON_SWITCH_TX_DISABLED);
	CHECK(simulation.rollback_stage == AIROHA_XPON_SWITCH_TX_DISABLED);
	return 0;
}

int main(void)
{
	if (check_descriptors() || check_success_matrix() ||
	    check_every_operation_failure() || check_rollback_failure() ||
	    check_broken_tx_disable() || check_same_mode_rejected() ||
	    check_rollback_cleanup_policy() ||
	    check_protocol_activation_after_commit())
		return 1;

	puts("XPON 20-way directed switch matrix and fail-closed transaction: OK");
	return 0;
}
