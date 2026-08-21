/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>

#include "../src/en7572-tx-eye.h"

static int expect_mismatch(const char *name,
			   const struct en7572_tx_eye_fingerprint *expected,
			   const struct en7572_tx_eye_fingerprint *actual,
			   unsigned int wanted)
{
	unsigned int mismatch = en7572_tx_eye_mismatch(expected, actual);

	if (mismatch == wanted)
		return 0;
	fprintf(stderr, "%s: got mismatch %#x, want %#x\n", name, mismatch,
		wanted);
	return 1;
}

int main(void)
{
	struct en7572_tx_eye_fingerprint expected, actual;
	en7572_u8 bank[EN7572_BOB_BANK_SIZE] = { 0 };
	int failed = 0;

	bank[EN7572_BOB_IMOD_OFFSET] = 0x34;
	bank[EN7572_BOB_IMOD_OFFSET + 1] = 0x12;
	bank[EN7572_BOB_IAV_OFFSET] = 0x78;
	bank[EN7572_BOB_IAV_OFFSET + 1] = 0x56;
	bank[EN7572_BOB_APC_OFFSET] = 0x9a;
	bank[EN7572_BOB_TIA_CUR_OFFSET] = 1;
	bank[EN7572_BOB_TIA_GAIN_OFFSET] = 5;
	bank[EN7572_BOB_TIA_BW_OFFSET] = 6;
	bank[EN7572_BOB_ERC_CDAC_OFFSET] = 0xbc;
	bank[EN7572_BOB_ERC_DAC_OFFSET] = 0xef;
	bank[EN7572_BOB_ERC_DAC_OFFSET + 1] = 0x0d;
	bank[EN7572_BOB_PGA_GAIN_OFFSET] = 5;
	bank[EN7572_BOB_PGA_CAP_OFFSET] = 2;
	bank[EN7572_BOB_TSSI_OFFSET] = 0x11;
	bank[EN7572_BOB_TSSI_OFFSET + 1] = 0x22;
	bank[EN7572_BOB_TSSI_OFFSET + 2] = 0x33;
	bank[EN7572_BOB_TSSI_OFFSET + 3] = 0x44;

	en7572_tx_eye_from_bank(bank, &expected);
	actual = expected;
	failed += expect_mismatch("matching eye", &expected, &actual, 0);

	actual.ben_ctrl ^= EN7572_BEN_MODE;
	failed += expect_mismatch("BEN", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_BEN);
	actual = expected;
	actual.dcl_ctrl2 ^= EN7572_DCL_IAV;
	failed += expect_mismatch("DCL", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_DCL);
	actual = expected;
	actual.apc_ctrl ^= EN7572_APC_DAC;
	failed += expect_mismatch("APC", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_APC);
	actual = expected;
	actual.tia_ctrl ^= EN7572_TIA_GAIN_BW;
	failed += expect_mismatch("TIA", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_TIA);
	actual = expected;
	actual.erc_ctrl ^= EN7572_ERC_DAC;
	failed += expect_mismatch("ERC", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_ERC);
	actual = expected;
	actual.pga_ctrl ^= EN7572_PGA_CAP;
	failed += expect_mismatch("PGA", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_PGA);
	actual = expected;
	actual.tssi ^= 1;
	failed += expect_mismatch("TSSI", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_TSSI);
	actual = expected;
	actual.loop_ctrl ^= EN7572_LOOP_ENABLE;
	failed += expect_mismatch("loop", &expected, &actual,
		EN7572_TX_EYE_MISMATCH_LOOP);

	actual = expected;
	actual.ben_ctrl ^= 1U << 31;
	actual.dcl_ctrl2 ^= 1U << 31;
	actual.apc_ctrl ^= 1U;
	actual.tia_ctrl ^= 1U << 31;
	actual.erc_ctrl ^= 1U;
	actual.pga_ctrl ^= 1U << 31;
	actual.loop_ctrl ^= 1U << 31;
	failed += expect_mismatch("unowned bits", &expected, &actual, 0);

	actual = expected;
	actual.ben_ctrl ^= EN7572_BEN_MODE;
	actual.dcl_ctrl2 ^= EN7572_DCL_IMOD;
	actual.apc_ctrl ^= EN7572_APC_DAC;
	actual.tia_ctrl ^= EN7572_TIA_CURRENT;
	actual.erc_ctrl ^= EN7572_ERC_CDAC;
	actual.pga_ctrl ^= EN7572_PGA_GAIN;
	actual.tssi ^= 1;
	actual.loop_ctrl ^= EN7572_LOOP_ENABLE;
	failed += expect_mismatch("all fields", &expected, &actual, 0xff);

	return failed ? 1 : 0;
}
