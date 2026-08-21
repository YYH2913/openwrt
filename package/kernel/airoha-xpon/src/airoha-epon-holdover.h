/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_HOLDOVER_H_
#define _AIROHA_EPON_HOLDOVER_H_

#include <linux/types.h>

#define AIROHA_EPON_HOLDOVER_TIME_MIN_MS	50
#define AIROHA_EPON_HOLDOVER_TIME_MAX_MS	1000
#define AIROHA_EPON_HOLDOVER_TIME_DEFAULT_MS	1000

enum airoha_epon_holdover_action {
	AIROHA_EPON_HOLDOVER_NONE,
	AIROHA_EPON_HOLDOVER_START,
	AIROHA_EPON_HOLDOVER_PRESERVE,
	AIROHA_EPON_HOLDOVER_RESTORE,
	AIROHA_EPON_HOLDOVER_CLEAR,
};

struct airoha_epon_holdover_state {
	unsigned int time_ms;
	bool enabled;
	bool active;
};

static inline bool airoha_epon_holdover_config_valid(bool enabled,
						      unsigned int time_ms)
{
	(void)enabled;
	return time_ms >= AIROHA_EPON_HOLDOVER_TIME_MIN_MS &&
	       time_ms <= AIROHA_EPON_HOLDOVER_TIME_MAX_MS;
}

static inline enum airoha_epon_holdover_action
airoha_epon_holdover_loss(struct airoha_epon_holdover_state *state)
{
	if (!state || !state->enabled)
		return AIROHA_EPON_HOLDOVER_CLEAR;
	if (state->active)
		return AIROHA_EPON_HOLDOVER_PRESERVE;
	state->active = true;
	return AIROHA_EPON_HOLDOVER_START;
}

static inline enum airoha_epon_holdover_action
airoha_epon_holdover_ready(struct airoha_epon_holdover_state *state)
{
	if (!state || !state->active)
		return AIROHA_EPON_HOLDOVER_NONE;
	state->active = false;
	return AIROHA_EPON_HOLDOVER_RESTORE;
}

static inline enum airoha_epon_holdover_action
airoha_epon_holdover_timeout(struct airoha_epon_holdover_state *state,
						     bool phy_ready)
{
	if (!state || !state->active)
		return AIROHA_EPON_HOLDOVER_NONE;
	state->active = false;
	return phy_ready ? AIROHA_EPON_HOLDOVER_RESTORE :
		AIROHA_EPON_HOLDOVER_CLEAR;
}

static inline bool
airoha_epon_holdover_cancel(struct airoha_epon_holdover_state *state)
{
	bool active;

	if (!state)
		return false;
	active = state->active;
	state->active = false;
	return active;
}

#endif
