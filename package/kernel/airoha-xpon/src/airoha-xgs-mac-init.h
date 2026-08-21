/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_XGS_MAC_INIT_H
#define __AIROHA_XGS_MAC_INIT_H

#include "airoha-xpon-mode.h"

enum airoha_xgs_mac_init_register {
	AIROHA_XGS_MAC_INIT_PLOAM_CONTROL,
	AIROHA_XGS_MAC_INIT_ACTIVATION,
	AIROHA_XGS_MAC_INIT_RSP_TIME,
	AIROHA_XGS_MAC_INIT_DS_FEC,
	AIROHA_XGS_MAC_INIT_DEBUG_CAP,
	AIROHA_XGS_MAC_INIT_DYING_GASP,
	AIROHA_XGS_MAC_INIT_PLOAM_DROP,
	AIROHA_XGS_MAC_INIT_TX_RESYNC,
	AIROHA_XGS_MAC_INIT_IDLE_GEM,
	AIROHA_XGS_MAC_INIT_MIB,
	AIROHA_XGS_MAC_INIT_REGISTER_COUNT,
};

enum airoha_xgs_mac_init_result {
	AIROHA_XGS_MAC_INIT_ROLLBACK_FAILED = 1,
};

struct airoha_xgs_mac_init_setting {
	enum airoha_xgs_mac_init_register reg;
	unsigned int mask;
	unsigned int value;
};

struct airoha_xgs_mac_init_ops {
	int (*read)(void *context, enum airoha_xgs_mac_init_register reg,
		    unsigned int *value);
	int (*update)(void *context, enum airoha_xgs_mac_init_register reg,
		      unsigned int mask, unsigned int value);
	void (*fail_closed)(void *context);
};

/* EN7581 XGS-PON defaults recovered from the vendor SDK. */
static const struct airoha_xgs_mac_init_setting
airoha_xgs_mac_init_settings[AIROHA_XGS_MAC_INIT_REGISTER_COUNT] = {
	/* Start dark in O1 with the SDK reset-default hardware reply mode. */
	{ AIROHA_XGS_MAC_INIT_PLOAM_CONTROL, 0x00000001U, 0x00000000U },
	{ AIROHA_XGS_MAC_INIT_ACTIVATION, 0x0000000fU, 0x00000001U },
	{ AIROHA_XGS_MAC_INIT_RSP_TIME, 0x00003fffU, 0x00001600U },
	{ AIROHA_XGS_MAC_INIT_DS_FEC, 0x00000003U, 0x00000003U },
	/* Do not repeat-filter PLOAMd; use downstream HW and upstream SW OMCI MIC. */
	{ AIROHA_XGS_MAC_INIT_DEBUG_CAP, 0x0000011bU, 0x00000110U },
	{ AIROHA_XGS_MAC_INIT_DYING_GASP, 0xfffff001U, 0x000ff001U },
	{ AIROHA_XGS_MAC_INIT_PLOAM_DROP, 0x00000001U, 0x00000000U },
	{ AIROHA_XGS_MAC_INIT_TX_RESYNC, 0x00001000U, 0x00000000U },
	{ AIROHA_XGS_MAC_INIT_IDLE_GEM, 0x0000ffffU, 0x00000120U },
	/* MAC reset enables MIB bit 0; make that prerequisite explicit here. */
	{ AIROHA_XGS_MAC_INIT_MIB, 0x00000101U, 0x00000101U },
};

/* XG-PON differs from XGS-PON in upstream response timing and idle XGEM. */
static const struct airoha_xgs_mac_init_setting
airoha_xgpon_mac_init_settings[AIROHA_XGS_MAC_INIT_REGISTER_COUNT] = {
	{ AIROHA_XGS_MAC_INIT_PLOAM_CONTROL, 0x00000001U, 0x00000000U },
	{ AIROHA_XGS_MAC_INIT_ACTIVATION, 0x0000000fU, 0x00000001U },
	{ AIROHA_XGS_MAC_INIT_RSP_TIME, 0x00003fffU, 0x00000551U },
	{ AIROHA_XGS_MAC_INIT_DS_FEC, 0x00000003U, 0x00000003U },
	{ AIROHA_XGS_MAC_INIT_DEBUG_CAP, 0x0000011bU, 0x00000110U },
	{ AIROHA_XGS_MAC_INIT_DYING_GASP, 0xfffff001U, 0x000ff001U },
	{ AIROHA_XGS_MAC_INIT_PLOAM_DROP, 0x00000001U, 0x00000000U },
	{ AIROHA_XGS_MAC_INIT_TX_RESYNC, 0x00001000U, 0x00000000U },
	{ AIROHA_XGS_MAC_INIT_IDLE_GEM, 0x0000ffffU, 0x0000001aU },
	{ AIROHA_XGS_MAC_INIT_MIB, 0x00000101U, 0x00000101U },
};

static inline const struct airoha_xgs_mac_init_setting *
airoha_xgs_mac_init_profile(enum airoha_xpon_mode mode)
{
	if (mode == AIROHA_XPON_MODE_XGPON)
		return airoha_xgpon_mac_init_settings;
	if (mode == AIROHA_XPON_MODE_XGSPON)
		return airoha_xgs_mac_init_settings;
	return NULL;
}

static inline int
airoha_xgs_mac_init_transaction_mode(
	const struct airoha_xgs_mac_init_ops *ops, void *context,
	enum airoha_xpon_mode mode)
{
	const struct airoha_xgs_mac_init_setting *settings;
	unsigned int previous[AIROHA_XGS_MAC_INIT_REGISTER_COUNT];
	unsigned int i;
	int ret;

	settings = airoha_xgs_mac_init_profile(mode);
	if (!settings)
		return -22;
	for (i = 0; i < AIROHA_XGS_MAC_INIT_REGISTER_COUNT; i++) {
		ret = ops->read(context, settings[i].reg,
				&previous[i]);
		if (ret) {
			if (ops->fail_closed)
				ops->fail_closed(context);
			return ret;
		}
	}

	for (i = 0; i < AIROHA_XGS_MAC_INIT_REGISTER_COUNT; i++) {
		const struct airoha_xgs_mac_init_setting *setting =
			&settings[i];

		ret = ops->update(context, setting->reg, setting->mask,
				  setting->value);
		if (ret) {
			bool rollback_failed = false;

			/* The failed write may be ambiguous, so restore it as well. */
			do {
				setting = &settings[i];
				rollback_failed |= ops->update(
					context, setting->reg, setting->mask,
					previous[i] & setting->mask) != 0;
			} while (i--);
			if (ops->fail_closed)
				ops->fail_closed(context);
			return rollback_failed ?
				AIROHA_XGS_MAC_INIT_ROLLBACK_FAILED : ret;
		}
	}

	return 0;
}

static inline int
airoha_xgs_mac_init_transaction(const struct airoha_xgs_mac_init_ops *ops,
				void *context)
{
	return airoha_xgs_mac_init_transaction_mode(
		ops, context, AIROHA_XPON_MODE_XGSPON);
}

#endif
