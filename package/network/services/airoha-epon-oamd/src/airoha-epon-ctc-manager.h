/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_CTC_MANAGER_H_
#define _AIROHA_EPON_CTC_MANAGER_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "airoha-epon-ctc.h"

#define AIROHA_CTC_MANAGEMENT_LLID_COUNT 32

typedef int (*airoha_ctc_management_apply_fn)(void *context,
		const struct airoha_ctc_management_set *effective);

struct airoha_ctc_management_manager {
	struct airoha_ctc_management_set llid[AIROHA_CTC_MANAGEMENT_LLID_COUNT];
	struct airoha_ctc_management_set effective;
	airoha_ctc_management_apply_fn apply;
	void *apply_context;
};

static inline bool airoha_ctc_management_policy_empty(
		const struct airoha_ctc_uni_policy *policy)
{
	return !policy->phy_admin_present && !policy->autoneg_present &&
	       !policy->autoneg_restart_present && !policy->pause_present &&
	       !policy->upstream_present && !policy->downstream_present &&
	       !policy->vlan_present && !policy->classification_present;
}

static inline bool airoha_ctc_management_set_empty(
		const struct airoha_ctc_management_set *state)
{
	unsigned int uni;

	if (!state || state->aging_present)
		return false;
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++)
		if (!airoha_ctc_management_policy_empty(&state->uni[uni]))
			return false;
	return true;
}

static inline bool airoha_ctc_management_has_valid_request(
		const struct airoha_ctc_management_set *state)
{
	unsigned int uni;

	if (!state)
		return false;
	if (state->aging_present && state->aging_valid)
		return true;
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++) {
		const struct airoha_ctc_uni_policy *policy = &state->uni[uni];

		if ((policy->phy_admin_present && policy->phy_admin_valid) ||
		    (policy->autoneg_present && policy->autoneg_valid) ||
		    (policy->autoneg_restart_present &&
		     policy->autoneg_restart_valid) ||
		    (policy->pause_present && policy->pause_valid) ||
		    (policy->upstream_present && policy->upstream_valid) ||
		    (policy->downstream_present && policy->downstream_valid) ||
		    (policy->vlan_present && policy->vlan_valid) ||
		    (policy->classification_present &&
		     policy->classification_valid))
			return true;
	}
	return false;
}

static inline bool airoha_ctc_management_policy_equal(
		const struct airoha_ctc_uni_policy *left,
		const struct airoha_ctc_uni_policy *right)
{
	if (left->phy_admin_present != right->phy_admin_present ||
	    left->autoneg_present != right->autoneg_present ||
	    left->pause_present != right->pause_present ||
	    left->upstream_present != right->upstream_present ||
	    left->downstream_present != right->downstream_present ||
	    left->vlan_present != right->vlan_present ||
	    left->classification_present != right->classification_present)
		return false;
	if (left->phy_admin_present &&
	    left->phy_admin_enabled != right->phy_admin_enabled)
		return false;
	if (left->autoneg_present &&
	    left->autoneg_enabled != right->autoneg_enabled)
		return false;
	if (left->pause_present &&
	    left->pause_enabled != right->pause_enabled)
		return false;
	if (left->upstream_present &&
	    !airoha_ctc_policy_upstream_equal(left, right->upstream_enabled,
		right->upstream_cir_kbps, right->upstream_cbs_bytes,
		right->upstream_ebs_bytes))
		return false;
	if (left->downstream_present &&
	    !airoha_ctc_policy_downstream_equal(left,
		right->downstream_enabled, right->downstream_cir_kbps,
		right->downstream_pir_kbps))
		return false;
	if (left->vlan_present && !airoha_ctc_policy_vlan_equal(left, right))
		return false;
	if (left->classification_present &&
	    !airoha_ctc_policy_classification_equal(left, right))
		return false;
	return true;
}

