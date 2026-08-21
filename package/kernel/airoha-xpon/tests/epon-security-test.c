// SPDX-License-Identifier: GPL-2.0-only

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/airoha-epon-oam-abi.h"
#include "../src/airoha-epon-security.h"

int main(void)
{
	const uint8_t ctc[9] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x10,
	};
	const uint8_t dpoe[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};

	assert(sizeof(struct airoha_epon_oam_key) == 32);
	assert(sizeof(struct airoha_epon_oam_dba) == 56);
	assert(sizeof(struct airoha_epon_oam_key_events) == 20);
	assert(AIROHA_EPON_OAM_IOC_GET_DBA != AIROHA_EPON_OAM_IOC_SET_DBA);
	assert(AIROHA_EPON_OAM_IOC_GET_KEY_EVENTS !=
	       AIROHA_EPON_OAM_IOC_SET_KEY);
	assert(airoha_epon_key_command(true, 31, 1, 2) == 0x80001f12U);
	assert(airoha_epon_key_command(false, 0, 0, 0) == 0);
	assert(airoha_epon_ctc_key_word(ctc, 0) == 0x00012345U);
	assert(airoha_epon_ctc_key_word(ctc, 1) == 0x006789abU);
	assert(airoha_epon_ctc_key_word(ctc, 2) == 0x00cdef10U);
	assert(airoha_epon_dpoe_key_word(dpoe, 0) == 0x00112233U);
	assert(airoha_epon_dpoe_key_word(dpoe, 3) == 0xccddeeffU);
	puts("EPON security ABI and key encoding tests passed");
	return 0;
}
