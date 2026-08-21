/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_EPON_DYING_GASP_H
#define __AIROHA_EPON_DYING_GASP_H

enum airoha_epon_dying_gasp_result {
	AIROHA_EPON_DYING_GASP_VERIFY_FAILED = -5,
	AIROHA_EPON_DYING_GASP_ROLLBACK_FAILED = -117,
};

struct airoha_epon_dying_gasp_ops {
	int (*update)(void *context, bool enabled);
	int (*read)(void *context, bool *enabled);
	void (*fail_closed)(void *context);
};

static inline int airoha_epon_dying_gasp_restore_disabled(
	const struct airoha_epon_dying_gasp_ops *ops, void *context)
{
	bool enabled = true;
	int ret;

	ops->update(context, false);
	ret = ops->read(context, &enabled);
	return ret || enabled ? AIROHA_EPON_DYING_GASP_ROLLBACK_FAILED : 0;
}

static inline int airoha_epon_dying_gasp_fail(
	const struct airoha_epon_dying_gasp_ops *ops, void *context, int cause)
{
	int ret;

	ret = airoha_epon_dying_gasp_restore_disabled(ops, context);
	if (ops->fail_closed)
		ops->fail_closed(context);
	return ret ? ret : cause;
}

static inline int airoha_epon_dying_gasp_set(
	const struct airoha_epon_dying_gasp_ops *ops, void *context,
	bool enabled)
{
	bool readback;
	int ret;

	if (!ops || !ops->update || !ops->read)
		return -22;

	ret = ops->update(context, enabled);
	if (ret)
		return airoha_epon_dying_gasp_fail(ops, context, ret);
	ret = ops->read(context, &readback);
	if (ret)
		return airoha_epon_dying_gasp_fail(ops, context, ret);
	if (readback != enabled)
		return airoha_epon_dying_gasp_fail(
			ops, context, AIROHA_EPON_DYING_GASP_VERIFY_FAILED);

	return 0;
}

#endif