static inline bool airoha_ctc_management_has_transient(
		const struct airoha_ctc_management_set *request)
{
	unsigned int uni;

	if (!request)
		return false;
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++)
		if (request->uni[uni].autoneg_restart_present &&
		    request->uni[uni].autoneg_restart_valid)
			return true;
	return false;
}

static inline void airoha_ctc_management_copy_transient(
		struct airoha_ctc_management_set *destination,
		const struct airoha_ctc_management_set *request)
{
	unsigned int uni;

	if (!request)
		return;
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++) {
		destination->uni[uni].autoneg_restart_present =
			request->uni[uni].autoneg_restart_present &&
			request->uni[uni].autoneg_restart_valid;
		destination->uni[uni].autoneg_restart_valid =
			destination->uni[uni].autoneg_restart_present;
	}
}

static inline void airoha_ctc_management_copy_phy_admin(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *source)
{
	destination->phy_admin_present = source->phy_admin_present;
	destination->phy_admin_valid = source->phy_admin_present;
	destination->phy_admin_enabled = source->phy_admin_enabled;
	destination->phy_admin_applied = false;
}

static inline void airoha_ctc_management_copy_autoneg(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *source)
{
	destination->autoneg_present = source->autoneg_present;
	destination->autoneg_valid = source->autoneg_present;
	destination->autoneg_enabled = source->autoneg_enabled;
	destination->autoneg_applied = false;
}

static inline bool airoha_ctc_management_set_equal(
		const struct airoha_ctc_management_set *left,
		const struct airoha_ctc_management_set *right)
{
	unsigned int uni;

	if (left->aging_present != right->aging_present ||
	    (left->aging_present &&
	     left->aging_time_seconds != right->aging_time_seconds))
		return false;
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++)
		if (!airoha_ctc_management_policy_equal(&left->uni[uni],
			&right->uni[uni]))
			return false;
	return true;
}

static inline void airoha_ctc_management_copy_pause(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *source)
{
	destination->pause_present = source->pause_present;
	destination->pause_valid = source->pause_present;
	destination->pause_enabled = source->pause_enabled;
	destination->pause_applied = false;
}

static inline void airoha_ctc_management_copy_upstream(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *source)
{
	destination->upstream_present = source->upstream_present;
	destination->upstream_valid = source->upstream_present;
	destination->upstream_enabled = source->upstream_enabled;
	destination->upstream_applied = false;
	destination->upstream_cir_kbps = source->upstream_cir_kbps;
	destination->upstream_cbs_bytes = source->upstream_cbs_bytes;
	destination->upstream_ebs_bytes = source->upstream_ebs_bytes;
}

static inline void airoha_ctc_management_copy_downstream(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *source)
{
	destination->downstream_present = source->downstream_present;
	destination->downstream_valid = source->downstream_present;
	destination->downstream_enabled = source->downstream_enabled;
	destination->downstream_applied = false;
	destination->downstream_cir_kbps = source->downstream_cir_kbps;
	destination->downstream_pir_kbps = source->downstream_pir_kbps;
}

static inline void airoha_ctc_management_copy_vlan(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *source)
{
	destination->vlan_present = source->vlan_present;
	destination->vlan_valid = source->vlan_present;
	destination->vlan_applied = false;
	destination->vlan_mode = source->vlan_mode;
	destination->vlan_rule_count = source->vlan_rule_count;
	destination->vlan_default_tag = source->vlan_default_tag;
	memcpy(destination->vlan_old_tag, source->vlan_old_tag,
	       sizeof(destination->vlan_old_tag));
	memcpy(destination->vlan_new_tag, source->vlan_new_tag,
	       sizeof(destination->vlan_new_tag));
}

static inline void airoha_ctc_management_copy_classification(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *source)
{
	destination->classification_present = source->classification_present;
	destination->classification_valid = source->classification_present;
	destination->classification_applied = false;
	destination->classification_action = AIROHA_CTC_CLASSIFICATION_ADD;
	destination->classification_rule_count = source->classification_rule_count;
	memcpy(destination->classification_rule, source->classification_rule,
	       sizeof(destination->classification_rule));
}

