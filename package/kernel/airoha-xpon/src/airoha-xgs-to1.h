/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_XGS_TO1_H
#define __AIROHA_XGS_TO1_H

#ifdef __KERNEL__
#include <linux/jiffies.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

enum airoha_xgs_to1_decision {
	AIROHA_XGS_TO1_DISARM = 0,
	AIROHA_XGS_TO1_WAIT,
	AIROHA_XGS_TO1_EXPIRE,
};

struct airoha_xgs_to1_state {
	bool removing;
	bool enabled;
	bool hardware_selected;
	bool mac_present;
	unsigned long deadline;
	unsigned long long armed_generation;
	unsigned long long current_generation;
	unsigned int expected_state;
	unsigned int current_state;
};

struct airoha_xgs_to1_cleanup_ops {
	int (*disable_tx)(void *context);
	int (*clear_session)(void *context);
};

struct airoha_xgs_to1_cleanup_result {
	int disable_error;
	int session_error;
};

static inline bool airoha_xgs_to1_time_before(unsigned long now,
					       unsigned long deadline)
{
#ifdef __KERNEL__
	return time_before(now, deadline);
#else
	return (long)(now - deadline) < 0;
#endif
}

static inline enum airoha_xgs_to1_decision
airoha_xgs_to1_decide(const struct airoha_xgs_to1_state *state,
		      unsigned long now, unsigned int o4_state)
{
	if (!state || state->removing || !state->enabled ||
	    !state->hardware_selected || !state->mac_present ||
	    !state->deadline || state->expected_state != o4_state ||
	    state->armed_generation != state->current_generation ||
	    state->current_state != state->expected_state)
		return AIROHA_XGS_TO1_DISARM;

	return airoha_xgs_to1_time_before(now, state->deadline) ?
		AIROHA_XGS_TO1_WAIT : AIROHA_XGS_TO1_EXPIRE;
}

static inline int airoha_xgs_to1_cleanup_transaction(
	const struct airoha_xgs_to1_cleanup_ops *ops, void *context,
	struct airoha_xgs_to1_cleanup_result *result)
{
	result->disable_error = ops->disable_tx(context);
	result->session_error = ops->clear_session(context);

	return result->disable_error ? result->disable_error :
		result->session_error;
}

#endif
