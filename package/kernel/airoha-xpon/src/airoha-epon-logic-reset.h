/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_EPON_LOGIC_RESET_H
#define __AIROHA_EPON_LOGIC_RESET_H

enum airoha_epon_logic_reset_result {
	AIROHA_EPON_LOGIC_RESET_VERIFY_FAILED = -5,
	AIROHA_EPON_LOGIC_RESET_ROLLBACK_FAILED = -117,
};

struct airoha_epon_logic_reset_ops {
	int (*update)(void *context, bool hold);
	int (*read)(void *context, bool *hold);
	void (*delay)(void *context);
	void (*fail_closed)(void *context);
};

static inline int airoha_epon_logic_reset_restore_hold(
	const struct airoha_epon_logic_reset_ops *ops, void *context)
{
	bool hold = false;
	int read_ret;

	ops->update(context, true);
	read_ret = ops->read(context, &hold);
	if (read_ret || !hold)
		return AIROHA_EPON_LOGIC_RESET_ROLLBACK_FAILED;

	/* A readback proving reset is held resolves an ambiguous update error. */
	return 0;
}

static inline int airoha_epon_logic_reset_fail(
	const struct airoha_epon_logic_reset_ops *ops, void *context, int cause)
{
	int ret;

	ret = airoha_epon_logic_reset_restore_hold(ops, context);
	if (ops->fail_closed)
		ops->fail_closed(context);
	return ret ? ret : cause;
}

static inline int airoha_epon_logic_reset_set(
	const struct airoha_epon_logic_reset_ops *ops, void *context, bool hold)
{
	bool readback;
	int ret;

	if (!ops || !ops->update || !ops->read || !ops->delay)
		return -22;

	ret = ops->update(context, hold);
	if (ret)
		return airoha_epon_logic_reset_fail(ops, context, ret);
	ret = ops->read(context, &readback);
	if (ret)
		return airoha_epon_logic_reset_fail(ops, context, ret);
	if (readback != hold)
		return airoha_epon_logic_reset_fail(
			ops, context, AIROHA_EPON_LOGIC_RESET_VERIFY_FAILED);
	ops->delay(context);

	return 0;
}

static inline int airoha_epon_logic_reset_pulse(
	const struct airoha_epon_logic_reset_ops *ops, void *context)
{
	int ret;

	ret = airoha_epon_logic_reset_set(ops, context, true);
	return ret ? ret :
		airoha_epon_logic_reset_set(ops, context, false);
}

#endif