static inline int airoha_ctc_management_classification_insert(
		struct airoha_ctc_uni_policy *policy,
		const struct airoha_ctc_classification_rule *rule)
{
	unsigned int index, position, expected;

	if (policy->classification_rule_count >=
	    AIROHA_CTC_CLASSIFICATION_RULE_MAX)
		return -ENOSPC;
	for (position = 0; position < policy->classification_rule_count;
	     position++)
		if (policy->classification_rule[position].precedence >=
		    rule->precedence)
			break;
	expected = rule->precedence;
	for (index = position; index < policy->classification_rule_count;
	     index++) {
		if (policy->classification_rule[index].precedence != expected)
			break;
		if (expected == UINT8_MAX)
			return -ERANGE;
		expected++;
	}
	memmove(policy->classification_rule + position + 1,
		policy->classification_rule + position,
		(policy->classification_rule_count - position) *
			sizeof(policy->classification_rule[0]));
	policy->classification_rule[position] = *rule;
	policy->classification_rule_count++;
	expected = rule->precedence;
	for (index = position + 1; index < policy->classification_rule_count;
	     index++) {
		if (policy->classification_rule[index].precedence != expected)
			break;
		policy->classification_rule[index].precedence++;
		expected++;
	}
	policy->classification_present = true;
	policy->classification_valid = true;
	policy->classification_action = AIROHA_CTC_CLASSIFICATION_ADD;
	return 0;
}

static inline void airoha_ctc_management_classification_delete(
		struct airoha_ctc_uni_policy *policy,
		const struct airoha_ctc_classification_rule *rule)
{
	unsigned int index;

	if (!policy->classification_present)
		return;
	for (index = 0; index < policy->classification_rule_count; index++) {
		if (policy->classification_rule[index].precedence < rule->precedence ||
		    !airoha_ctc_classification_rule_equal(
			&policy->classification_rule[index], rule, false, false))
			continue;
		memmove(policy->classification_rule + index,
			policy->classification_rule + index + 1,
			(policy->classification_rule_count - index - 1) *
				sizeof(policy->classification_rule[0]));
		policy->classification_rule_count--;
		memset(&policy->classification_rule[policy->classification_rule_count],
		       0, sizeof(policy->classification_rule[0]));
		break;
	}
}

static inline int airoha_ctc_management_apply_classification_delta(
		struct airoha_ctc_uni_policy *destination,
		const struct airoha_ctc_uni_policy *request)
{
	unsigned int rule;
	int result;

	if (request->classification_action == AIROHA_CTC_CLASSIFICATION_CLEAR) {
		destination->classification_present = false;
		destination->classification_valid = false;
		destination->classification_rule_count = 0;
		memset(destination->classification_rule, 0,
		       sizeof(destination->classification_rule));
		return 0;
	}
	for (rule = 0; rule < request->classification_rule_count; rule++) {
		if (request->classification_action ==
		    AIROHA_CTC_CLASSIFICATION_ADD) {
			result = airoha_ctc_management_classification_insert(destination,
				&request->classification_rule[rule]);
			if (result)
				return result;
		} else {
			airoha_ctc_management_classification_delete(destination,
				&request->classification_rule[rule]);
		}
	}
	return 0;
}

static inline int airoha_ctc_management_merge_override(
		const struct airoha_ctc_management_manager *manager,
		unsigned int override_llid,
		const struct airoha_ctc_management_set *override,
		struct airoha_ctc_management_set *effective)
{
	unsigned int llid, uni;

