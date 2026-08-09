/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * Qualcomm MSM8998 interconnect IDs
 *
 * Generated from the downstream msm8998-bus.dtsi topology
 * (LineageOS android_kernel_lge_msm8998 lineage-22.2).
 */

#ifndef __DT_BINDINGS_INTERCONNECT_QCOM_MSM8998_H
#define __DT_BINDINGS_INTERCONNECT_QCOM_MSM8998_H

/* BIMC */
#define MAS_GNOC_BIMC			0
#define MAS_OXILI			1
#define MAS_MNOC_BIMC			2
#define MAS_SNOC_BIMC			3
#define SLV_EBI			4
#define SLV_HMSS_L3			5
#define SLV_BIMC_SNOC_0			6
#define SLV_BIMC_SNOC_1			7

/* CNOC */
#define MAS_SNOC_CNOC			0
#define MAS_QDSS_DAP			1
#define SLV_CNOC_A2NOC			2
#define SLV_SSC_CFG			3
#define SLV_MPM			4
#define SLV_PMIC_ARB			5
#define SLV_TLMM_NORTH			6
#define SLV_PIMEM_CFG			7
#define SLV_IMEM_CFG			8
#define SLV_MESSAGE_RAM			9
#define SLV_SKL			10
#define SLV_BIMC_CFG			11
#define SLV_PRNG			12
#define SLV_A2NOC_CFG			13
#define SLV_IPA			14
#define SLV_TCSR			15
#define SLV_SNOC_CFG			16
#define SLV_CLK_CTL			17
#define SLV_GLM			18
#define SLV_SPDM			19
#define SLV_GPUSS_CFG			20
#define SLV_CNOC_MNOC_CFG			21
#define SLV_QM_CFG			22
#define SLV_MSS_CFG			23
#define SLV_UFS_CFG			24
#define SLV_TLMM_WEST			25
#define SLV_A1NOC_CFG			26
#define SLV_AHB2PHY			27
#define SLV_BLSP_2			28
#define SLV_PDM			29
#define SLV_USB3_0			30
#define SLV_A1NOC_SMMU_CFG			31
#define SLV_BLSP_1			32
#define SLV_SDCC_2			33
#define SLV_SDCC_4			34
#define SLV_TSIF			35
#define SLV_QDSS_CFG			36
#define SLV_TLMM_EAST			37
#define SLV_CNOC_MNOC_MMSS_CFG			38
#define SLV_SRVC_CNOC			39

/* SNOC */
#define MAS_HMSS			0
#define MAS_QDSS_BAM			1
#define MAS_SNOC_CFG			2
#define MAS_BIMC_SNOC_0			3
#define MAS_BIMC_SNOC_1			4
#define MAS_A1NOC_SNOC			5
#define MAS_A2NOC_SNOC			6
#define MAS_QDSS_ETR			7
#define SLV_HMSS			8
#define SLV_LPASS			9
#define SLV_WLAN			10
#define SLV_SNOC_BIMC			11
#define SLV_SNOC_CNOC			12
#define SLV_IMEM			13
#define SLV_PIMEM			14
#define SLV_QDSS_STM			15
#define SLV_PCIE_0			16
#define SLV_SRVC_SNOC			17

/* MNOC */
#define MAS_CNOC_MNOC_CFG			0
#define MAS_CPP			1
#define MAS_JPEG			2
#define MAS_MDP_P0			3
#define MAS_MDP_P1			4
#define MAS_ROTATOR			5
#define MAS_VENUS			6
#define MAS_VFE			7
#define MAS_VENUS_VMEM			8
#define SLV_MNOC_BIMC			9
#define SLV_VMEM			10
#define SLV_SRVC_MNOC			11


/* A1NOC */
#define MAS_PCIE_0                  	0
#define MAS_UFS                     	1
#define MAS_USB3                    	2
#define MAS_BLSP_2                  	3
#define SLV_A1NOC_SNOC              	4

/* gnoc */
#define MAS_APSS_PROC			0
#define SLV_GNOC_BIMC			1


/* A2NOC */
#define MAS_IPA                     	0
#define MAS_CNOC_A2NOC              	1
#define MAS_SDCC_2                  	2
#define MAS_SDCC_4                  	3
#define MAS_BLSP_1                  	4
#define MAS_TSIF                    	5
#define MAS_CRYPTO_C0               	6
#define MAS_CR_VIRT_A2NOC           	7
#define SLV_CR_VIRT_A2NOC           	8
#define SLV_A2NOC_SNOC              	9

#endif
