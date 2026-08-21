/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_EPON_MAC_INIT_H
#define __AIROHA_EPON_MAC_INIT_H

#include "airoha-xpon-mode.h"

enum airoha_epon_mac_init_register {
	AIROHA_EPON_MAC_INIT_REPORT_BITMAP,
	AIROHA_EPON_MAC_INIT_REPORT_CONFIG,
	AIROHA_EPON_MAC_INIT_REPORT_CONFIG2,
	AIROHA_EPON_MAC_INIT_REPORT_QSIZE_ADJUST,
	AIROHA_EPON_MAC_INIT_LASER_TIME,
	AIROHA_EPON_MAC_INIT_SYNC_TIME,
	AIROHA_EPON_MAC_INIT_MPCP_TIMEOUT,
	AIROHA_EPON_MAC_INIT_TRX_ADJUST1,
	AIROHA_EPON_MAC_INIT_TRX_ADJUST2,
	AIROHA_EPON_MAC_INIT_TRX_ADJUST3,
	AIROHA_EPON_MAC_INIT_TRX_ADJUST4,
	AIROHA_EPON_MAC_INIT_TX_FETCH,
	AIROHA_EPON_MAC_INIT_TX_CAL,
	AIROHA_EPON_MAC_INIT_DYING_GASP,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD1,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD2,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD3,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD4,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD5,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD6,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD7,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD8,
	AIROHA_EPON_MAC_INIT_DYING_GASP_WORD9,
	AIROHA_EPON_MAC_INIT_REGISTER_COUNT,
};

enum airoha_epon_mac_init_result {
	AIROHA_EPON_MAC_INIT_ROLLBACK_FAILED = -117,
};

struct airoha_epon_mac_init_setting {
	enum airoha_epon_mac_init_register reg;
	unsigned int mask;
	unsigned int value;
};

struct airoha_epon_mac_init_ops {
	int (*read)(void *context, enum airoha_epon_mac_init_register reg,
		    unsigned int *value);
	int (*update)(void *context, enum airoha_epon_mac_init_register reg,
		      unsigned int mask, unsigned int value);
	void (*fail_closed)(void *context);
};

/* AN7581 10G/1G EPON values and writable masks recovered from the SDK. */
static const struct airoha_epon_mac_init_setting
airoha_epon_10g_1g_mac_init_settings[
	AIROHA_EPON_MAC_INIT_REGISTER_COUNT] = {
	{ AIROHA_EPON_MAC_INIT_REPORT_BITMAP, 0x0000ffffU, 0x0000ffffU },
	{ AIROHA_EPON_MAC_INIT_REPORT_CONFIG, 0xffffffffU, 0x00000001U },
	{ AIROHA_EPON_MAC_INIT_REPORT_CONFIG2, 0xffffffffU, 0x00000000U },
	{ AIROHA_EPON_MAC_INIT_REPORT_QSIZE_ADJUST,
	  0xffffffffU, 0x00f100f1U },
	{ AIROHA_EPON_MAC_INIT_LASER_TIME, 0x0000ffffU, 0x00002020U },
	{ AIROHA_EPON_MAC_INIT_SYNC_TIME, 0x0000ffffU, 0x00000020U },
	{ AIROHA_EPON_MAC_INIT_MPCP_TIMEOUT, 0xffffffffU, 0x000001f4U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST1, 0x0000ffffU, 0x0000fff1U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST2, 0xffffffffU, 0x00000006U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST3, 0xffffffffU, 0x00000000U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST4, 0xffff1f00U, 0xff900000U },
	{ AIROHA_EPON_MAC_INIT_TX_FETCH, 0x00ffffffU, 0x002a03e8U },
	{ AIROHA_EPON_MAC_INIT_TX_CAL, 0x0000003fU, 0x00000008U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP, 0x8000ff00U, 0x00000100U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD1,
	  0xffffffffU, 0x88090300U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD2,
	  0xffffffffU, 0x52000110U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD3,
	  0xffffffffU, 0x01000000U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD4,
	  0xffffffffU, 0x0f05ee00U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD5,
	  0xffffffffU, 0x13250022U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD6,
	  0xffffffffU, 0x01000210U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD7,
	  0xffffffffU, 0x01000000U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD8,
	  0xffffffffU, 0x0f05ee00U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD9,
	  0xffffffffU, 0x13250000U },
};

