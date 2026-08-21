// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-epon-logic-reset.h"

#define TEST_ERROR (-123)

struct simulation {
	bool hold;
	unsigned int updates;
	unsigned int reads;
	unsigned int delays;
	unsigned int fail_update_at;
	unsigned int fail_after_update_at;
	unsigned int fail_read_at;
	unsigned int ignore_update_at;
	unsigned int fail_closed_count;
};

static int simulation_update(void *context, bool hold)
{
	struct simulation *simulation = context;

	simulation->updates++;
	if (simulation->updates == simulation->fail_update_at)
		return TEST_ERROR;
	if (simulation->updates != simulation->ignore_update_at)
		simulation->hold = hold;
	if (simulation->updates == simulation->fail_after_update_at)
		return TEST_ERROR;
	return 0;
}

static int simulation_read(void *context, bool *hold)
{
	struct simulation *simulation = context;

	simulation->reads++;
	if (simulation->reads == simulation->fail_read_at)
		return TEST_ERROR;
	*hold = simulation->hold;
	return 0;
}

static void simulation_delay(void *context)
{
	struct simulation *simulation = context;

	simulation->delays++;
}

static void simulation_fail_closed(void *context)
{
	struct simulation *simulation = context;

	simulation->fail_closed_count++;
}

static const struct airoha_epon_logic_reset_ops operations = {
	.update = simulation_update,
	.read = simulation_read,
	.delay = simulation_delay,
	.fail_closed = simulation_fail_closed,
};

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static int test_success(void)
{
	struct simulation simulation = { .hold = false };
	int ret;

	ret = airoha_epon_logic_reset_pulse(&operations, &simulation);
	CHECK(!ret);
	CHECK(!simulation.hold);
	CHECK(simulation.updates == 2);
	CHECK(simulation.reads == 2);
	CHECK(simulation.delays == 2);
	CHECK(!simulation.fail_closed_count);
	return 0;
}

static int test_set_success(void)
{
	struct simulation simulation = { .hold = false };
	int ret;

	ret = airoha_epon_logic_reset_set(&operations, &simulation, true);
	CHECK(!ret);
	CHECK(simulation.hold);
	CHECK(simulation.updates == 1);
	CHECK(simulation.reads == 1);
	CHECK(simulation.delays == 1);
	CHECK(!simulation.fail_closed_count);

	ret = airoha_epon_logic_reset_set(&operations, &simulation, false);
	CHECK(!ret);
	CHECK(!simulation.hold);
	CHECK(simulation.updates == 2);
	CHECK(simulation.reads == 2);
	CHECK(simulation.delays == 2);
	CHECK(!simulation.fail_closed_count);
	return 0;
}

static int test_invalid_ops(void)
{
	struct airoha_epon_logic_reset_ops incomplete = operations;
	struct simulation simulation = { 0 };

	CHECK(airoha_epon_logic_reset_set(NULL, &simulation, true) == -22);
	CHECK(airoha_epon_logic_reset_pulse(NULL, &simulation) == -22);
	incomplete.read = NULL;
	CHECK(airoha_epon_logic_reset_set(
		&incomplete, &simulation, true) == -22);
	CHECK(airoha_epon_logic_reset_pulse(&incomplete, &simulation) == -22);
	CHECK(!simulation.updates && !simulation.reads);
	return 0;
}

static int test_update_failures(bool after_write)
{
	unsigned int fail_at;

	for (fail_at = 1; fail_at <= 2; fail_at++) {
		struct simulation simulation = { 0 };
		int ret;

		if (after_write)
			simulation.fail_after_update_at = fail_at;
		else
			simulation.fail_update_at = fail_at;
		ret = airoha_epon_logic_reset_pulse(&operations, &simulation);
		CHECK(ret == TEST_ERROR);
		CHECK(simulation.hold);
		CHECK(simulation.fail_closed_count == 1);
		CHECK(simulation.delays == (fail_at == 2 ? 1U : 0U));
	}
	return 0;
}

static int test_read_failures(void)
{
	unsigned int fail_at;

	for (fail_at = 1; fail_at <= 2; fail_at++) {
		struct simulation simulation = { .fail_read_at = fail_at };
		int ret;

		ret = airoha_epon_logic_reset_pulse(&operations, &simulation);
		CHECK(ret == TEST_ERROR);
		CHECK(simulation.hold);
		CHECK(simulation.fail_closed_count == 1);
		CHECK(simulation.delays == (fail_at == 2 ? 1U : 0U));
	}
	return 0;
}

static int test_readback_mismatch(void)
{
	unsigned int ignore_at;

	for (ignore_at = 1; ignore_at <= 2; ignore_at++) {
		struct simulation simulation = { .ignore_update_at = ignore_at };
		int ret;

		ret = airoha_epon_logic_reset_pulse(&operations, &simulation);
		CHECK(ret == AIROHA_EPON_LOGIC_RESET_VERIFY_FAILED);
		CHECK(simulation.hold);
		CHECK(simulation.fail_closed_count == 1);
		CHECK(simulation.delays == (ignore_at == 2 ? 1U : 0U));
	}
	return 0;
}

static int test_rollback_failure(void)
{
	struct simulation simulation = {
		.fail_after_update_at = 2,
		.ignore_update_at = 3,
	};
	int ret;

	ret = airoha_epon_logic_reset_pulse(&operations, &simulation);
	CHECK(ret == AIROHA_EPON_LOGIC_RESET_ROLLBACK_FAILED);
	CHECK(!simulation.hold);
	CHECK(simulation.fail_closed_count == 1);
	return 0;
}

int main(void)
{
	if (test_success() || test_set_success() || test_invalid_ops() ||
	    test_update_failures(false) || test_update_failures(true) ||
	    test_read_failures() || test_readback_mismatch() ||
	    test_rollback_failure())
		return 1;
	puts("EPON reset pulse and hold/release transaction tests passed");
	return 0;
}
