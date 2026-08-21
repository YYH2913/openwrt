/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_EPON_CTC_TRANSACTION_H_
#define _AIROHA_EPON_CTC_TRANSACTION_H_

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "airoha-epon-ctc.h"

struct airoha_ctc_set_transaction_ops {
	int (*get_fec)(void *context, unsigned int llid, bool *rx_enabled,
		bool *tx_enabled);
	int (*set_fec)(void *context, unsigned int llid, bool rx_enabled,
		bool tx_enabled);
	int (*get_holdover)(void *context, bool *enabled, uint32_t *time_ms);
	int (*set_holdover)(void *context, bool enabled, uint32_t time_ms);
	int (*apply_management)(void *context, unsigned int llid,
		struct airoha_ctc_management_set *management);
	void *context;
};

struct airoha_ctc_set_transaction_request {
	unsigned int llid;
	bool fec_present;
	bool fec_enabled;
	bool holdover_present;
	bool holdover_enabled;
	uint32_t holdover_time_ms;
	bool management_present;
	struct airoha_ctc_management_set *management;
};

struct airoha_ctc_set_transaction_result {
	bool fec_applied;
	bool holdover_applied;
	bool management_applied;
	int failure;
	int rollback_failure;
};

static inline bool airoha_ctc_set_transaction_fatal(
		const struct airoha_ctc_set_transaction_result *result)
{
	return result && result->rollback_failure;
}

static inline int airoha_ctc_execute_set_transaction(
		const struct airoha_ctc_set_transaction_ops *ops,
		const struct airoha_ctc_set_transaction_request *request,
		struct airoha_ctc_set_transaction_result *result)
{
	bool previous_fec_rx = false, previous_fec_tx = false;
	bool previous_holdover_enabled = false;
	uint32_t previous_holdover_time_ms = 0;
	bool fec_changed = false, holdover_changed = false;
	int failure = 0, rollback = 0, current;

	if (!ops || !request || !result ||
	    (request->fec_present && (!ops->get_fec || !ops->set_fec)) ||
	    (request->holdover_present &&
	     (!ops->get_holdover || !ops->set_holdover)) ||
	    (request->management_present &&
	     (!ops->apply_management || !request->management)))
		return -EINVAL;
	memset(result, 0, sizeof(*result));

	if (request->fec_present)
		failure = ops->get_fec(ops->context, request->llid,
			&previous_fec_rx, &previous_fec_tx);
	if (!failure && request->holdover_present)
		failure = ops->get_holdover(ops->context,
			&previous_holdover_enabled, &previous_holdover_time_ms);
	if (!failure && request->fec_present) {
		failure = ops->set_fec(ops->context, request->llid,
			request->fec_enabled, request->fec_enabled);
		fec_changed = !failure;
	}
	if (!failure && request->holdover_present) {
		failure = ops->set_holdover(ops->context,
			request->holdover_enabled, request->holdover_time_ms);
		holdover_changed = !failure;
	}
	if (!failure && request->management_present)
		failure = ops->apply_management(ops->context, request->llid,
			request->management);

	if (failure) {
		/* The platform helper uses EUCLEAN only after its own rollback
		 * failed, so the daemon must stop even if kernel rollback succeeds. */
		if (failure == -EUCLEAN)
			rollback = failure;
		if (holdover_changed) {
			current = ops->set_holdover(ops->context,
				previous_holdover_enabled,
				previous_holdover_time_ms);
			if (current && !rollback)
				rollback = current;
		}
		if (fec_changed) {
			current = ops->set_fec(ops->context, request->llid,
				previous_fec_rx, previous_fec_tx);
			if (current && !rollback)
				rollback = current;
		}
		result->failure = failure;
		result->rollback_failure = rollback;
		return failure;
	}

	result->fec_applied = request->fec_present;
	result->holdover_applied = request->holdover_present;
	result->management_applied = request->management_present;
	return 0;
}

#endif