/* Symmetric EPON selects the 10G report adjust and upstream STM offset. */
static const struct airoha_epon_mac_init_setting
airoha_epon_10g_10g_mac_init_settings[
	AIROHA_EPON_MAC_INIT_REGISTER_COUNT] = {
	{ AIROHA_EPON_MAC_INIT_REPORT_BITMAP, 0x0000ffffU, 0x0000ffffU },
	{ AIROHA_EPON_MAC_INIT_REPORT_CONFIG, 0xffffffffU, 0x00000001U },
	{ AIROHA_EPON_MAC_INIT_REPORT_CONFIG2, 0xffffffffU, 0x00000000U },
	{ AIROHA_EPON_MAC_INIT_REPORT_QSIZE_ADJUST,
	  0x0000ffffU, 0x00000019U },
	{ AIROHA_EPON_MAC_INIT_LASER_TIME, 0x0000ffffU, 0x00002020U },
	{ AIROHA_EPON_MAC_INIT_SYNC_TIME, 0x0000ffffU, 0x00000020U },
	{ AIROHA_EPON_MAC_INIT_MPCP_TIMEOUT, 0xffffffffU, 0x000001f4U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST1, 0x0000ffffU, 0x0000fff1U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST2, 0xffffffffU, 0x00000006U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST3, 0xffffffffU, 0x00000008U },
	{ AIROHA_EPON_MAC_INIT_TRX_ADJUST4, 0xffff1f00U, 0xff900000U },
	{ AIROHA_EPON_MAC_INIT_TX_FETCH, 0x00ffffffU, 0x002a03e8U },
	{ AIROHA_EPON_MAC_INIT_TX_CAL, 0x0000003fU, 0x00000008U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP, 0x8000ff00U, 0x00000100U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD1,
	  0xffffffffU, 0x88090300U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD2,
	  0xffffffffU, 0x52000110U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD3,
	  0xffffffffU, 0x01000000U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD4,
	  0xffffffffU, 0x0f05ee00U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD5,
	  0xffffffffU, 0x13250022U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD6,
	  0xffffffffU, 0x01000210U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD7,
	  0xffffffffU, 0x01000000U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD8,
	  0xffffffffU, 0x0f05ee00U },
	{ AIROHA_EPON_MAC_INIT_DYING_GASP_WORD9,
	  0xffffffffU, 0x13250000U },
};

static inline const struct airoha_epon_mac_init_setting *
airoha_epon_mac_init_profile(enum airoha_xpon_mode mode)
{
	if (mode == AIROHA_XPON_MODE_EPON_10G_1G)
		return airoha_epon_10g_1g_mac_init_settings;
	if (mode == AIROHA_XPON_MODE_EPON_10G_10G)
		return airoha_epon_10g_10g_mac_init_settings;
	return NULL;
}

static inline int
airoha_epon_mac_init_transaction(const struct airoha_epon_mac_init_ops *ops,
				 void *context, enum airoha_xpon_mode mode)
{
	const struct airoha_epon_mac_init_setting *settings;
	unsigned int previous[AIROHA_EPON_MAC_INIT_REGISTER_COUNT];
	unsigned int i;
	int ret;

	settings = airoha_epon_mac_init_profile(mode);
	if (!settings || !ops || !ops->read || !ops->update)
		return -22;
	for (i = 0; i < AIROHA_EPON_MAC_INIT_REGISTER_COUNT; i++) {
		ret = ops->read(context, settings[i].reg, &previous[i]);
		if (ret) {
			if (ops->fail_closed)
				ops->fail_closed(context);
			return ret;
		}
	}

	for (i = 0; i < AIROHA_EPON_MAC_INIT_REGISTER_COUNT; i++) {
		const struct airoha_epon_mac_init_setting *setting =
			&settings[i];

		ret = ops->update(context, setting->reg, setting->mask,
				  setting->value);
		if (ret) {
			bool rollback_failed = false;

			/* A failed readback leaves the write ambiguous; restore it too. */
			do {
				setting = &settings[i];
				rollback_failed |= ops->update(
					context, setting->reg, setting->mask,
					previous[i] & setting->mask) != 0;
			} while (i--);
			if (ops->fail_closed)
				ops->fail_closed(context);
			return rollback_failed ?
				AIROHA_EPON_MAC_INIT_ROLLBACK_FAILED : ret;
		}
	}

	return 0;
}

#endif
