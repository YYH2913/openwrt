// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define __u8 uint8_t
#define __u16 uint16_t
#define __u32 uint32_t
#define u8 uint8_t
#define u16 uint16_t
#define u32 uint32_t
#include "../src/airoha-xpon-mode.h"
#include "../src/airoha-epon-mac.h"

#define FIELD_GET(mask, reg) (((reg) & (mask)) >> __bf_shf(mask))

int main(void)
{
	const struct airoha_epon_mode_profile *asym;
	const struct airoha_epon_mode_profile *sym;
	struct airoha_epon_mpcp_sync_update sync;
	u32 command, status;

	asym = airoha_epon_mode_profile(AIROHA_XPON_MODE_EPON_10G_1G);
	sym = airoha_epon_mode_profile(AIROHA_XPON_MODE_EPON_10G_10G);
	assert(asym && sym);
	assert(!asym->upstream_10g && asym->qdma_report_fec);
	assert(sym->upstream_10g && !sym->qdma_report_fec);
	assert(sym->upstream_timestamp_adjust == 8);
	assert(!airoha_epon_mode_profile(AIROHA_XPON_MODE_XGSPON));

	command = airoha_epon_mpcp_command(
		AIROHA_EPON_MPCP_CMD_REGISTER_ACK, 31, false, true);
	assert(FIELD_GET(AIROHA_EPON_MPCP_COMMAND, command) == 3);
	assert(FIELD_GET(AIROHA_EPON_MPCP_LLID_INDEX, command) == 31);
	assert(command & AIROHA_EPON_MPCP_ACK);
	assert(!(command & AIROHA_EPON_MPCP_REQUEST_FLAG));
	command = airoha_epon_mpcp_command(
		AIROHA_EPON_MPCP_CMD_NORMAL_REQUEST, 7, true, false);
	assert(FIELD_GET(AIROHA_EPON_MPCP_COMMAND, command) == 2);
	assert(FIELD_GET(AIROHA_EPON_MPCP_LLID_INDEX, command) == 7);
	assert(command & AIROHA_EPON_MPCP_REQUEST_FLAG);
	assert(!(command & AIROHA_EPON_MPCP_ACK));

	status = airoha_epon_phy_tx_mode(0xffffffff, true);
	assert(!(status & AIROHA_EPON_PHY_TX_CONTINUOUS));
	assert((status | AIROHA_EPON_PHY_TX_CONTINUOUS) == 0xffffffff);
	status = airoha_epon_phy_tx_mode(0, false);
	assert(status == AIROHA_EPON_PHY_TX_CONTINUOUS);

	assert(airoha_epon_mpcp_register_flag_allowed(
		AIROHA_EPON_MPCP_REGISTER_REQUEST,
		AIROHA_EPON_REGISTER_FLAG_ACK, true));
	assert(!airoha_epon_mpcp_register_flag_allowed(
		AIROHA_EPON_MPCP_REGISTERING,
		AIROHA_EPON_REGISTER_FLAG_ACK, true));
	assert(airoha_epon_mpcp_register_flag_allowed(
		AIROHA_EPON_MPCP_REGISTERED,
		AIROHA_EPON_REGISTER_FLAG_REREGISTER, true));
	assert(!airoha_epon_mpcp_register_flag_allowed(
		AIROHA_EPON_MPCP_REGISTER_REQUEST,
		AIROHA_EPON_REGISTER_FLAG_REREGISTER, true));
	assert(!airoha_epon_mpcp_register_flag_allowed(
		AIROHA_EPON_MPCP_REGISTERED,
		AIROHA_EPON_REGISTER_FLAG_REREGISTER, false));
	assert(airoha_epon_mpcp_data_ready(AIROHA_EPON_MPCP_REGISTERED,
					   true));
	assert(!airoha_epon_mpcp_data_ready(AIROHA_EPON_MPCP_REGISTER_PENDING,
					    true));
	assert(!airoha_epon_mpcp_data_ready(AIROHA_EPON_MPCP_REGISTERED,
					    false));
	assert(airoha_epon_mpcp_timeout_action(false,
		AIROHA_EPON_MPCP_REGISTERED, true) ==
		AIROHA_EPON_MPCP_TIMEOUT_IGNORE);
	assert(airoha_epon_mpcp_timeout_action(true,
		AIROHA_EPON_MPCP_REGISTERED, true) ==
		AIROHA_EPON_MPCP_TIMEOUT_DEREGISTER);
	assert(airoha_epon_mpcp_timeout_action(true,
		AIROHA_EPON_MPCP_REGISTERED, false) ==
		AIROHA_EPON_MPCP_TIMEOUT_REINITIALIZE);
	assert(airoha_epon_mpcp_timeout_action(true,
		AIROHA_EPON_MPCP_REGISTER_REQUEST, true) ==
		AIROHA_EPON_MPCP_TIMEOUT_REINITIALIZE);

	sync = airoha_epon_mpcp_sync_update(0x30, 0x20, false);
	assert(sync.sync_time == 0x30);
	assert(!sync.program_sync_time && sync.program_timestamp_adjust);
	assert(sync.timestamp_adjust == 0x005ffff1);
	sync = airoha_epon_mpcp_sync_update(0, 0x20, false);
	assert(sync.sync_time == 0x20);
	assert(sync.program_sync_time && sync.program_timestamp_adjust);
	assert(sync.timestamp_adjust == 0x004ffff1);
	sync = airoha_epon_mpcp_sync_update(0x60, 0x20, false);
	assert(sync.sync_time == AIROHA_EPON_MPCP_SYNC_TIME_MAX);
	assert(sync.program_sync_time && sync.program_timestamp_adjust);
	assert(sync.timestamp_adjust == 0x008efff1);
	sync = airoha_epon_mpcp_sync_update(0x60, 0x20, true);
	assert(!sync.program_sync_time && !sync.program_timestamp_adjust);

	status = 0xa5121234;
	status = airoha_epon_llid_set_discovery_state(status,
		AIROHA_EPON_DISCOVERY_STATE_REGISTERED);
	assert(FIELD_GET(AIROHA_EPON_LLID_DISCOVERY_STATE, status) == 2);
	assert(FIELD_GET(AIROHA_EPON_LLID_VALUE, status) == 0x1234);

	assert(airoha_epon_nonfatal_irq_count(0, 0) == 0);
	assert(airoha_epon_nonfatal_irq_count(
		AIROHA_EPON_INT_GRANT_OVERRUN, 0) == 1);
	assert(airoha_epon_nonfatal_irq_count(
		AIROHA_EPON_INT_GRANT_OVERRUN |
		AIROHA_EPON_INT_TX_UNDERRUN, BIT(0) | BIT(12)) == 4);
	assert(airoha_epon_nonfatal_irq_count(BIT(13), BIT(13)) == 0);
	puts("epon MAC profile and MPCP encoding tests passed");
	return 0;
}
