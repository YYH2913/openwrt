/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_MAC_H_
#define _AIROHA_EPON_MAC_H_

#ifdef __KERNEL__
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
#ifndef BIT
#define BIT(n) (1U << (n))
#endif
#ifndef GENMASK
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#endif
#ifndef FIELD_PREP
#define __bf_shf(x) (__builtin_ffs(x) - 1)
#define FIELD_PREP(mask, val) (((val) << __bf_shf(mask)) & (mask))
#endif
#endif

#include "airoha-xpon-mode.h"

#define AIROHA_EPON_LLID_COUNT		32

#define AIROHA_EPON_DISCOVERY_STATE_UNREGISTERED	0
#define AIROHA_EPON_DISCOVERY_STATE_REGISTERING		1
#define AIROHA_EPON_DISCOVERY_STATE_REGISTERED		2

#define AIROHA_EPON_REGISTER_FLAG_REREGISTER	0
#define AIROHA_EPON_REGISTER_FLAG_DEREGISTER	1
#define AIROHA_EPON_REGISTER_FLAG_ACK		2
#define AIROHA_EPON_REGISTER_FLAG_NACK		3

#define AIROHA_EPON_MPCP_CMD_DISCOVERY_REQUEST	1
#define AIROHA_EPON_MPCP_CMD_NORMAL_REQUEST	2
#define AIROHA_EPON_MPCP_CMD_REGISTER_ACK	3

#define AIROHA_EPON_MPCP_COMMAND		GENMASK(31, 30)
#define AIROHA_EPON_MPCP_COMMAND_DONE	BIT(16)
#define AIROHA_EPON_MPCP_ACK		BIT(12)
#define AIROHA_EPON_MPCP_REQUEST_FLAG	BIT(8)
#define AIROHA_EPON_MPCP_LLID_INDEX	GENMASK(4, 0)

#define AIROHA_EPON_MPCP_SYNC_TIME	GENMASK(15, 0)
#define AIROHA_EPON_MPCP_SYNC_TIME_DEFAULT	0x20
#define AIROHA_EPON_MPCP_SYNC_TIME_MAX		0x5f
#define AIROHA_EPON_MPCP_TIMESTAMP_ADJUST_BASE	0x002ffff1

/* The SDK records these MAC conditions but does not tear down the link. */
#define AIROHA_EPON_INT_GRANT_OVERRUN	BIT(9)
#define AIROHA_EPON_INT_TX_UNDERRUN	BIT(16)
#define AIROHA_EPON_INT2_DIAGNOSTICS	GENMASK(12, 0)

/* PHY_CSR_DUMMY_REG_RX[21]: 0 selects burst TX, 1 continuous TX. */
#define AIROHA_EPON_PHY_TX_CONTINUOUS	BIT(21)

#define AIROHA_EPON_LLID_DISCOVERY_STATE	GENMASK(31, 30)
#define AIROHA_EPON_LLID_REGISTER_FLAG	GENMASK(25, 24)
#define AIROHA_EPON_LLID_VALID		BIT(16)
#define AIROHA_EPON_LLID_VALUE		GENMASK(15, 0)

enum airoha_epon_mpcp_state {
	AIROHA_EPON_MPCP_WAIT,
	AIROHA_EPON_MPCP_REGISTERING,
	AIROHA_EPON_MPCP_REGISTER_REQUEST,
	AIROHA_EPON_MPCP_REGISTER_PENDING,
	AIROHA_EPON_MPCP_RETRY,
	AIROHA_EPON_MPCP_DENIED,
	AIROHA_EPON_MPCP_REGISTER_ACK,
	AIROHA_EPON_MPCP_NACK,
	AIROHA_EPON_MPCP_REGISTERED,
	AIROHA_EPON_MPCP_REMOTE_DEREGISTER,
	AIROHA_EPON_MPCP_LOCAL_DEREGISTER,
};

enum airoha_epon_mpcp_timeout_action {
	AIROHA_EPON_MPCP_TIMEOUT_IGNORE,
	AIROHA_EPON_MPCP_TIMEOUT_REINITIALIZE,
	AIROHA_EPON_MPCP_TIMEOUT_DEREGISTER,
};

struct airoha_epon_mpcp_sync_update {
	u16 sync_time;
	u32 timestamp_adjust;
	bool program_sync_time;
	bool program_timestamp_adjust;
};

static inline unsigned int
airoha_epon_nonfatal_irq_count(airoha_xpon_u32 status,
			       airoha_xpon_u32 status2)
{
	airoha_xpon_u32 pending = status &
		(AIROHA_EPON_INT_GRANT_OVERRUN | AIROHA_EPON_INT_TX_UNDERRUN);
	unsigned int count = 0;

	while (pending) {
		count += pending & 1;
		pending >>= 1;
	}
	pending = status2 & AIROHA_EPON_INT2_DIAGNOSTICS;
	while (pending) {
		count += pending & 1;
		pending >>= 1;
	}
	return count;
}

