/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _EN7572_TX_EYE_H_
#define _EN7572_TX_EYE_H_

#include "en7572-calibration.h"

/* EN7572 registers touched by the SDK AdaptivePon() TX-eye transaction. */
#define EN7572_BEN_CTRL			0x0100
#define EN7572_BEN_MODE			0x0000000cU
#define EN7572_BEN_NORMAL		0
#define EN7572_BEN_OFF			2
#define EN7572_APC_CTRL			0x0124
#define EN7572_APC_DAC			0x0000ff00U
#define EN7572_ERC_CTRL			0x0128
#define EN7572_ERC_CDAC			0x0000ff00U
#define EN7572_ERC_DAC			0x0fff0000U
#define EN7572_TIA_CTRL			0x0130
#define EN7572_TIA_CURRENT		0x00000001U
#define EN7572_TIA_GAIN			0x00000700U
#define EN7572_TIA_BANDWIDTH		0x00003800U
#define EN7572_TIA_GAIN_BW		(EN7572_TIA_GAIN | \
					 EN7572_TIA_BANDWIDTH)
#define EN7572_PGA_CTRL			0x013c
#define EN7572_PGA_CAP			0x00000060U
#define EN7572_PGA_GAIN			0x00070000U
#define EN7572_LOOP_CTRL			0x0208
#define EN7572_LOOP_ENABLE		0x00000001U
#define EN7572_DCL_CTRL2		0x0210
#define EN7572_DCL_IAV			0x00001fffU
#define EN7572_DCL_IMOD			0x0fff0000U

#define EN7572_BOB_IMOD_OFFSET		0x86
#define EN7572_BOB_IAV_OFFSET		0x88
#define EN7572_BOB_TIA_CUR_OFFSET	0x8c
#define EN7572_BOB_ERC_CDAC_OFFSET	0x8d
#define EN7572_BOB_ERC_DAC_OFFSET	0x8e
#define EN7572_BOB_TIA_GAIN_OFFSET	0x90
#define EN7572_BOB_TIA_BW_OFFSET	0x91
#define EN7572_BOB_PGA_GAIN_OFFSET	0x92
#define EN7572_BOB_PGA_CAP_OFFSET	0x93
#define EN7572_BOB_TSSI_OFFSET		0xb4

struct en7572_tx_eye_fingerprint {
	en7572_u32 ben_ctrl;
	en7572_u32 dcl_ctrl2;
	en7572_u32 apc_ctrl;
	en7572_u32 tia_ctrl;
	en7572_u32 erc_ctrl;
	en7572_u32 pga_ctrl;
	en7572_u32 tssi;
	en7572_u32 loop_ctrl;
};

#define EN7572_TX_EYE_MISMATCH_BEN	(1U << 0)
#define EN7572_TX_EYE_MISMATCH_DCL	(1U << 1)
#define EN7572_TX_EYE_MISMATCH_APC	(1U << 2)
#define EN7572_TX_EYE_MISMATCH_TIA	(1U << 3)
#define EN7572_TX_EYE_MISMATCH_ERC	(1U << 4)
#define EN7572_TX_EYE_MISMATCH_PGA	(1U << 5)
#define EN7572_TX_EYE_MISMATCH_TSSI	(1U << 6)
#define EN7572_TX_EYE_MISMATCH_LOOP	(1U << 7)

static inline en7572_u32 en7572_tx_eye_le16(const en7572_u8 *data)
{
	return (en7572_u32)data[0] | ((en7572_u32)data[1] << 8);
}

static inline en7572_u32 en7572_tx_eye_le32(const en7572_u8 *data)
{
	return (en7572_u32)data[0] | ((en7572_u32)data[1] << 8) |
	       ((en7572_u32)data[2] << 16) | ((en7572_u32)data[3] << 24);
}

static inline en7572_u32 en7572_tx_eye_field(en7572_u32 mask,
					      en7572_u32 value)
{
	unsigned int shift = 0;

	while (!(mask & (1U << shift)))
		shift++;

	return (value << shift) & mask;
}