	memset(effective, 0, sizeof(*effective));
	for (llid = 0; llid < AIROHA_CTC_MANAGEMENT_LLID_COUNT; llid++) {
		const struct airoha_ctc_management_set *source =
			llid == override_llid ? override : &manager->llid[llid];

		if (source->aging_present) {
			if (effective->aging_present &&
			    effective->aging_time_seconds !=
				    source->aging_time_seconds)
				return -EADDRINUSE;
			effective->aging_present = true;
			effective->aging_valid = true;
			effective->aging_time_seconds = source->aging_time_seconds;
		}
		for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++) {
			const struct airoha_ctc_uni_policy *input =
				&source->uni[uni];
			struct airoha_ctc_uni_policy *output =
				&effective->uni[uni];

			if (input->phy_admin_present) {
				if (output->phy_admin_present &&
				    output->phy_admin_enabled !=
					    input->phy_admin_enabled)
					return -EADDRINUSE;
				airoha_ctc_management_copy_phy_admin(output, input);
			}
			if (input->autoneg_present) {
				if (output->autoneg_present &&
				    output->autoneg_enabled != input->autoneg_enabled)
					return -EADDRINUSE;
				airoha_ctc_management_copy_autoneg(output, input);
			}
			if (input->pause_present) {
				if (output->pause_present &&
				    output->pause_enabled != input->pause_enabled)
					return -EADDRINUSE;
				airoha_ctc_management_copy_pause(output, input);
			}
			if (input->upstream_present) {
				if (output->upstream_present &&
				    !airoha_ctc_policy_upstream_equal(output,
					input->upstream_enabled,
					input->upstream_cir_kbps,
					input->upstream_cbs_bytes,
					input->upstream_ebs_bytes))
					return -EADDRINUSE;
				airoha_ctc_management_copy_upstream(output, input);
			}
			if (input->downstream_present) {
				if (output->downstream_present &&
				    !airoha_ctc_policy_downstream_equal(output,
					input->downstream_enabled,
					input->downstream_cir_kbps,
					input->downstream_pir_kbps))
					return -EADDRINUSE;
				airoha_ctc_management_copy_downstream(output, input);
			}
			if (input->vlan_present) {
				if (output->vlan_present &&
				    !airoha_ctc_policy_vlan_equal(output, input))
					return -EADDRINUSE;
				airoha_ctc_management_copy_vlan(output, input);
			}
			if (input->classification_present) {
				if (output->classification_present &&
				    !airoha_ctc_policy_classification_equal(output, input))
					return -EADDRINUSE;
				airoha_ctc_management_copy_classification(output, input);
			}
		}
	}
	return 0;
}

static inline int airoha_ctc_management_merge(
		const struct airoha_ctc_management_manager *manager,
		struct airoha_ctc_management_set *effective)
{
	return airoha_ctc_management_merge_override(manager,
		AIROHA_CTC_MANAGEMENT_LLID_COUNT, NULL, effective);
}

static inline int airoha_ctc_management_update_llid(
		struct airoha_ctc_management_set *destination,
		const struct airoha_ctc_management_set *request)
{
	unsigned int uni;
	int result;

	if (request->aging_present && request->aging_valid) {
		destination->aging_present = true;
		destination->aging_valid = true;
		destination->aging_time_seconds = request->aging_time_seconds;
	}
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++) {
		const struct airoha_ctc_uni_policy *input = &request->uni[uni];
		struct airoha_ctc_uni_policy *output = &destination->uni[uni];

		if (input->phy_admin_present && input->phy_admin_valid)
			airoha_ctc_management_copy_phy_admin(output, input);
		if (input->autoneg_present && input->autoneg_valid)
			airoha_ctc_management_copy_autoneg(output, input);
		if (input->pause_present && input->pause_valid)
			airoha_ctc_management_copy_pause(output, input);
		if (input->upstream_present && input->upstream_valid)
			airoha_ctc_management_copy_upstream(output, input);
		if (input->downstream_present && input->downstream_valid)
			airoha_ctc_management_copy_downstream(output, input);
		if (input->vlan_present && input->vlan_valid)
			airoha_ctc_management_copy_vlan(output, input);
		if (input->classification_present && input->classification_valid) {
			result = airoha_ctc_management_apply_classification_delta(output,
				input);
			if (result)
				return result;
		}
	}
	return 0;
}

