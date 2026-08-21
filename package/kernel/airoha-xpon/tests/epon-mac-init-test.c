// SPDX-License-Identifier: GPL-2.0-only
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/airoha-epon-mac-init.h"

#define TEST_ERROR (-5)

struct simulation {
	unsigned int registers[AIROHA_EPON_MAC_INIT_REGISTER_COUNT];
	unsigned int original[AIROHA_EPON_MAC_INIT_REGISTER_COUNT];
	unsigned int reads;
	unsigned int updates;
	unsigned int fail_read_at;
	unsigned int fail_update_at;
	unsigned int fail_after_write_at;
	unsigned int fail_rollback_at;
	unsigned int fail_closed_count;
};

static int simulation_read(void *context,
			   enum airoha_epon_mac_init_register reg,
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
			     enum airoha_epon_mac_init_register reg,
			     unsigned int mask, unsigned int value)
{
	struct simulation *simulation = context;

	simulation->updates++;
	if (simulation->updates == simulation->fail_update_at ||
	    simulation->updates == simulation->fail_rollback_at)
		return TEST_ERROR;
	simulation->registers[reg] =
		(simulation->registers[reg] & ~mask) | (value & mask);
	if (simulation->updates == simulation->fail_after_write_at)
		return TEST_ERROR;
	return 0;
}

static void simulation_fail_closed(void *context)
{
	struct simulation *simulation = context;

	simulation->fail_closed_count++;
}

static const struct airoha_epon_mac_init_ops operations = {
	.read = simulation_read,
	.update = simulation_update,
	.fail_closed = simulation_fail_closed,
};

static void simulation_init(struct simulation *simulation)
{
	unsigned int i;

	memset(simulation, 0, sizeof(*simulation));
	for (i = 0; i < AIROHA_EPON_MAC_INIT_REGISTER_COUNT; i++) {
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

static int test_success(enum airoha_xpon_mode mode)
{
	const struct airoha_epon_mac_init_setting *settings;
	struct simulation simulation;
	unsigned int i;
	int ret;

	settings = airoha_epon_mac_init_profile(mode);
	CHECK(settings);
	simulation_init(&simulation);
	ret = airoha_epon_mac_init_transaction(&operations, &simulation, mode);
	CHECK(!ret);
	CHECK(simulation.reads == AIROHA_EPON_MAC_INIT_REGISTER_COUNT);
	CHECK(simulation.updates == AIROHA_EPON_MAC_INIT_REGISTER_COUNT);
	CHECK(!simulation.fail_closed_count);
	for (i = 0; i < AIROHA_EPON_MAC_INIT_REGISTER_COUNT; i++) {
		const struct airoha_epon_mac_init_setting *setting =
			&settings[i];

		CHECK((simulation.registers[setting->reg] & setting->mask) ==
		      setting->value);
		CHECK((simulation.registers[setting->reg] & ~setting->mask) ==
		      (simulation.original[setting->reg] & ~setting->mask));
	}
	CHECK((simulation.registers[AIROHA_EPON_MAC_INIT_TRX_ADJUST2] &
	       0xffffffffU) == 0x00000006U);
	CHECK((simulation.registers[AIROHA_EPON_MAC_INIT_TRX_ADJUST4] &
	       0xffff1f00U) == 0xff900000U);
	CHECK((simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP] &
	       0x8000ff00U) == 0x00000100U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD1] ==
	      0x88090300U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD2] ==
	      0x52000110U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD3] ==
	      0x01000000U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD4] ==
	      0x0f05ee00U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD5] ==
	      0x13250022U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD6] ==
	      0x01000210U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD7] ==
	      0x01000000U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD8] ==
	      0x0f05ee00U);
	CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_DYING_GASP_WORD9] ==
	      0x13250000U);
	if (mode == AIROHA_XPON_MODE_EPON_10G_1G) {
		CHECK(simulation.registers[
			AIROHA_EPON_MAC_INIT_REPORT_QSIZE_ADJUST] ==
		      0x00f100f1U);
		CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_TRX_ADJUST3] ==
		      0x00000000U);
	} else {
		CHECK((simulation.registers[
			AIROHA_EPON_MAC_INIT_REPORT_QSIZE_ADJUST] & 0xffffU) ==
		      0x0019U);
		CHECK(simulation.registers[AIROHA_EPON_MAC_INIT_TRX_ADJUST3] ==
		      0x00000008U);
	}
	return 0;
}

static int test_invalid_input(void)
{
	struct simulation simulation;
	struct airoha_epon_mac_init_ops incomplete = operations;
	int ret;

	simulation_init(&simulation);
	ret = airoha_epon_mac_init_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_XGSPON);
	CHECK(ret == -22);
	CHECK(!simulation.reads && !simulation.updates);
	incomplete.update = NULL;
	ret = airoha_epon_mac_init_transaction(
		&incomplete, &simulation, AIROHA_XPON_MODE_EPON_10G_1G);
	CHECK(ret == -22);
	return 0;
}

static int test_snapshot_failures(void)
{
	struct simulation simulation;
	unsigned int fail_at;
	int ret;

	for (fail_at = 1; fail_at <= AIROHA_EPON_MAC_INIT_REGISTER_COUNT;
	     fail_at++) {
		simulation_init(&simulation);
		simulation.fail_read_at = fail_at;
		ret = airoha_epon_mac_init_transaction(
			&operations, &simulation, AIROHA_XPON_MODE_EPON_10G_1G);
		CHECK(ret == TEST_ERROR);
		CHECK(!memcmp(simulation.registers, simulation.original,
			      sizeof(simulation.registers)));
		CHECK(!simulation.updates);
		CHECK(simulation.fail_closed_count == 1);
	}
	return 0;
}

static int test_update_failures(bool after_write)
{
	struct simulation simulation;
	unsigned int fail_at;
	int ret;

	for (fail_at = 1; fail_at <= AIROHA_EPON_MAC_INIT_REGISTER_COUNT;
	     fail_at++) {
		simulation_init(&simulation);
		if (after_write)
			simulation.fail_after_write_at = fail_at;
		else
			simulation.fail_update_at = fail_at;
		ret = airoha_epon_mac_init_transaction(
			&operations, &simulation, AIROHA_XPON_MODE_EPON_10G_10G);
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
	simulation.fail_after_write_at = 4;
	simulation.fail_rollback_at = 5;
	ret = airoha_epon_mac_init_transaction(
		&operations, &simulation, AIROHA_XPON_MODE_EPON_10G_1G);
	CHECK(ret == AIROHA_EPON_MAC_INIT_ROLLBACK_FAILED);
	CHECK(simulation.fail_closed_count == 1);
	CHECK(simulation.updates == 8);
	return 0;
}

int main(void)
{
	if (test_success(AIROHA_XPON_MODE_EPON_10G_1G) ||
	    test_success(AIROHA_XPON_MODE_EPON_10G_10G) ||
	    test_invalid_input() || test_snapshot_failures() ||
	    test_update_failures(false) || test_update_failures(true) ||
	    test_rollback_failure())
		return 1;
	puts("EPON MAC initialization transaction tests passed");
	return 0;
}
