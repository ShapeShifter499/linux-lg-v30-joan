/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2019 Linaro Limited
 * Copyright (c) 2021, AngeloGioacchino Del Regno
 *                     <angelogioacchino.delregno@somainline.org>
 */

#ifndef __CPR_H__
#define __CPR_H__

struct cpr_ext_data {
	int mem_acc_threshold_uV;
	int apm_threshold_uV;
};

struct device;

#if IS_ENABLED(CONFIG_QCOM_CPR3)
int cpr3_cprh_setup_corners(struct device *dev);
#else
static inline int cpr3_cprh_setup_corners(struct device *dev)
{
	return -ENODEV;
}
#endif

#endif /* __CPR_H__ */
