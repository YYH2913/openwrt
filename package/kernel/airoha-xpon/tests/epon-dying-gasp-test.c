// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-epon-dying-gasp.h"

#define TEST_ERROR (-9)

struct simulation {
	bool enabled;
	unsigned int updates;
	unsigned int reads;
	unsigned int fail_update_at;
	unsigned int fail_after_update_at;
	unsigned int fail_read_at;
	unsigned int mismatch_read_at;
	unsigned int fail_closed_count;
};

static int simulation_update(void *context, bool enabled)
{
	struct simulation *simulation = context;

	simulation->updates++;
	if (simulation->updates == simulation->fail_update_at)
		return TEST_ERROR;
	simulation->enabled = enabled;
	return simulation->updates == simulation->fail_after_update_at ?
		TEST_ERROR : 0;
}

static int simulation_read(void *context, bool *enabled)
{
	struct simulation *simulation = context;

	simulation->reads++;
	if (simulation->reads == simulation->fail_read_at)
		return TEST_ERROR;
	*enabled = simulation->enabled;
	if (simulation->reads == simulation->mismatch_read_at)
		*enabled = !*enabled;
	return 0;
}

static void simulation_fail_closed(void *context)
{
	struct simulation *simulation = context;

	simulation->fail_closed_count++;
}

static const struct airoha_epon_dying_gasp_ops operations = {
	.update = simulation_update,
	.read = simulation_read,
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
	struct simulation simulation = {};
	int ret;

	ret = airoha_epon_dying_gasp_set(&operations, &simulation, true);
	CHECK(!ret && simulation.enabled);
	ret = airoha_epon_dying_gasp_set(&operations, &simulation, false);
	CHECK(!ret && !simulation.enabled);
	CHECK(simulation.updates == 2 && simulation.reads == 2);
	CHECK(!simulation.fail_closed_count);
	return 0;
}

static int test_invalid_ops(void)
{
	struct airoha_epon_dying_gasp_ops incomplete = operations;
	struct simulation simulation = {};

	CHECK(airoha_epon_dying_gasp_set(NULL, &simulation, true) == -22);
	incomplete.read = NULL;
	CHECK(airoha_epon_dying_gasp_set(
		&incomplete, &simulation, true) == -22);
	return 0;
}

static int test_failure(unsigned int fail_update_at,
			unsigned int fail_after_update_at,
			unsigned int fail_read_at,
			unsigned int mismatch_read_at, int expected)
{
	struct simulation simulation = {
		.fail_update_at = fail_update_at,
		.fail_after_update_at = fail_after_update_at,
		.fail_read_at = fail_read_at,
		.mismatch_read_at = mismatch_read_at,
	};
	int ret;

	ret = airoha_epon_dying_gasp_set(&operations, &simulation, true);
	CHECK(ret == expected);
	CHECK(!simulation.enabled);
	CHECK(simulation.fail_closed_count == 1);
	return 0;
}

static int test_rollback_failure(void)
{
	struct simulation simulation = {
		.fail_update_at = 2,
		.mismatch_read_at = 1,
	};
	int ret;

	ret = airoha_epon_dying_gasp_set(&operations, &simulation, true);
	CHECK(ret == AIROHA_EPON_DYING_GASP_ROLLBACK_FAILED);
	CHECK(simulation.enabled);
	CHECK(simulation.fail_closed_count == 1);
	return 0;
}

int main(void)
{
	if (test_success() || test_invalid_ops() ||
	    test_failure(1, 0, 0, 0, TEST_ERROR) ||
	    test_failure(0, 1, 0, 0, TEST_ERROR) ||
	    test_failure(0, 0, 1, 0, TEST_ERROR) ||
	    test_failure(0, 0, 0, 1,
		AIROHA_EPON_DYING_GASP_VERIFY_FAILED) ||
	    test_rollback_failure())
		return 1;
	puts("EPON post-reset Dying Gasp transaction tests passed");
	return 0;
}
