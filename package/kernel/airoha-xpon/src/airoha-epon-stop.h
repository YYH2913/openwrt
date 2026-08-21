/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_EPON_STOP_H
#define __AIROHA_EPON_STOP_H

struct airoha_epon_stop_ops {
	int (*clear_keys)(void *context);
	int (*clear_report_thresholds)(void *context);
	int (*clear_qdma)(void *context);
};

static inline int
airoha_epon_stop_cleanup(const struct airoha_epon_stop_ops *ops,
			 void *context)
{
	int first_error = 0;
	int ret;

	if (!ops || !ops->clear_keys || !ops->clear_report_thresholds ||
	    !ops->clear_qdma)
		return -22;

	ret = ops->clear_keys(context);
	if (ret)
		first_error = ret;
	ret = ops->clear_report_thresholds(context);
	if (ret && !first_error)
		first_error = ret;
	ret = ops->clear_qdma(context);
	if (ret && !first_error)
		first_error = ret;

	return first_error;
}

#endif
