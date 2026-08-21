// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>

#include "../src/airoha-xpon-claim-transaction.h"

#define TEST_ERROR (-123)

struct simulation {
	unsigned int operation;
	unsigned int fail_at;
	unsigned int disable_count;
	unsigned int quiesce_count;
	unsigned int clear_count;
	bool tx_disabled;
	bool owner_committed;
	bool mode_validated;
	bool owner_commit_order_violation;
	bool unmask_saw_owner;
	bool break_tx_after_datapath;
	bool break_tx_after_activation;
};

static int operation(struct simulation *simulation)
{
	simulation->operation++;
	return simulation->operation == simulation->fail_at ? TEST_ERROR : 0;
}

static bool tx_is_disabled(void *context)
{
	struct simulation *simulation = context;

	return simulation->tx_disabled;
}

static int start_mac(void *context)
{
	return operation(context);
}

static int start_datapath(void *context)
{
	struct simulation *simulation = context;
	int ret = operation(simulation);

	if (!ret && simulation->break_tx_after_datapath)
		simulation->tx_disabled = false;
	return ret;
}

static int validate_mode(void *context)
{
	struct simulation *simulation = context;
	int ret = operation(simulation);

	if (!ret)
		simulation->mode_validated = true;
	return ret;
}

static void commit_owner(void *context)
{
	struct simulation *simulation = context;

	if (!simulation->mode_validated)
		simulation->owner_commit_order_violation = true;
	simulation->owner_committed = true;
}

static int unmask_irqs(void *context)
{
	struct simulation *simulation = context;

	simulation->unmask_saw_owner = simulation->owner_committed;
	return operation(simulation);
}

static int activate_mode(void *context)
{
	struct simulation *simulation = context;
	int ret = operation(simulation);

	if (!ret && simulation->break_tx_after_activation)
		simulation->tx_disabled = false;
	return ret;
}

static void disable_tx(void *context)
{
	struct simulation *simulation = context;

	simulation->disable_count++;
	simulation->tx_disabled = true;
}

static int quiesce_backend(void *context)
{
	struct simulation *simulation = context;

	simulation->quiesce_count++;
	return 0;
}

static void clear_owner(void *context)
{
	struct simulation *simulation = context;

	simulation->clear_count++;
	simulation->owner_committed = false;
}

static const struct airoha_xpon_claim_ops operations = {
	.tx_is_disabled = tx_is_disabled,
	.start_mac = start_mac,
	.start_datapath = start_datapath,
	.validate_mode = validate_mode,
	.commit_owner = commit_owner,
	.unmask_irqs = unmask_irqs,
	.activate_mode = activate_mode,
	.disable_tx = disable_tx,
	.quiesce_backend = quiesce_backend,
	.clear_owner = clear_owner,
};

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static int check_success(void)
{
	struct simulation simulation = { .tx_disabled = true };

	CHECK(!airoha_xpon_claim_transaction(&operations, &simulation));
	CHECK(simulation.operation == 5);
	CHECK(simulation.owner_committed);
	CHECK(simulation.mode_validated);
	CHECK(!simulation.owner_commit_order_violation);
	CHECK(simulation.unmask_saw_owner);
	CHECK(!simulation.disable_count);
	CHECK(!simulation.quiesce_count);
	CHECK(!simulation.clear_count);
	CHECK(simulation.tx_disabled);
	return 0;
}

static int check_every_failure(void)
{
	unsigned int fail_at;

	for (fail_at = 1; fail_at <= 5; fail_at++) {
		struct simulation simulation = {
			.fail_at = fail_at,
			.tx_disabled = true,
		};

		CHECK(airoha_xpon_claim_transaction(&operations, &simulation) ==
		      TEST_ERROR);
		CHECK(simulation.operation == fail_at);
		CHECK(simulation.disable_count == 1);
		CHECK(simulation.quiesce_count == 1);
		CHECK(simulation.clear_count == (unsigned int)(fail_at >= 4));
		CHECK(!simulation.owner_committed);
		CHECK(!simulation.owner_commit_order_violation);
		CHECK(simulation.tx_disabled);
	}
	return 0;
}

static int check_tx_gate(bool after_activation)
{
	struct simulation simulation = {
		.tx_disabled = true,
		.break_tx_after_datapath = !after_activation,
		.break_tx_after_activation = after_activation,
	};

	CHECK(airoha_xpon_claim_transaction(&operations, &simulation) == -EIO);
	CHECK(simulation.disable_count == 1);
	CHECK(simulation.quiesce_count == 1);
	CHECK(simulation.clear_count == (unsigned int)after_activation);
	CHECK(!simulation.owner_committed);
	CHECK(simulation.tx_disabled);
	return 0;
}

static int check_initial_tx_gate(void)
{
	struct simulation simulation = { 0 };

	CHECK(airoha_xpon_claim_transaction(&operations, &simulation) == -EIO);
	CHECK(!simulation.operation);
	CHECK(simulation.disable_count == 1);
	CHECK(simulation.quiesce_count == 1);
	CHECK(!simulation.clear_count);
	CHECK(simulation.tx_disabled);
	return 0;
}

int main(void)
{
	if (check_success() || check_every_failure() ||
	    check_tx_gate(false) || check_tx_gate(true) ||
	    check_initial_tx_gate())
		return 1;

	puts("XPON backend claim transaction: OK");
	return 0;
}
