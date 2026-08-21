#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "airoha-epon-ctc-manager.h"

struct fake_backend {
	struct airoha_ctc_management_set last;
	unsigned int calls;
	int result;
};

static int fake_apply(void *context,
		const struct airoha_ctc_management_set *effective)
{
	struct fake_backend *backend = context;

	backend->calls++;
	backend->last = *effective;
	return backend->result;
}

static struct airoha_ctc_management_set pause_request(unsigned int uni,
		bool enabled)
{
	struct airoha_ctc_management_set request = { 0 };

	request.uni[uni].pause_present = true;
	request.uni[uni].pause_valid = true;
	request.uni[uni].pause_enabled = enabled;
	return request;
}

static struct airoha_ctc_management_set upstream_request(unsigned int uni,
		uint32_t cir, uint32_t cbs, uint32_t ebs)
{
	struct airoha_ctc_management_set request = { 0 };

	request.uni[uni].upstream_present = true;
	request.uni[uni].upstream_valid = true;
	request.uni[uni].upstream_enabled = true;
	request.uni[uni].upstream_cir_kbps = cir;
	request.uni[uni].upstream_cbs_bytes = cbs;
	request.uni[uni].upstream_ebs_bytes = ebs;
	return request;
}

static struct airoha_ctc_management_set vlan_request(unsigned int uni,
		uint32_t old_tag, uint32_t new_tag)
{
	struct airoha_ctc_management_set request = { 0 };
	struct airoha_ctc_uni_policy *policy = &request.uni[uni];

	policy->vlan_present = true;
	policy->vlan_valid = true;
	policy->vlan_mode = AIROHA_CTC_VLAN_MODE_TRANSLATION;
	policy->vlan_default_tag = UINT32_C(0x81000064);
	policy->vlan_rule_count = 1;
	policy->vlan_old_tag[0] = old_tag;
	policy->vlan_new_tag[0] = new_tag;
	return request;
}

static struct airoha_ctc_classification_match classification_u8_match(
		uint8_t field, uint8_t value)
{
	struct airoha_ctc_classification_match match = {
		.field = field,
		.operation = AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		.value_length = 1,
		.value = { value },
	};

	return match;
}

static struct airoha_ctc_classification_match classification_u16_match(
		uint8_t field, uint16_t value)
{
	struct airoha_ctc_classification_match match = {
		.field = field,
		.operation = AIROHA_CTC_CLASSIFICATION_OP_EQUAL,
		.value_length = 2,
		.value = { value >> 8, value & 0xff },
	};

	return match;
}

static struct airoha_ctc_uni_policy *classification_delta(
		struct airoha_ctc_management_set *request, unsigned int uni,
		uint8_t action)
{
	struct airoha_ctc_uni_policy *policy;

	memset(request, 0, sizeof(*request));
	policy = &request->uni[uni];
	policy->classification_present = true;
	policy->classification_valid = true;
	policy->classification_action = action;
	return policy;
}

static void test_identical_merge_and_clear(void)
{
	struct fake_backend backend = { 0 };
	struct airoha_ctc_management_manager manager = {
		.apply = fake_apply,
		.apply_context = &backend,
	};
	struct airoha_ctc_management_set first = pause_request(0, true);
	struct airoha_ctc_management_set second = pause_request(0, true);
	struct airoha_ctc_management_set conflict = pause_request(0, false);

	assert(airoha_ctc_management_apply(&manager, 0, &first) == 0);
	assert(first.uni[0].pause_applied);
	assert(backend.calls == 1 && backend.last.uni[0].pause_enabled);
	assert(airoha_ctc_management_apply(&manager, 1, &second) == 0);
	assert(second.uni[0].pause_applied);
	assert(backend.calls == 1);
	assert(airoha_ctc_management_apply(&manager, 2, &conflict) ==
	       -EADDRINUSE);
	assert(!conflict.uni[0].pause_applied && backend.calls == 1);
	assert(!manager.llid[2].uni[0].pause_present);

	assert(airoha_ctc_management_clear_llid(&manager, 0) == 0);
	assert(backend.calls == 1 && manager.effective.uni[0].pause_present);
	assert(airoha_ctc_management_clear_llid(&manager, 1) == 0);
	assert(backend.calls == 2);
	assert(airoha_ctc_management_set_empty(&manager.effective));
}

