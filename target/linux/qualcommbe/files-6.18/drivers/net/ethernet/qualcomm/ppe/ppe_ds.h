/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __QCOM_PPE_DS_H
#define __QCOM_PPE_DS_H

struct ppe_device;

int ppe_ds_setup(struct ppe_device *ppe_dev);
void ppe_ds_teardown(struct ppe_device *ppe_dev);

#endif