static inline void
en7572_tx_eye_from_bank(const en7572_u8 *bank,
			struct en7572_tx_eye_fingerprint *eye)
{
	eye->ben_ctrl = en7572_tx_eye_field(EN7572_BEN_MODE,
					 EN7572_BEN_NORMAL);
	eye->dcl_ctrl2 = en7572_tx_eye_field(EN7572_DCL_IMOD,
		en7572_tx_eye_le16(bank + EN7572_BOB_IMOD_OFFSET)) |
		en7572_tx_eye_field(EN7572_DCL_IAV,
		en7572_tx_eye_le16(bank + EN7572_BOB_IAV_OFFSET));
	eye->apc_ctrl = en7572_tx_eye_field(EN7572_APC_DAC,
		bank[EN7572_BOB_APC_OFFSET]);
	eye->tia_ctrl = en7572_tx_eye_field(EN7572_TIA_CURRENT,
		bank[EN7572_BOB_TIA_CUR_OFFSET]) |
		en7572_tx_eye_field(EN7572_TIA_GAIN_BW,
		(bank[EN7572_BOB_TIA_BW_OFFSET] << 3) |
		 bank[EN7572_BOB_TIA_GAIN_OFFSET]);
	eye->erc_ctrl = en7572_tx_eye_field(EN7572_ERC_CDAC,
		bank[EN7572_BOB_ERC_CDAC_OFFSET]) |
		en7572_tx_eye_field(EN7572_ERC_DAC,
		en7572_tx_eye_le16(bank + EN7572_BOB_ERC_DAC_OFFSET));
	eye->pga_ctrl = en7572_tx_eye_field(EN7572_PGA_GAIN,
		bank[EN7572_BOB_PGA_GAIN_OFFSET]) |
		en7572_tx_eye_field(EN7572_PGA_CAP,
		bank[EN7572_BOB_PGA_CAP_OFFSET]);
	eye->tssi = en7572_tx_eye_le32(bank + EN7572_BOB_TSSI_OFFSET);
	eye->loop_ctrl = EN7572_LOOP_ENABLE;
}

static inline unsigned int
en7572_tx_eye_mismatch(const struct en7572_tx_eye_fingerprint *expected,
			const struct en7572_tx_eye_fingerprint *actual)
{
	unsigned int mismatch = 0;

	if ((expected->ben_ctrl ^ actual->ben_ctrl) & EN7572_BEN_MODE)
		mismatch |= EN7572_TX_EYE_MISMATCH_BEN;
	if ((expected->dcl_ctrl2 ^ actual->dcl_ctrl2) &
	    (EN7572_DCL_IMOD | EN7572_DCL_IAV))
		mismatch |= EN7572_TX_EYE_MISMATCH_DCL;
	if ((expected->apc_ctrl ^ actual->apc_ctrl) & EN7572_APC_DAC)
		mismatch |= EN7572_TX_EYE_MISMATCH_APC;
	if ((expected->tia_ctrl ^ actual->tia_ctrl) &
	    (EN7572_TIA_CURRENT | EN7572_TIA_GAIN_BW))
		mismatch |= EN7572_TX_EYE_MISMATCH_TIA;
	if ((expected->erc_ctrl ^ actual->erc_ctrl) &
	    (EN7572_ERC_CDAC | EN7572_ERC_DAC))
		mismatch |= EN7572_TX_EYE_MISMATCH_ERC;
	if ((expected->pga_ctrl ^ actual->pga_ctrl) &
	    (EN7572_PGA_GAIN | EN7572_PGA_CAP))
		mismatch |= EN7572_TX_EYE_MISMATCH_PGA;
	if (expected->tssi != actual->tssi)
		mismatch |= EN7572_TX_EYE_MISMATCH_TSSI;
	if ((expected->loop_ctrl ^ actual->loop_ctrl) & EN7572_LOOP_ENABLE)
		mismatch |= EN7572_TX_EYE_MISMATCH_LOOP;

	return mismatch;
}

/*
 * Once the receive loop is running, MD32 may retune TIA bandwidth according
 * to the downstream optical input. The host still owns and validates TIA
 * current and gain; the complete field is checked immediately after an eye is
 * selected, before this runtime-only comparison is used.
 */
static inline unsigned int
en7572_tx_eye_runtime_mismatch(
		const struct en7572_tx_eye_fingerprint *expected,
		const struct en7572_tx_eye_fingerprint *actual)
{
	unsigned int mismatch = en7572_tx_eye_mismatch(expected, actual);

	if (!((expected->tia_ctrl ^ actual->tia_ctrl) &
	      (EN7572_TIA_CURRENT | EN7572_TIA_GAIN)))
		mismatch &= ~EN7572_TX_EYE_MISMATCH_TIA;

	return mismatch;
}

#endif