static void test_atomic_backend_failure(void)
{
	struct fake_backend backend = { .result = -EIO };
	struct airoha_ctc_management_manager manager = {
		.apply = fake_apply,
		.apply_context = &backend,
	};
	struct airoha_ctc_management_set request = upstream_request(3,
		100000, 8192, 4096);

	assert(airoha_ctc_management_apply(&manager, 7, &request) == -EIO);
	assert(backend.calls == 1 && !request.uni[3].upstream_applied);
	assert(!manager.llid[7].uni[3].upstream_present);
	assert(airoha_ctc_management_set_empty(&manager.effective));

	backend.result = 0;
	assert(airoha_ctc_management_apply(&manager, 7, &request) == 0);
	assert(request.uni[3].upstream_applied && backend.calls == 2);
	backend.result = -EBUSY;
	assert(airoha_ctc_management_clear_llid(&manager, 7) == -EBUSY);
	assert(manager.llid[7].uni[3].upstream_present);
	assert(manager.effective.uni[3].upstream_present);
	backend.result = 0;
	assert(airoha_ctc_management_clear_all(&manager) == 0);
	assert(airoha_ctc_management_set_empty(&manager.effective));
}

static void test_global_aging_conflict(void)
{
	struct fake_backend backend = { 0 };
	struct airoha_ctc_management_manager manager = {
		.apply = fake_apply,
		.apply_context = &backend,
	};
	struct airoha_ctc_management_set first = {
		.aging_present = true,
		.aging_valid = true,
		.aging_time_seconds = 300,
	};
	struct airoha_ctc_management_set second = first;
	struct airoha_ctc_management_set conflict = first;

	conflict.aging_time_seconds = 600;
	assert(airoha_ctc_management_apply(&manager, 3, &first) == 0);
	assert(first.aging_applied && backend.calls == 1);
	assert(airoha_ctc_management_apply(&manager, 4, &second) == 0);
	assert(second.aging_applied && backend.calls == 1);
	assert(airoha_ctc_management_apply(&manager, 5, &conflict) ==
	       -EADDRINUSE);
	assert(!conflict.aging_applied && backend.calls == 1);
}

static void test_phy_autoneg_and_transient_restart(void)
{
	struct fake_backend backend = { 0 };
	struct airoha_ctc_management_manager manager = {
		.apply = fake_apply,
		.apply_context = &backend,
	};
	struct airoha_ctc_management_set request = { 0 };
	struct airoha_ctc_management_set identical = { 0 };
	struct airoha_ctc_management_set restart = { 0 };
	struct airoha_ctc_management_set conflict = { 0 };

	request.uni[1].phy_admin_present = true;
	request.uni[1].phy_admin_valid = true;
	request.uni[1].phy_admin_enabled = true;
	request.uni[1].autoneg_present = true;
	request.uni[1].autoneg_valid = true;
	request.uni[1].autoneg_enabled = false;
	request.uni[1].autoneg_restart_present = true;
	request.uni[1].autoneg_restart_valid = true;
	identical = request;
	identical.uni[1].autoneg_restart_present = false;
	identical.uni[1].autoneg_restart_valid = false;
	restart.uni[1].autoneg_restart_present = true;
	restart.uni[1].autoneg_restart_valid = true;
	conflict.uni[1].autoneg_present = true;
	conflict.uni[1].autoneg_valid = true;
	conflict.uni[1].autoneg_enabled = true;

	assert(airoha_ctc_management_apply(&manager, 0, &request) == 0);
	assert(backend.calls == 1);
	assert(backend.last.uni[1].phy_admin_enabled);
	assert(!backend.last.uni[1].autoneg_enabled);
	assert(backend.last.uni[1].autoneg_restart_present);
	assert(request.uni[1].phy_admin_applied);
	assert(request.uni[1].autoneg_applied);
	assert(request.uni[1].autoneg_restart_applied);
	assert(manager.effective.uni[1].phy_admin_present);
	assert(manager.effective.uni[1].autoneg_present);
	assert(!manager.effective.uni[1].autoneg_restart_present);
	assert(!manager.llid[0].uni[1].autoneg_restart_present);

	assert(airoha_ctc_management_apply(&manager, 1, &identical) == 0);
	assert(backend.calls == 1);
	assert(airoha_ctc_management_apply(&manager, 2, &restart) == 0);
	assert(backend.calls == 2 && restart.uni[1].autoneg_restart_applied);
	assert(backend.last.uni[1].autoneg_restart_present);
	assert(!manager.llid[2].uni[1].autoneg_restart_present);
	assert(airoha_ctc_management_apply(&manager, 3, &conflict) ==
	       -EADDRINUSE);
	assert(backend.calls == 2 && !conflict.uni[1].autoneg_applied);
}

