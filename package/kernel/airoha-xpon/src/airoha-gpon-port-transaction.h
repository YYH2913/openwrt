/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_GPON_PORT_TRANSACTION_H
#define __AIROHA_GPON_PORT_TRANSACTION_H

enum airoha_gpon_port_transaction_result {
	AIROHA_GPON_PORT_TRANSACTION_REJECTED = 1,
	AIROHA_GPON_PORT_TRANSACTION_ROLLBACK_FAILED,
};

struct airoha_gpon_port_snapshot {
	u16 active_port;
	bool active_valid;
	bool active_encrypted;
	bool target_valid;
	bool target_encrypted;
};

struct airoha_gpon_port_transaction_ops {
	int (*set_gem)(void *context, u16 port, bool valid, bool encrypted);
	int (*set_omcc)(void *context, u16 port, bool valid);
	void (*commit_omcc)(void *context, u16 port, bool valid);
	int (*acknowledge)(void *context);
	void (*fail_closed)(void *context);
};

struct airoha_gpon_session_reset_ops {
	int (*disable_tx)(void *context);
	int (*clear_data)(void *context);
	int (*disable_omcc)(void *context);
	int (*clear_gem_table)(void *context);
	void (*commit_reset)(void *context, unsigned int state);
};

static inline int
airoha_gpon_session_reset_transaction(
		const struct airoha_gpon_session_reset_ops *ops, void *context,
		unsigned int requested_state, unsigned int failure_state)
{
	int ret = 0;
	int step_ret;

	step_ret = ops->disable_tx(context);
	if (step_ret && !ret)
		ret = step_ret;
	step_ret = ops->clear_data(context);
	if (step_ret && !ret)
		ret = step_ret;
	step_ret = ops->disable_omcc(context);
	if (step_ret && !ret)
		ret = step_ret;
	step_ret = ops->clear_gem_table(context);
	if (step_ret && !ret)
		ret = step_ret;

	ops->commit_reset(context, ret ? failure_state : requested_state);
	return ret;
}

static inline int
airoha_gpon_port_transaction_rollback_failed(
		const struct airoha_gpon_port_transaction_ops *ops, void *context)
{
	if (ops->fail_closed)
		ops->fail_closed(context);

	return AIROHA_GPON_PORT_TRANSACTION_ROLLBACK_FAILED;
}

static inline int
airoha_gpon_configure_port_transaction(
	const struct airoha_gpon_port_transaction_ops *ops, void *context,
	u16 port, bool enable, const struct airoha_gpon_port_snapshot *snapshot)
{
	bool rollback_failed;
	int ret;

	if (enable) {
		ret = ops->set_gem(context, port, true, false);
		if (ret) {
			if (ops->set_gem(context, port, snapshot->target_valid,
					 snapshot->target_encrypted))
				return airoha_gpon_port_transaction_rollback_failed(
					ops, context);
			return ret;
		}

		ret = ops->set_omcc(context, port, true);
		if (ret)
			goto restore_target;

		if (snapshot->active_valid && snapshot->active_port != port) {
			ret = ops->set_gem(context, snapshot->active_port,
						   false, false);
			if (ret) {
				rollback_failed =
					ops->set_gem(context, snapshot->active_port,
						     true,
						     snapshot->active_encrypted);
				rollback_failed |=
					ops->set_omcc(context, snapshot->active_port,
						      true) != 0;
				rollback_failed |=
					ops->set_gem(context, port,
						     snapshot->target_valid,
						     snapshot->target_encrypted) != 0;
				if (rollback_failed)
					return airoha_gpon_port_transaction_rollback_failed(
						ops, context);
				return ret;
			}
		}

		ops->commit_omcc(context, port, true);
	} else {
		/* A repeated disable after a lost ACK is already complete. */
		if (!snapshot->active_valid)
			return ops->acknowledge(context);
		if (snapshot->active_port != port)
			return AIROHA_GPON_PORT_TRANSACTION_REJECTED;

		ret = ops->set_gem(context, port, false, false);
		if (ret) {
			if (ops->set_gem(context, port, true,
					 snapshot->target_encrypted))
				return airoha_gpon_port_transaction_rollback_failed(
					ops, context);
			return ret;
		}

		ret = ops->set_omcc(context, port, false);
		if (ret) {
			rollback_failed =
				ops->set_gem(context, port, true,
					     snapshot->target_encrypted);
			rollback_failed |=
				ops->set_omcc(context, port, true) != 0;
			if (rollback_failed)
				return airoha_gpon_port_transaction_rollback_failed(
					ops, context);
			return ret;
		}

		ops->commit_omcc(context, 0, false);
	}

	return ops->acknowledge(context);

restore_target:
	if (ops->set_gem(context, port, snapshot->target_valid,
			 snapshot->target_encrypted))
		return airoha_gpon_port_transaction_rollback_failed(ops,
								 context);
	return ret;
}

static inline int
airoha_gpon_encrypted_port_transaction(
		const struct airoha_gpon_port_transaction_ops *ops, void *context,
		u16 port, bool port_valid, bool previous_encrypted, bool encrypted)
{
	int ret;

	if (!port_valid)
		return AIROHA_GPON_PORT_TRANSACTION_REJECTED;

	ret = ops->set_gem(context, port, true, encrypted);
	if (ret) {
		if (ops->set_gem(context, port, true, previous_encrypted))
			return airoha_gpon_port_transaction_rollback_failed(ops,
								 context);
		return ret;
	}

	return ops->acknowledge(context);
}

#endif