static inline void airoha_ctc_management_mark_applied(
		struct airoha_ctc_management_set *request)
{
	unsigned int uni;

	if (request->aging_present && request->aging_valid)
		request->aging_applied = true;
	for (uni = 0; uni < AIROHA_CTC_ETHERNET_UNI_COUNT; uni++) {
		struct airoha_ctc_uni_policy *policy = &request->uni[uni];

		if (policy->phy_admin_present && policy->phy_admin_valid)
			policy->phy_admin_applied = true;
		if (policy->autoneg_present && policy->autoneg_valid)
			policy->autoneg_applied = true;
		if (policy->autoneg_restart_present &&
		    policy->autoneg_restart_valid)
			policy->autoneg_restart_applied = true;
		if (policy->pause_present && policy->pause_valid)
			policy->pause_applied = true;
		if (policy->upstream_present && policy->upstream_valid)
			policy->upstream_applied = true;
		if (policy->downstream_present && policy->downstream_valid)
			policy->downstream_applied = true;
		if (policy->vlan_present && policy->vlan_valid)
			policy->vlan_applied = true;
		if (policy->classification_present && policy->classification_valid)
			policy->classification_applied = true;
	}
}

static inline int airoha_ctc_management_apply_effective(
		struct airoha_ctc_management_manager *manager,
		const struct airoha_ctc_management_set *effective,
		const struct airoha_ctc_management_set *transient)
{
	struct airoha_ctc_management_set platform = *effective;
	int result = 0;

	airoha_ctc_management_copy_transient(&platform, transient);
	if (!airoha_ctc_management_set_equal(&manager->effective, effective) ||
	    airoha_ctc_management_has_transient(transient)) {
		if (!manager->apply)
			return -EOPNOTSUPP;
		result = manager->apply(manager->apply_context, &platform);
		if (result)
			return result;
	}
	return 0;
}

static inline int airoha_ctc_management_apply(
		struct airoha_ctc_management_manager *manager, unsigned int llid,
		struct airoha_ctc_management_set *request)
{
	struct airoha_ctc_management_set candidate;
	struct airoha_ctc_management_set effective;
	int result;

	if (!manager || !request || llid >= AIROHA_CTC_MANAGEMENT_LLID_COUNT)
		return -EINVAL;
	candidate = manager->llid[llid];
	result = airoha_ctc_management_update_llid(&candidate, request);
	if (result)
		return result;
	result = airoha_ctc_management_merge_override(manager, llid, &candidate,
		&effective);
	if (result)
		return result;
	result = airoha_ctc_management_apply_effective(manager, &effective,
		request);
	if (result)
		return result;
	manager->llid[llid] = candidate;
	manager->effective = effective;
	airoha_ctc_management_mark_applied(request);
	return 0;
}

static inline int airoha_ctc_management_clear_llid(
		struct airoha_ctc_management_manager *manager, unsigned int llid)
{
	const struct airoha_ctc_management_set empty = { 0 };
	struct airoha_ctc_management_set effective;
	int result;

	if (!manager || llid >= AIROHA_CTC_MANAGEMENT_LLID_COUNT)
		return -EINVAL;
	result = airoha_ctc_management_merge_override(manager, llid, &empty,
		&effective);
	if (result)
		return result;
	result = airoha_ctc_management_apply_effective(manager, &effective, NULL);
	if (result)
		return result;
	memset(&manager->llid[llid], 0, sizeof(manager->llid[llid]));
	manager->effective = effective;
	return 0;
}

static inline int airoha_ctc_management_clear_all(
		struct airoha_ctc_management_manager *manager)
{
	struct airoha_ctc_management_set effective = { 0 };
	int result;

	if (!manager)
		return -EINVAL;
	result = airoha_ctc_management_apply_effective(manager, &effective, NULL);
	if (result)
		return result;
	memset(manager->llid, 0, sizeof(manager->llid));
	manager->effective = effective;
	return 0;
}

#endif