static void test_vlan_multi_llid_conflict(void)
{
	struct fake_backend backend = { 0 };
	struct airoha_ctc_management_manager manager = {
		.apply = fake_apply,
		.apply_context = &backend,
	};
	struct airoha_ctc_management_set first = vlan_request(2,
		UINT32_C(0x8100000a), UINT32_C(0x810000c8));
	struct airoha_ctc_management_set identical = first;
	struct airoha_ctc_management_set conflict = vlan_request(2,
		UINT32_C(0x8100000a), UINT32_C(0x8100012c));

	assert(airoha_ctc_management_apply(&manager, 4, &first) == 0);
	assert(first.uni[2].vlan_applied && backend.calls == 1);
	assert(backend.last.uni[2].vlan_new_tag[0] ==
	       UINT32_C(0x810000c8));
	assert(airoha_ctc_management_apply(&manager, 5, &identical) == 0);
	assert(identical.uni[2].vlan_applied && backend.calls == 1);
	assert(airoha_ctc_management_apply(&manager, 6, &conflict) ==
	       -EADDRINUSE);
	assert(!conflict.uni[2].vlan_applied && backend.calls == 1);
	assert(!manager.llid[6].uni[2].vlan_present);
	assert(airoha_ctc_management_clear_llid(&manager, 4) == 0);
	assert(backend.calls == 1);
	assert(airoha_ctc_management_clear_llid(&manager, 5) == 0);
	assert(backend.calls == 2 &&
	       airoha_ctc_management_set_empty(&manager.effective));
}

