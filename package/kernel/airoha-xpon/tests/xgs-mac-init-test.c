// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-xgs-mac-init.h"

#define TEST_ERROR (-5)

struct simulation {
	unsigned int registers[AIROHA_XGS_MAC_INIT_REGISTER_COUNT];
	unsigned int original[AIROHA_XGS_MAC_INIT_REGISTER_COUNT];
	unsigned int reads;
	unsigned int updates;
	unsigned int fail_read_at;
	unsigned int fail_update_at;
	unsigned int fail_rollback_at;
	unsigned int fail_closed_count;
};

static int simulation_read(void *context,
			   enum airoha_xgs_mac_init_register reg,
			   unsigned int *value)
{
	struct simulation *simulation = context;

	simulation->reads++;
	if (simulation->reads == simulation->fail_read_at)
		return TEST_ERROR;
	*value = simulation->registers[reg];
	return 0;
}

static int simulation_update(void *context,
			     enum airoha_xgs_mac_init_register reg,
			     unsigned int mask, unsigned int value)
{
	struct simulation *simulation = context;

	simulation->updates++;
	if (simulation->updates == simulation->fail_update_at ||
	    simulation->updates == simulation->fail_rollback_at)
		return TEST_ERROR;
	simulation->registers[reg] =
		(simulation->registers[reg] & ~mask) | (value & mask);
	return 0;
}

static void simulation_fail_closed(void *context)
{
	struct simulation *simulation = context;

	simulation->fail_closed_count++;
}

static const struct airoha_xgs_mac_init_ops operations = {
	.read = simulation_read,
	.update = simulation_update,
	.fail_closed = simulation_fail_closed,
};

static void simulation_init(struct simulation *simulation)
{
	unsigned int i;

	memset(simulation, 0, sizeof(*simulation));
	for (i = 0; i < AIROHA_XGS_MAC_INIT_REGISTER_COUNT; i++) {
		simulation->registers[i] = 0xa5a50000U | (i * 0x111U);
		simulation->original[i] = simulation->registers[i];
	}
}

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
			#condition); \
		return 1; \
	} \
} while (0)

static int test_success(void)
{
	struct simulation simulation;
	unsigned int i;
	int ret;

	simulation_init(&simulation);
	ret = airoha_xgs_mac_init_transaction(&operations, &simulation);
	CHECK(!ret);
	CHECK(simulation.reads == AIROHA_XGS_MAC_INIT_REGISTER_COUNT);
	CHECK(simulation.updates == AIROHA_XGS_MAC_INIT_REGISTER_COUNT);
	CHECK(!simulation.fail_closed_count);
	for (i = 0; i < AIROHA_XGS_MAC_INIT_REGISTER_COUNT; i++) {
		const struct airoha_xgs_mac_init_setting *setting =
			&airoha_xgs_mac_init_settings[i];

		CHECK((simulation.registers[setting->reg] & setting->mask) ==
		      setting->value);
		CHECK((simulation.registers[setting->reg] & ~setting->mask) ==
		      (simulation.original[setting->reg] & ~setting->mask));
	}
	return 0;
}

static int test_xgpon_success(void)
{
	struct simulation simulation;
	unsigned int i;
	int ret;

	simulation_init(&simulation);
	ret = airoha_xgs_mac_init_transaction_mode(
		&operations, &simulation, AIROHA_XPON_MODE_XGPON);
	CHECK(!ret);
	for (i = 0; i < AIROHA_XGS_MAC_INIT_REGISTER_COUNT; i++) {
		const struct airoha_xgs_mac_init_setting *setting =
			&airoha_xgpon_mac_init_settings[i];

		CHECK((simulation.registers[setting->reg] & setting->mask) ==
		      setting->value);
	}
	CHECK((simulation.registers[AIROHA_XGS_MAC_INIT_RSP_TIME] &
	       0x00003fffU) == 0x00000551U);
	CHECK((simulation.registers[AIROHA_XGS_MAC_INIT_IDLE_GEM] &
	       0x0000ffffU) == 0x0000001aU);
	return 0;
}

static int test_snapshot_failures(void)
{
	struct simulation simulation;
	unsigned int fail_at;
	int ret;

	for (fail_at = 1; fail_at <= AIROHA_XGS_MAC_INIT_REGISTER_COUNT;
	     fail_at++) {
		simulation_init(&simulation);
		simulation.fail_read_at = fail_at;
		ret = airoha_xgs_mac_init_transaction(&operations, &simulation);
		CHECK(ret == TEST_ERROR);
		CHECK(!memcmp(simulation.registers, simulation.original,
			      sizeof(simulation.registers)));
		CHECK(!simulation.updates);
		CHECK(simulation.fail_closed_count == 1);
	}
	return 0;
}

static int test_update_failures(void)
{
	struct simulation simulation;
	unsigned int fail_at;
	int ret;

	for (fail_at = 1; fail_at <= AIROHA_XGS_MAC_INIT_REGISTER_COUNT;
	     fail_at++) {
		simulation_init(&simulation);
		simulation.fail_update_at = fail_at;
		ret = airoha_xgs_mac_init_transaction(&operations, &simulation);
		CHECK(ret == TEST_ERROR);
		CHECK(!memcmp(simulation.registers, simulation.original,
			      sizeof(simulation.registers)));
		CHECK(simulation.updates == fail_at + fail_at);
		CHECK(simulation.fail_closed_count == 1);
	}
	return 0;
}

static int test_rollback_failure(void)
{
	struct simulation simulation;
	int ret;

	simulation_init(&simulation);
	simulation.fail_update_at = 4;
	simulation.fail_rollback_at = 5;
	ret = airoha_xgs_mac_init_transaction(&operations, &simulation);
	CHECK(ret == AIROHA_XGS_MAC_INIT_ROLLBACK_FAILED);
	CHECK(simulation.fail_closed_count == 1);
	CHECK(simulation.updates == 8);
	return 0;
}

int main(void)
{
	if (test_success() || test_xgpon_success() || test_snapshot_failures() ||
	    test_update_failures() || test_rollback_failure())
		return 1;
	puts("XG-PON/XGS-PON MAC initialization transaction tests passed");
	return 0;
}
