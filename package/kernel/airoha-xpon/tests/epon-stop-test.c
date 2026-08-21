// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>

#include "../src/airoha-epon-stop.h"

#define KEY_ERROR (-101)
#define THRESHOLD_ERROR (-102)
#define QDMA_ERROR (-103)

struct simulation {
	int key_error;
	int threshold_error;
	int qdma_error;
	unsigned int sequence;
	unsigned int key_sequence;
	unsigned int threshold_sequence;
	unsigned int qdma_sequence;
};

static int clear_keys(void *context)
{
	struct simulation *simulation = context;

	simulation->key_sequence = ++simulation->sequence;
	return simulation->key_error;
}

static int clear_report_thresholds(void *context)
{
	struct simulation *simulation = context;

	simulation->threshold_sequence = ++simulation->sequence;
	return simulation->threshold_error;
}

static int clear_qdma(void *context)
{
	struct simulation *simulation = context;

	simulation->qdma_sequence = ++simulation->sequence;
	return simulation->qdma_error;
}

static const struct airoha_epon_stop_ops operations = {
	.clear_keys = clear_keys,
	.clear_report_thresholds = clear_report_thresholds,
	.clear_qdma = clear_qdma,
};

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static int check_run(struct simulation *simulation, int expected)
{
	CHECK(airoha_epon_stop_cleanup(&operations, simulation) == expected);
	CHECK(simulation->sequence == 3);
	CHECK(simulation->key_sequence == 1);
	CHECK(simulation->threshold_sequence == 2);
	CHECK(simulation->qdma_sequence == 3);
	return 0;
}

static int test_invalid_operations(void)
{
	struct airoha_epon_stop_ops incomplete = operations;
	struct simulation simulation = { 0 };

	CHECK(airoha_epon_stop_cleanup(NULL, &simulation) == -22);
	incomplete.clear_report_thresholds = NULL;
	CHECK(airoha_epon_stop_cleanup(&incomplete, &simulation) == -22);
	CHECK(!simulation.sequence);
	return 0;
}

int main(void)
{
	struct simulation success = { 0 };
	struct simulation key_failure = { .key_error = KEY_ERROR };
	struct simulation threshold_failure = {
		.threshold_error = THRESHOLD_ERROR,
	};
	struct simulation qdma_failure = { .qdma_error = QDMA_ERROR };
	struct simulation combined_failure = {
		.key_error = KEY_ERROR,
		.threshold_error = THRESHOLD_ERROR,
		.qdma_error = QDMA_ERROR,
	};

	if (test_invalid_operations() || check_run(&success, 0) ||
	    check_run(&key_failure, KEY_ERROR) ||
	    check_run(&threshold_failure, THRESHOLD_ERROR) ||
	    check_run(&qdma_failure, QDMA_ERROR) ||
	    check_run(&combined_failure, KEY_ERROR))
		return 1;
	puts("EPON stop cleanup fault-injection tests passed");
	return 0;
}
