// SPDX-License-Identifier: GPL-2.0-only
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-xgs-eqd.h"

#define TEST_ERROR (-71)

struct simulation {
	unsigned int value;
	unsigned int reads;
	unsigned int writes;
	unsigned int fail_reads;
	unsigned int fail_writes;
	unsigned int corrupt_reads;
	unsigned int fail_closed_count;
};

static int simulation_read(void *context, unsigned int *value)
{
	struct simulation *simulation = context;

	simulation->reads++;
	if (simulation->fail_reads & (1U << simulation->reads))
		return TEST_ERROR;
	*value = simulation->value;
	if (simulation->corrupt_reads & (1U << simulation->reads))
		*value ^= 1U;
	return 0;
}

static int simulation_write(void *context, unsigned int value)
{
	struct simulation *simulation = context;

	simulation->writes++;
	if (simulation->fail_writes & (1U << simulation->writes))
		return TEST_ERROR;
	simulation->value = value;
	return 0;
}

static void simulation_fail_closed(void *context)
{
	struct simulation *simulation = context;

	simulation->fail_closed_count++;
}

static const struct airoha_xgs_eqd_ops operations = {
	.read = simulation_read,
	.write = simulation_write,
	.fail_closed = simulation_fail_closed,
};

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static int test_resolve_and_encode(void)
{
	unsigned int value = 0xa5a5a5a5U;

	CHECK(!airoha_xgs_eqd_resolve(100, 25, true, true, &value));
	CHECK(value == 25);
	CHECK(!airoha_xgs_eqd_resolve(100, 25, false, false, &value));
	CHECK(value == 125);
	CHECK(!airoha_xgs_eqd_resolve(100, 25, false, true, &value));
	CHECK(value == 75);
	CHECK(airoha_xgs_eqd_resolve(UINT_MAX - 5, 6, false, false,
				    &value) == AIROHA_XGS_EQD_ERANGE);
	CHECK(airoha_xgs_eqd_resolve(5, 6, false, true, &value) ==
	      AIROHA_XGS_EQD_ERANGE);
	CHECK(airoha_xgs_eqd_resolve(0, 0, false, false, NULL) ==
	      AIROHA_XGS_EQD_EINVAL);
	CHECK(!airoha_xgs_eqd_encode(AIROHA_XPON_MODE_XGPON, 0x12345678,
				     &value));
	CHECK(value == 0x12345678);
	CHECK(!airoha_xgs_eqd_encode(AIROHA_XPON_MODE_XGSPON, 0x1234567,
				     &value));
	CHECK(value == 0x048d159c);
	CHECK(airoha_xgs_eqd_encode(AIROHA_XPON_MODE_XGSPON,
				    (UINT_MAX >> 2) + 1, &value) ==
	      AIROHA_XGS_EQD_ERANGE);
	CHECK(airoha_xgs_eqd_encode(AIROHA_XPON_MODE_EPON_10G_10G, 1,
				    &value) == AIROHA_XGS_EQD_EINVAL);
	return 0;
}

static int test_success(void)
{
	struct simulation simulation = { .value = 0xdeadbeefU };
	unsigned int resolved = 0xa5a5a5a5U;
	int ret;

	ret = airoha_xgs_eqd_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGSPON,
		100, 20, false, false, &resolved);
	CHECK(!ret);
	CHECK(resolved == 120);
	CHECK(simulation.value == 480);
	CHECK(simulation.reads == 2 && simulation.writes == 1);
	CHECK(!simulation.fail_closed_count);

	memset(&simulation, 0, sizeof(simulation));
	simulation.value = 0xdeadbeefU;
	ret = airoha_xgs_eqd_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGPON,
		100, 20, false, true, &resolved);
	CHECK(!ret && resolved == 80 && simulation.value == 80);
	return 0;
}

static int test_primary_failures_rollback(void)
{
	struct simulation simulation;
	unsigned int resolved;
	int ret;

	memset(&simulation, 0, sizeof(simulation));
	simulation.value = 0x11223344U;
	simulation.fail_writes = 1U << 1;
	resolved = 0xa5a5a5a5U;
	ret = airoha_xgs_eqd_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGPON,
		100, 20, false, false, &resolved);
	CHECK(ret == TEST_ERROR);
	CHECK(simulation.value == 0x11223344U);
	CHECK(simulation.writes == 2 && simulation.reads == 2);
	CHECK(resolved == 0xa5a5a5a5U && !simulation.fail_closed_count);

	memset(&simulation, 0, sizeof(simulation));
	simulation.value = 0x55667788U;
	simulation.fail_reads = 1U << 2;
	resolved = 0xa5a5a5a5U;
	ret = airoha_xgs_eqd_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGPON,
		100, 20, false, false, &resolved);
	CHECK(ret == TEST_ERROR);
	CHECK(simulation.value == 0x55667788U);
	CHECK(simulation.writes == 2 && simulation.reads == 3);
	CHECK(resolved == 0xa5a5a5a5U && !simulation.fail_closed_count);

	memset(&simulation, 0, sizeof(simulation));
	simulation.value = 0x99aabbccU;
	simulation.corrupt_reads = 1U << 2;
	resolved = 0xa5a5a5a5U;
	ret = airoha_xgs_eqd_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGPON,
		100, 20, false, false, &resolved);
	CHECK(ret == AIROHA_XGS_EQD_EIO);
	CHECK(simulation.value == 0x99aabbccU);
	CHECK(simulation.writes == 2 && simulation.reads == 3);
	CHECK(resolved == 0xa5a5a5a5U && !simulation.fail_closed_count);
	return 0;
}

static int test_rollback_failures_fail_closed(void)
{
	struct simulation simulation;
	unsigned int resolved;
	int ret;

	memset(&simulation, 0, sizeof(simulation));
	simulation.value = 0x01020304U;
	simulation.fail_writes = (1U << 1) | (1U << 2);
	resolved = 0xa5a5a5a5U;
	ret = airoha_xgs_eqd_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGPON,
		100, 20, false, false, &resolved);
	CHECK(ret == TEST_ERROR && simulation.fail_closed_count == 1);
	CHECK(resolved == 0xa5a5a5a5U);

	memset(&simulation, 0, sizeof(simulation));
	simulation.value = 0x05060708U;
	simulation.corrupt_reads = (1U << 2) | (1U << 3);
	resolved = 0xa5a5a5a5U;
	ret = airoha_xgs_eqd_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGPON,
		100, 20, false, false, &resolved);
	CHECK(ret == AIROHA_XGS_EQD_EIO);
	CHECK(simulation.fail_closed_count == 1);
	CHECK(resolved == 0xa5a5a5a5U);
	return 0;
}

int main(void)
{
	CHECK(!test_resolve_and_encode());
	CHECK(!test_success());
	CHECK(!test_primary_failures_rollback());
	CHECK(!test_rollback_failures_fail_closed());
	puts("XG-PON/XGS-PON EqD transaction tests passed");
	return 0;
}
