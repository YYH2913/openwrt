/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XGS_EQD_H_
#define _AIROHA_XGS_EQD_H_

#include "airoha-xpon-mode.h"

#define AIROHA_XGS_EQD_EIO	(-5)
#define AIROHA_XGS_EQD_EINVAL	(-22)
#define AIROHA_XGS_EQD_ERANGE	(-34)

struct airoha_xgs_eqd_ops {
	int (*read)(void *context, unsigned int *value);
	int (*write)(void *context, unsigned int value);
	void (*fail_closed)(void *context);
};

static inline int
airoha_xgs_eqd_resolve(unsigned int current_eqd, unsigned int requested,
		       bool absolute, bool negative, unsigned int *resolved)
{
	if (!resolved)
		return AIROHA_XGS_EQD_EINVAL;
	if (absolute) {
		*resolved = requested;
		return 0;
	}
	if (negative) {
		if (requested > current_eqd)
			return AIROHA_XGS_EQD_ERANGE;
		*resolved = current_eqd - requested;
		return 0;
	}
	if (requested > ~0U - current_eqd)
		return AIROHA_XGS_EQD_ERANGE;
	*resolved = current_eqd + requested;
	return 0;
}

static inline int
airoha_xgs_eqd_encode(enum airoha_xpon_mode mode, unsigned int resolved,
		      unsigned int *encoded)
{
	if (!encoded)
		return AIROHA_XGS_EQD_EINVAL;
	if (mode == AIROHA_XPON_MODE_XGPON) {
		*encoded = resolved;
		return 0;
	}
	if (mode != AIROHA_XPON_MODE_XGSPON)
		return AIROHA_XGS_EQD_EINVAL;
	if (resolved > (~0U >> 2))
		return AIROHA_XGS_EQD_ERANGE;
	*encoded = resolved << 2;
	return 0;
}

/* Commit EqD only after readback. A failed rollback invokes fail_closed. */
static inline int
airoha_xgs_eqd_transaction(const struct airoha_xgs_eqd_ops *ops,
			   void *context, enum airoha_xpon_mode mode,
			   unsigned int current_eqd, unsigned int requested,
			   bool absolute, bool negative,
			   unsigned int *resolved)
{
	unsigned int old_value, readback, new_value, new_resolved;
	int ret, rollback_ret;

	if (!ops || !ops->read || !ops->write || !resolved)
		return AIROHA_XGS_EQD_EINVAL;
	ret = airoha_xgs_eqd_resolve(current_eqd, requested, absolute, negative,
				     &new_resolved);
	if (ret)
		return ret;
	ret = airoha_xgs_eqd_encode(mode, new_resolved, &new_value);
	if (ret)
		return ret;
	ret = ops->read(context, &old_value);
	if (ret)
		return ret;
	ret = ops->write(context, new_value);
	if (!ret)
		ret = ops->read(context, &readback);
	if (!ret && readback == new_value) {
		*resolved = new_resolved;
		return 0;
	}
	if (!ret)
		ret = AIROHA_XGS_EQD_EIO;

	rollback_ret = ops->write(context, old_value);
	if (!rollback_ret)
		rollback_ret = ops->read(context, &readback);
	if (!rollback_ret && readback != old_value)
		rollback_ret = AIROHA_XGS_EQD_EIO;
	if (rollback_ret && ops->fail_closed)
		ops->fail_closed(context);
	return ret;
}

#endif
