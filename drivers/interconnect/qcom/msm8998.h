/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Qualcomm MSM8998 interconnect IDs (driver-internal, globally unique)
 *
 * These are the icc core node IDs. The DT bindings header
 * (dt-bindings/interconnect/qcom,msm8998.h) carries per-fabric
 * 0-based IDs for consumers; the driver maps fabric-local to
 * global via this header, like msm8996.
 */

#ifndef __DRIVERS_INTERCONNECT_QCOM_MSM8998_H__
#define __DRIVERS_INTERCONNECT_QCOM_MSM8998_H__

/* BIMC */
#define MSM8998_MAS_GNOC_BIMC				1
#define MSM8998_MAS_OXILI				2
#define MSM8998_MAS_MNOC_BIMC				3
#define MSM8998_MAS_SNOC_BIMC				4
#define MSM8998_SLV_EBI				5
#define MSM8998_SLV_HMSS_L3				6
#define MSM8998_SLV_BIMC_SNOC_0				7
#define MSM8998_SLV_BIMC_SNOC_1				8

/* CNOC */
#define MSM8998_MAS_SNOC_CNOC				9
#define MSM8998_MAS_QDSS_DAP				10
#define MSM8998_SLV_CNOC_A2NOC				11
#define MSM8998_SLV_SSC_CFG				12
#define MSM8998_SLV_MPM				13
#define MSM8998_SLV_PMIC_ARB				14
#define MSM8998_SLV_TLMM_NORTH				15
#define MSM8998_SLV_PIMEM_CFG				16
#define MSM8998_SLV_IMEM_CFG				17
#define MSM8998_SLV_MESSAGE_RAM				18
#define MSM8998_SLV_SKL				19
#define MSM8998_SLV_BIMC_CFG				20
#define MSM8998_SLV_PRNG				21
#define MSM8998_SLV_A2NOC_CFG				22
#define MSM8998_SLV_IPA				23
#define MSM8998_SLV_TCSR				24
#define MSM8998_SLV_SNOC_CFG				25
#define MSM8998_SLV_CLK_CTL				26
#define MSM8998_SLV_GLM				27
#define MSM8998_SLV_SPDM				28
#define MSM8998_SLV_GPUSS_CFG				29
#define MSM8998_SLV_CNOC_MNOC_CFG				30
#define MSM8998_SLV_QM_CFG				31
#define MSM8998_SLV_MSS_CFG				32
#define MSM8998_SLV_UFS_CFG				33
#define MSM8998_SLV_TLMM_WEST				34
#define MSM8998_SLV_A1NOC_CFG				35
#define MSM8998_SLV_AHB2PHY				36
#define MSM8998_SLV_BLSP_2				37
#define MSM8998_SLV_PDM				38
#define MSM8998_SLV_USB3_0				39
#define MSM8998_SLV_A1NOC_SMMU_CFG				40
#define MSM8998_SLV_BLSP_1				41
#define MSM8998_SLV_SDCC_2				42
#define MSM8998_SLV_SDCC_4				43
#define MSM8998_SLV_TSIF				44
#define MSM8998_SLV_QDSS_CFG				45
#define MSM8998_SLV_TLMM_EAST				46
#define MSM8998_SLV_CNOC_MNOC_MMSS_CFG				47
#define MSM8998_SLV_SRVC_CNOC				48

/* SNOC */
#define MSM8998_MAS_HMSS				49
#define MSM8998_MAS_QDSS_BAM				50
#define MSM8998_MAS_SNOC_CFG				51
#define MSM8998_MAS_BIMC_SNOC_0				52
#define MSM8998_MAS_BIMC_SNOC_1				53
#define MSM8998_MAS_A1NOC_SNOC				54
#define MSM8998_MAS_A2NOC_SNOC				55
#define MSM8998_MAS_QDSS_ETR				56
#define MSM8998_SLV_HMSS				57
#define MSM8998_SLV_LPASS				58
#define MSM8998_SLV_WLAN				59
#define MSM8998_SLV_SNOC_BIMC				60
#define MSM8998_SLV_SNOC_CNOC				61
#define MSM8998_SLV_IMEM				62
#define MSM8998_SLV_PIMEM				63
#define MSM8998_SLV_QDSS_STM				64
#define MSM8998_SLV_PCIE_0				65
#define MSM8998_SLV_SRVC_SNOC				66

/* MNOC */
#define MSM8998_MAS_CNOC_MNOC_CFG				67
#define MSM8998_MAS_CPP				68
#define MSM8998_MAS_JPEG				69
#define MSM8998_MAS_MDP_P0				70
#define MSM8998_MAS_MDP_P1				71
#define MSM8998_MAS_ROTATOR				72
#define MSM8998_MAS_VENUS				73
#define MSM8998_MAS_VFE				74
#define MSM8998_MAS_VENUS_VMEM				75
#define MSM8998_SLV_MNOC_BIMC				76
#define MSM8998_SLV_VMEM				77
#define MSM8998_SLV_SRVC_MNOC				78

#define MSM8998_MAS_PCIE_0                          	79
#define MSM8998_MAS_UFS                             	80
#define MSM8998_MAS_USB3                            	81
#define MSM8998_MAS_BLSP_2                          	82
#define MSM8998_SLV_A1NOC_SNOC                      	83
#define MSM8998_MAS_IPA                             	84
#define MSM8998_MAS_CNOC_A2NOC                      	85
#define MSM8998_MAS_SDCC_2                          	86
#define MSM8998_MAS_SDCC_4                          	87
#define MSM8998_MAS_BLSP_1                          	88
#define MSM8998_MAS_TSIF                            	89
#define MSM8998_MAS_CRYPTO_C0                       	90
#define MSM8998_MAS_CR_VIRT_A2NOC                   	91
#define MSM8998_SLV_CR_VIRT_A2NOC                   	92
#define MSM8998_SLV_A2NOC_SNOC                      	93

#endif
