/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * MSM8998 DisplayPort PHY block offsets, from LG's downstream
 * drivers/clk/msm/mdss/mdss-dp-pll-8998.h. This is not the QCS615/V3
 * DP PHY map: PD_CTL is at 0x14 here and at 0x18 there, and STATUS is
 * at 0xbc here and at 0xc0 there.
 */

#ifndef QCOM_PHY_QMP_DP_PHY_V3_8998_H_
#define QCOM_PHY_QMP_DP_PHY_V3_8998_H_

#define QSERDES_V3_DP_PHY_REVISION_ID0			0x000
#define QSERDES_V3_DP_PHY_REVISION_ID1			0x004
#define QSERDES_V3_DP_PHY_REVISION_ID2			0x008
#define QSERDES_V3_DP_PHY_REVISION_ID3			0x00c
#define QSERDES_V3_DP_PHY_CFG				0x010
#define QSERDES_V3_DP_PHY_PD_CTL			0x014
#define QSERDES_V3_DP_PHY_MODE				0x018
#define QSERDES_V3_DP_PHY_AUX_CFG0			0x01c
#define QSERDES_V3_DP_PHY_AUX_CFG1			0x020
#define QSERDES_V3_DP_PHY_AUX_CFG2			0x024
#define QSERDES_V3_DP_PHY_AUX_CFG3			0x028
#define QSERDES_V3_DP_PHY_AUX_CFG4			0x02c
#define QSERDES_V3_DP_PHY_AUX_CFG5			0x030
#define QSERDES_V3_DP_PHY_AUX_CFG6			0x034
#define QSERDES_V3_DP_PHY_AUX_CFG7			0x038
#define QSERDES_V3_DP_PHY_AUX_CFG8			0x03c
#define QSERDES_V3_DP_PHY_AUX_CFG9			0x040
#define QSERDES_V3_DP_PHY_AUX_INTERRUPT_MASK		0x044
#define QSERDES_V3_DP_PHY_AUX_INTERRUPT_CLEAR		0x048
#define QSERDES_V3_DP_PHY_AUX_BIST_CFG			0x04c
#define QSERDES_V3_DP_PHY_VCO_DIV			0x064
#define QSERDES_V3_DP_PHY_TX0_TX1_LANE_CTL		0x068
#define QSERDES_V3_DP_PHY_TX2_TX3_LANE_CTL		0x084
#define QSERDES_V3_DP_PHY_SPARE0			0x0a8
#define QSERDES_V3_DP_PHY_STATUS			0x0bc

#endif