static void test_classification_transactions(void)
{
	static struct airoha_ctc_management_manager manager;
	static struct fake_backend backend;
	static struct airoha_ctc_management_set first, second, identical, conflict;
	static struct airoha_ctc_management_set wrong_delete, reverse_delete;
	static struct airoha_ctc_management_set correct_delete, clear, failed;
	static struct airoha_ctc_uni_policy overflow, snapshot;
	struct airoha_ctc_classification_rule *rule;
	struct airoha_ctc_uni_policy *policy;
	unsigned int calls;

	memset(&manager, 0, sizeof(manager));
	memset(&backend, 0, sizeof(backend));
	manager.apply = fake_apply;
	manager.apply_context = &backend;

	policy = classification_delta(&first, 0, AIROHA_CTC_CLASSIFICATION_ADD);
	policy->classification_rule_count = 1;
	rule = &policy->classification_rule[0];
	rule->precedence = 10;
	rule->queue = 7;
	rule->priority = 5;
	rule->match_count = 2;
	rule->match[0] = classification_u8_match(AIROHA_CTC_CLASSIFICATION_PBIT, 3);
	rule->match[1] = rule->match[0];
	assert(airoha_ctc_management_apply(&manager, 0, &first) == 0);
	assert(first.uni[0].classification_applied && backend.calls == 1);

	policy = classification_delta(&second, 0, AIROHA_CTC_CLASSIFICATION_ADD);
	policy->classification_rule_count = 1;
	rule = &policy->classification_rule[0];
	rule->precedence = 10;
	rule->queue = 1;
	rule->priority = AIROHA_CTC_CLASSIFICATION_PRIORITY_UNCHANGED;
	rule->match_count = 2;
	rule->match[0] = classification_u16_match(
		AIROHA_CTC_CLASSIFICATION_VLAN_ID, 100);
	rule->match[1] = classification_u16_match(
		AIROHA_CTC_CLASSIFICATION_ETHERTYPE, 0x0800);
	assert(airoha_ctc_management_apply(&manager, 0, &second) == 0);
	assert(second.uni[0].classification_applied && backend.calls == 2);
	policy = &manager.effective.uni[0];
	assert(policy->classification_present &&
	       policy->classification_rule_count == 2);
	assert(policy->classification_rule[0].precedence == 10 &&
	       policy->classification_rule[0].queue == 1);
	assert(policy->classification_rule[1].precedence == 11 &&
	       policy->classification_rule[1].queue == 7);

	identical = manager.llid[0];
	conflict = identical;
	conflict.uni[0].classification_rule[1].queue = 6;
	assert(airoha_ctc_management_apply(&manager, 1, &identical) == 0);
	assert(identical.uni[0].classification_applied && backend.calls == 2);
	assert(airoha_ctc_management_apply(&manager, 2, &conflict) ==
	       -EADDRINUSE);
	assert(!conflict.uni[0].classification_applied && backend.calls == 2);
	assert(!manager.llid[2].uni[0].classification_present);
	assert(airoha_ctc_management_clear_llid(&manager, 1) == 0);
	assert(backend.calls == 2);

	policy = classification_delta(&wrong_delete, 0,
		AIROHA_CTC_CLASSIFICATION_DELETE);
	policy->classification_rule_count = 1;
	policy->classification_rule[0] =
		manager.llid[0].uni[0].classification_rule[1];
	policy->classification_rule[0].match[1] = classification_u16_match(
		AIROHA_CTC_CLASSIFICATION_VLAN_ID, 100);
	calls = backend.calls;
	assert(airoha_ctc_management_apply(&manager, 0, &wrong_delete) == 0);
	assert(wrong_delete.uni[0].classification_applied &&
	       backend.calls == calls);
	assert(manager.llid[0].uni[0].classification_rule_count == 2);

	policy = classification_delta(&reverse_delete, 0,
		AIROHA_CTC_CLASSIFICATION_DELETE);
	policy->classification_rule_count = 1;
	policy->classification_rule[0] =
		manager.llid[0].uni[0].classification_rule[0];
	rule = &policy->classification_rule[0];
	{
		struct airoha_ctc_classification_match swap = rule->match[0];

		rule->match[0] = rule->match[1];
		rule->match[1] = swap;
	}
	assert(airoha_ctc_management_apply(&manager, 0, &reverse_delete) == 0);
	assert(reverse_delete.uni[0].classification_applied &&
	       manager.llid[0].uni[0].classification_rule_count == 1);
	assert(manager.llid[0].uni[0].classification_rule[0].queue == 7);

	policy = classification_delta(&correct_delete, 0,
		AIROHA_CTC_CLASSIFICATION_DELETE);
	policy->classification_rule_count = 1;
	policy->classification_rule[0] =
		manager.llid[0].uni[0].classification_rule[0];
	assert(airoha_ctc_management_apply(&manager, 0, &correct_delete) == 0);
	assert(correct_delete.uni[0].classification_applied);
	assert(manager.effective.uni[0].classification_present &&
	       manager.effective.uni[0].classification_rule_count == 0);
	assert(backend.last.uni[0].classification_present &&
	       backend.last.uni[0].classification_rule_count == 0);

	classification_delta(&clear, 0, AIROHA_CTC_CLASSIFICATION_CLEAR);
	assert(airoha_ctc_management_apply(&manager, 0, &clear) == 0);
	assert(clear.uni[0].classification_applied &&
	       !manager.llid[0].uni[0].classification_present &&
	       airoha_ctc_management_set_empty(&manager.effective));

	failed = first;
	failed.uni[0].classification_applied = false;
	backend.result = -EIO;
	calls = backend.calls;
	assert(airoha_ctc_management_apply(&manager, 0, &failed) == -EIO);
	assert(backend.calls == calls + 1 &&
	       !failed.uni[0].classification_applied &&
	       !manager.llid[0].uni[0].classification_present &&
	       airoha_ctc_management_set_empty(&manager.effective));
	backend.result = 0;

	memset(&overflow, 0, sizeof(overflow));
	overflow.classification_present = true;
	overflow.classification_valid = true;
	overflow.classification_rule_count = 2;
	overflow.classification_rule[0].precedence = 254;
	overflow.classification_rule[1].precedence = 255;
	rule = &failed.uni[0].classification_rule[0];
	rule->precedence = 254;
	snapshot = overflow;
	assert(airoha_ctc_management_classification_insert(&overflow, rule) ==
	       -ERANGE);
	assert(!memcmp(&overflow, &snapshot, sizeof(overflow)));
	overflow.classification_rule_count = AIROHA_CTC_CLASSIFICATION_RULE_MAX;
	assert(airoha_ctc_management_classification_insert(&overflow, rule) ==
	       -ENOSPC);
}

int main(void)
{
	test_identical_merge_and_clear();
	test_atomic_backend_failure();
	test_global_aging_conflict();
	test_phy_autoneg_and_transient_restart();
	test_vlan_multi_llid_conflict();
	test_classification_transactions();
	return 0;
}
