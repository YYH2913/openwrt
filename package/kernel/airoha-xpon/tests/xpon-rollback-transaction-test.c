// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>

#include "../src/airoha-xpon-rollback-transaction.h"

#define TEST_ERROR (-123)
#define CLEANUP_ERROR (-124)

struct simulation {
	unsigned int operation;
	unsigned int fail_at;
	unsigned int disable_tx_count;
	unsigned int target_cleanup_count;
	unsigned int previous_cleanup_count;
	bool fail_previous_cleanup;
	bool expect_target_cleanup;
	bool pcs_order_violation;
	bool owner_committed;
	bool mode_validated;
	bool owner_commit_order_violation;
	enum airoha_xpon_mode mode;
};

static int operation(struct simulation *simulation)
{
	simulation->operation++;
	return simulation->operation == simulation->fail_at ? TEST_ERROR : 0;
}

static void disable_tx(void *context)
{
	struct simulation *simulation = context;

	simulation->disable_tx_count++;
}

static int quiesce_target(void *context)
{
	struct simulation *simulation = context;

	simulation->target_cleanup_count++;
	return operation(simulation);
}

static int plain_operation(void *context)
{
	return operation(context);
}

static int quiesce_pcs(void *context)
{
	struct simulation *simulation = context;

	if (simulation->expect_target_cleanup) {
		if (!simulation->target_cleanup_count)
			simulation->pcs_order_violation = true;
	} else if (!simulation->previous_cleanup_count) {
		simulation->pcs_order_violation = true;
	}
	return operation(simulation);
}

static int mode_operation(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;

	if (!airoha_xpon_mode_valid(mode))
		return -EINVAL;
	return operation(simulation);
}

static int validate_previous(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;
	int ret;

	if (!airoha_xpon_mode_valid(mode))
		return -EINVAL;
	ret = operation(simulation);
	if (!ret)
		simulation->mode_validated = true;
	return ret;
}

static void commit_owner(void *context, enum airoha_xpon_mode mode)
{
	struct simulation *simulation = context;

	if (!simulation->mode_validated)
		simulation->owner_commit_order_violation = true;
	simulation->owner_committed = true;
	simulation->mode = mode;
}

static int quiesce_previous(void *context)
{
	struct simulation *simulation = context;

	simulation->previous_cleanup_count++;
	return simulation->fail_previous_cleanup ? CLEANUP_ERROR : 0;
}

static const struct airoha_xpon_rollback_ops operations = {
	.disable_tx = disable_tx,
	.quiesce_target = quiesce_target,
	.quiesce_pcs = quiesce_pcs,
	.select_previous = mode_operation,
	.configure_previous = mode_operation,
	.load_previous_calibration = mode_operation,
	.recover_pcs = plain_operation,
	.start_previous_mac = plain_operation,
	.start_previous_datapath = plain_operation,
	.validate_previous = validate_previous,
	.commit_previous_owner = commit_owner,
	.unmask_previous_irqs = plain_operation,
	.resume_previous = plain_operation,
	.quiesce_previous = quiesce_previous,
};

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static int check_success(bool target_started)
{
	struct simulation simulation = {
		.expect_target_cleanup = target_started,
		.mode = AIROHA_XPON_MODE_EPON_10G_10G,
	};
	unsigned int expected_operations = target_started ? 11 : 10;
	int ret;

	ret = airoha_xpon_rollback_transaction(&operations, &simulation,
		AIROHA_XPON_MODE_XGSPON, target_started);
	CHECK(!ret);
	CHECK(simulation.operation == expected_operations);
	CHECK(simulation.disable_tx_count == 1);
	CHECK(simulation.target_cleanup_count == (unsigned int)target_started);
	CHECK(simulation.previous_cleanup_count == (unsigned int)!target_started);
	CHECK(!simulation.pcs_order_violation);
	CHECK(simulation.owner_committed);
	CHECK(simulation.mode_validated);
	CHECK(!simulation.owner_commit_order_violation);
	CHECK(simulation.mode == AIROHA_XPON_MODE_XGSPON);
	return 0;
}

static int check_every_failure(bool target_started)
{
	unsigned int fail_at, operations_count = target_started ? 11 : 10;
	unsigned int start_operation = target_started ? 7 : 6;

	for (fail_at = 1; fail_at <= operations_count; fail_at++) {
		struct simulation simulation = {
			.fail_at = fail_at,
			.expect_target_cleanup = target_started,
			.mode = AIROHA_XPON_MODE_EPON_10G_1G,
		};
		bool previous_start_attempted = fail_at >= start_operation;
		int ret;

		ret = airoha_xpon_rollback_transaction(&operations, &simulation,
			AIROHA_XPON_MODE_XGPON, target_started);
		CHECK(ret == TEST_ERROR);
		CHECK(simulation.operation == fail_at);
		CHECK(simulation.target_cleanup_count ==
		      (unsigned int)target_started);
		CHECK(simulation.previous_cleanup_count ==
		      (unsigned int)(!target_started) +
		      (unsigned int)previous_start_attempted);
		CHECK(!simulation.pcs_order_violation);
		CHECK(simulation.disable_tx_count ==
		      (previous_start_attempted ? 2U : 1U));
		CHECK(simulation.owner_committed ==
		      (fail_at > start_operation + 2));
		CHECK(!simulation.owner_commit_order_violation);
	}
	return 0;
}

static int check_cleanup_failure_preserves_original(void)
{
	struct simulation simulation = {
		.fail_at = 7,
		.fail_previous_cleanup = true,
		.expect_target_cleanup = true,
	};

	CHECK(airoha_xpon_rollback_transaction(&operations, &simulation,
		AIROHA_XPON_MODE_XGSPON, true) == TEST_ERROR);
	CHECK(simulation.previous_cleanup_count == 1);
	CHECK(simulation.disable_tx_count == 2);
	return 0;
}

static int check_early_cleanup_failure_stops_before_pcs(void)
{
	struct simulation simulation = {
		.fail_previous_cleanup = true,
	};

	CHECK(airoha_xpon_rollback_transaction(&operations, &simulation,
		AIROHA_XPON_MODE_XGPON, false) == CLEANUP_ERROR);
	CHECK(simulation.operation == 0);
	CHECK(simulation.previous_cleanup_count == 1);
	CHECK(simulation.target_cleanup_count == 0);
	CHECK(!simulation.pcs_order_violation);
	CHECK(!simulation.owner_committed);
	return 0;
}

int main(void)
{
	if (check_success(false) || check_success(true) ||
	    check_every_failure(false) || check_every_failure(true) ||
	    check_cleanup_failure_preserves_original() ||
	    check_early_cleanup_failure_stops_before_pcs())
		return 1;

	puts("XPON production rollback callback matrix: OK");
	return 0;
}