static inline struct airoha_epon_mpcp_sync_update
airoha_epon_mpcp_sync_update(u16 observed, u16 fallback,
			     bool any_registered)
{
	struct airoha_epon_mpcp_sync_update update = {};

	if (!fallback || fallback > AIROHA_EPON_MPCP_SYNC_TIME_MAX)
		fallback = AIROHA_EPON_MPCP_SYNC_TIME_DEFAULT;
	if (observed > AIROHA_EPON_MPCP_SYNC_TIME_MAX) {
		/* Match the SDK: never retime an already registered LLID. */
		if (any_registered)
			return update;
		update.sync_time = AIROHA_EPON_MPCP_SYNC_TIME_MAX;
		update.program_sync_time = true;
	} else if (!observed) {
		update.sync_time = fallback;
		update.program_sync_time = true;
	} else {
		update.sync_time = observed;
	}

	update.timestamp_adjust = AIROHA_EPON_MPCP_TIMESTAMP_ADJUST_BASE +
		((u32)update.sync_time << 16);
	update.program_timestamp_adjust = true;
	return update;
}

static inline enum airoha_epon_mpcp_timeout_action
airoha_epon_mpcp_timeout_action(bool enabled,
				enum airoha_epon_mpcp_state state,
				bool llid_valid)
{
	if (!enabled)
		return AIROHA_EPON_MPCP_TIMEOUT_IGNORE;
	if (state == AIROHA_EPON_MPCP_REGISTERED && llid_valid)
		return AIROHA_EPON_MPCP_TIMEOUT_DEREGISTER;
	return AIROHA_EPON_MPCP_TIMEOUT_REINITIALIZE;
}

static inline bool
airoha_epon_mpcp_data_ready(enum airoha_epon_mpcp_state state,
			    bool llid_valid)
{
	return state == AIROHA_EPON_MPCP_REGISTERED && llid_valid;
}

static inline bool
airoha_epon_mpcp_register_flag_allowed(enum airoha_epon_mpcp_state state,
				       airoha_xpon_u8 flag, bool llid_valid)
{
	switch (flag) {
	case AIROHA_EPON_REGISTER_FLAG_REREGISTER:
		return state == AIROHA_EPON_MPCP_REGISTERED && llid_valid;
	case AIROHA_EPON_REGISTER_FLAG_ACK:
		return state == AIROHA_EPON_MPCP_REGISTER_REQUEST && llid_valid;
	case AIROHA_EPON_REGISTER_FLAG_DEREGISTER:
		return true;
	case AIROHA_EPON_REGISTER_FLAG_NACK:
		return state != AIROHA_EPON_MPCP_WAIT;
	default:
		return false;
	}
}

struct airoha_epon_mode_profile {
	u32 report_config;
	u32 tx_fetch;
	u16 downstream_timestamp_adjust;
	u16 upstream_timestamp_adjust;
	bool upstream_10g;
	bool qdma_report_fec;
};

static inline const struct airoha_epon_mode_profile *
airoha_epon_mode_profile(enum airoha_xpon_mode mode)
{
	static const struct airoha_epon_mode_profile profiles[] = {
		[AIROHA_XPON_MODE_EPON_10G_1G] = {
			.report_config = 1,
			.tx_fetch = 0x002a03e8,
			.downstream_timestamp_adjust = 0xff90,
			.upstream_timestamp_adjust = 0,
			.upstream_10g = false,
			.qdma_report_fec = true,
		},
		[AIROHA_XPON_MODE_EPON_10G_10G] = {
			.report_config = 1,
			.tx_fetch = 0x002a03e8,
			.downstream_timestamp_adjust = 0xff90,
			.upstream_timestamp_adjust = 0x0008,
			.upstream_10g = true,
			.qdma_report_fec = false,
		},
	};

	if (mode != AIROHA_XPON_MODE_EPON_10G_1G &&
	    mode != AIROHA_XPON_MODE_EPON_10G_10G)
		return NULL;
	return &profiles[mode];
}

static inline airoha_xpon_u32
airoha_epon_mpcp_command(airoha_xpon_u8 command,
			airoha_xpon_u8 llid_index,
			bool request_deregister, bool ack)
{
	return FIELD_PREP(AIROHA_EPON_MPCP_COMMAND, command) |
	       FIELD_PREP(AIROHA_EPON_MPCP_LLID_INDEX, llid_index) |
	       (request_deregister ? AIROHA_EPON_MPCP_REQUEST_FLAG : 0) |
	       (ack ? AIROHA_EPON_MPCP_ACK : 0);
}

static inline airoha_xpon_u32
airoha_epon_phy_tx_mode(airoha_xpon_u32 value, bool burst)
{
	return burst ? value & ~AIROHA_EPON_PHY_TX_CONTINUOUS :
		value | AIROHA_EPON_PHY_TX_CONTINUOUS;
}

static inline airoha_xpon_u32
airoha_epon_llid_set_discovery_state(airoha_xpon_u32 value,
				     airoha_xpon_u8 state)
{
	return (value & ~AIROHA_EPON_LLID_DISCOVERY_STATE) |
	       FIELD_PREP(AIROHA_EPON_LLID_DISCOVERY_STATE, state);
}

#endif
