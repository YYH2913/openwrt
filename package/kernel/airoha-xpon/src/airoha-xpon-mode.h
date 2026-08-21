/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_MODE_H_
#define _AIROHA_XPON_MODE_H_

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 airoha_xpon_u8;
typedef u32 airoha_xpon_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t airoha_xpon_u8;
typedef uint32_t airoha_xpon_u32;
#endif

/* Values are an internal ABI. WAN_SEL values live in the descriptor table. */
enum airoha_xpon_mode {
	AIROHA_XPON_MODE_GPON = 0,
	AIROHA_XPON_MODE_XGPON,
	AIROHA_XPON_MODE_XGSPON,
	AIROHA_XPON_MODE_EPON_10G_1G,
	AIROHA_XPON_MODE_EPON_10G_10G,
	AIROHA_XPON_MODE_COUNT,
	AIROHA_XPON_MODE_INVALID = 0xff,
};

struct airoha_xpon_mode_descriptor {
	const char *name;
	airoha_xpon_u8 wan_sel;
	airoha_xpon_u32 downstream_kbps;
	airoha_xpon_u32 upstream_kbps;
	bool uses_omci;
};

static inline const struct airoha_xpon_mode_descriptor *
airoha_xpon_mode_descriptor(enum airoha_xpon_mode mode)
{
	static const struct airoha_xpon_mode_descriptor modes[] = {
		[AIROHA_XPON_MODE_GPON] = {
			.name = "gpon",
			.wan_sel = 0,
			.downstream_kbps = 2488320,
			.upstream_kbps = 1244160,
			.uses_omci = true,
		},
		[AIROHA_XPON_MODE_XGPON] = {
			.name = "xgpon",
			.wan_sel = 9,
			.downstream_kbps = 9953280,
			.upstream_kbps = 2488320,
			.uses_omci = true,
		},
		[AIROHA_XPON_MODE_XGSPON] = {
			.name = "xgspon",
			.wan_sel = 10,
			.downstream_kbps = 9953280,
			.upstream_kbps = 9953280,
			.uses_omci = true,
		},
		[AIROHA_XPON_MODE_EPON_10G_1G] = {
			.name = "epon-10g-1g",
			.wan_sel = 6,
			.downstream_kbps = 10312500,
			.upstream_kbps = 1250000,
			.uses_omci = false,
		},
		[AIROHA_XPON_MODE_EPON_10G_10G] = {
			.name = "epon-10g-10g",
			.wan_sel = 7,
			.downstream_kbps = 10312500,
			.upstream_kbps = 10312500,
			.uses_omci = false,
		},
	};

	if ((unsigned int)mode >= AIROHA_XPON_MODE_COUNT)
		return NULL;

	return &modes[mode];
}

static inline bool airoha_xpon_mode_valid(enum airoha_xpon_mode mode)
{
	return airoha_xpon_mode_descriptor(mode) != NULL;
}

static inline bool airoha_xpon_mode_uses_omci(enum airoha_xpon_mode mode)
{
	const struct airoha_xpon_mode_descriptor *descriptor;

	descriptor = airoha_xpon_mode_descriptor(mode);
	return descriptor && descriptor->uses_omci;
}

#endif
