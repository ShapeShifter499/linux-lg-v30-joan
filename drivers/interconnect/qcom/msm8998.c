// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm MSM8998 Network-on-Chip (NoC) interconnect driver
 *
 * Node topology and the per-node ap_owned/buswidth/channels values are
 * generated from the downstream msm8998-bus.dtsi; RPM clock resources
 * from the icc-rpm family. Node IDs are the globally-unique driver IDs
 * from msm8998.h (like msm8996).
 *
 * DO NOT set qos_mode to anything but NOC_QOS_MODE_INVALID here.
 *
 * These descs carry no .regmap_cfg, so qp->regmap stays NULL, and the
 * ap_owned && qos_mode != INVALID gate in qcom_icc_probe() is the only
 * thing stopping qcom_icc_set_noc_qos() from dereferencing it. Opening
 * that gate hangs the SoC at probe, before USB comes up - measured on
 * device 2026-08-07, an unrecoverable hang needing a forced power-off.
 *
 * QoS bring-up therefore needs a regmap_cfg and qos_offset per provider
 * first, the way msm8996.c does it. The ap_owned flags and the
 * downstream qos_port/qos_mode/prio values needed for that are recorded
 * in the commit that ported them, not carried here.
 */

#include <linux/device.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/interconnect/qcom,msm8998.h>

#include "icc-rpm.h"
#include "msm8998.h"

static const u16 mas_gnoc_bimc_links[] = {
	MSM8998_SLV_EBI,
	MSM8998_SLV_BIMC_SNOC_0,
};

static const u16 mas_oxili_links[] = {
	MSM8998_SLV_BIMC_SNOC_1,
	MSM8998_SLV_HMSS_L3,
	MSM8998_SLV_EBI,
	MSM8998_SLV_BIMC_SNOC_0,
};

static const u16 mas_mnoc_bimc_links[] = {
	MSM8998_SLV_BIMC_SNOC_1,
	MSM8998_SLV_HMSS_L3,
	MSM8998_SLV_EBI,
	MSM8998_SLV_BIMC_SNOC_0,
};

static const u16 mas_snoc_bimc_links[] = {
	MSM8998_SLV_HMSS_L3,
	MSM8998_SLV_EBI,
};

static const u16 slv_bimc_snoc_0_links[] = {
	MSM8998_MAS_BIMC_SNOC_0,
};

static const u16 slv_bimc_snoc_1_links[] = {
	MSM8998_MAS_BIMC_SNOC_1,
};

static const u16 mas_snoc_cnoc_links[] = {
	MSM8998_SLV_SKL,
	MSM8998_SLV_BLSP_2,
	MSM8998_SLV_MESSAGE_RAM,
	MSM8998_SLV_TLMM_WEST,
	MSM8998_SLV_TSIF,
	MSM8998_SLV_MPM,
	MSM8998_SLV_BIMC_CFG,
	MSM8998_SLV_TLMM_EAST,
	MSM8998_SLV_SPDM,
	MSM8998_SLV_PIMEM_CFG,
	MSM8998_SLV_A1NOC_SMMU_CFG,
	MSM8998_SLV_BLSP_1,
	MSM8998_SLV_CLK_CTL,
	MSM8998_SLV_PRNG,
	MSM8998_SLV_USB3_0,
	MSM8998_SLV_QDSS_CFG,
	MSM8998_SLV_QM_CFG,
	MSM8998_SLV_A2NOC_CFG,
	MSM8998_SLV_PMIC_ARB,
	MSM8998_SLV_UFS_CFG,
	MSM8998_SLV_SRVC_CNOC,
	MSM8998_SLV_AHB2PHY,
	MSM8998_SLV_IPA,
	MSM8998_SLV_GLM,
	MSM8998_SLV_SNOC_CFG,
	MSM8998_SLV_SSC_CFG,
	MSM8998_SLV_SDCC_2,
	MSM8998_SLV_SDCC_4,
	MSM8998_SLV_PDM,
	MSM8998_SLV_CNOC_MNOC_MMSS_CFG,
	MSM8998_SLV_CNOC_MNOC_CFG,
	MSM8998_SLV_MSS_CFG,
	MSM8998_SLV_IMEM_CFG,
	MSM8998_SLV_A1NOC_CFG,
	MSM8998_SLV_GPUSS_CFG,
	MSM8998_SLV_TCSR,
	MSM8998_SLV_TLMM_NORTH,
};

static const u16 mas_qdss_dap_links[] = {
	MSM8998_SLV_SKL,
	MSM8998_SLV_BLSP_2,
	MSM8998_SLV_MESSAGE_RAM,
	MSM8998_SLV_TLMM_WEST,
	MSM8998_SLV_TSIF,
	MSM8998_SLV_MPM,
	MSM8998_SLV_BIMC_CFG,
	MSM8998_SLV_TLMM_EAST,
	MSM8998_SLV_SPDM,
	MSM8998_SLV_PIMEM_CFG,
	MSM8998_SLV_A1NOC_SMMU_CFG,
	MSM8998_SLV_BLSP_1,
	MSM8998_SLV_CLK_CTL,
	MSM8998_SLV_PRNG,
	MSM8998_SLV_USB3_0,
	MSM8998_SLV_QDSS_CFG,
	MSM8998_SLV_QM_CFG,
	MSM8998_SLV_A2NOC_CFG,
	MSM8998_SLV_PMIC_ARB,
	MSM8998_SLV_UFS_CFG,
	MSM8998_SLV_SRVC_CNOC,
	MSM8998_SLV_AHB2PHY,
	MSM8998_SLV_IPA,
	MSM8998_SLV_GLM,
	MSM8998_SLV_SNOC_CFG,
	MSM8998_SLV_SDCC_2,
	MSM8998_SLV_SDCC_4,
	MSM8998_SLV_PDM,
	MSM8998_SLV_CNOC_MNOC_MMSS_CFG,
	MSM8998_SLV_CNOC_MNOC_CFG,
	MSM8998_SLV_MSS_CFG,
	MSM8998_SLV_IMEM_CFG,
	MSM8998_SLV_A1NOC_CFG,
	MSM8998_SLV_GPUSS_CFG,
	MSM8998_SLV_SSC_CFG,
	MSM8998_SLV_TCSR,
	MSM8998_SLV_TLMM_NORTH,
	MSM8998_SLV_CNOC_A2NOC,
};

static const u16 slv_cnoc_mnoc_cfg_links[] = {
	MSM8998_MAS_CNOC_MNOC_CFG,
};

static const u16 mas_hmss_links[] = {
	MSM8998_SLV_PIMEM,
	MSM8998_SLV_IMEM,
	MSM8998_SLV_SNOC_BIMC,
};

static const u16 mas_qdss_bam_links[] = {
	MSM8998_SLV_IMEM,
	MSM8998_SLV_PIMEM,
	MSM8998_SLV_SNOC_CNOC,
	MSM8998_SLV_SNOC_BIMC,
};

static const u16 mas_snoc_cfg_links[] = {
	MSM8998_SLV_SRVC_SNOC,
};

static const u16 mas_bimc_snoc_0_links[] = {
	MSM8998_SLV_PIMEM,
	MSM8998_SLV_LPASS,
	MSM8998_SLV_HMSS,
	MSM8998_SLV_WLAN,
	MSM8998_SLV_SNOC_CNOC,
	MSM8998_SLV_IMEM,
	MSM8998_SLV_QDSS_STM,
};

static const u16 mas_bimc_snoc_1_links[] = {
	MSM8998_SLV_PCIE_0,
};

static const u16 mas_a1noc_snoc_links[] = {
	MSM8998_SLV_PIMEM,
	MSM8998_SLV_PCIE_0,
	MSM8998_SLV_LPASS,
	MSM8998_SLV_HMSS,
	MSM8998_SLV_SNOC_BIMC,
	MSM8998_SLV_SNOC_CNOC,
	MSM8998_SLV_IMEM,
	MSM8998_SLV_QDSS_STM,
};

static const u16 mas_a2noc_snoc_links[] = {
	MSM8998_SLV_PIMEM,
	MSM8998_SLV_PCIE_0,
	MSM8998_SLV_LPASS,
	MSM8998_SLV_HMSS,
	MSM8998_SLV_SNOC_BIMC,
	MSM8998_SLV_WLAN,
	MSM8998_SLV_SNOC_CNOC,
	MSM8998_SLV_IMEM,
	MSM8998_SLV_QDSS_STM,
};

static const u16 mas_qdss_etr_links[] = {
	MSM8998_SLV_IMEM,
	MSM8998_SLV_PIMEM,
	MSM8998_SLV_SNOC_CNOC,
	MSM8998_SLV_SNOC_BIMC,
};

static const u16 slv_snoc_bimc_links[] = {
	MSM8998_MAS_SNOC_BIMC,
};

static const u16 slv_snoc_cnoc_links[] = {
	MSM8998_MAS_SNOC_CNOC,
};

static const u16 mas_cnoc_mnoc_cfg_links[] = {
	MSM8998_SLV_SRVC_MNOC,
};

static const u16 mas_cpp_links[] = {
	MSM8998_SLV_MNOC_BIMC,
};

static const u16 mas_jpeg_links[] = {
	MSM8998_SLV_MNOC_BIMC,
};

static const u16 mas_mdp_p0_links[] = {
	MSM8998_SLV_MNOC_BIMC,
};

static const u16 mas_mdp_p1_links[] = {
	MSM8998_SLV_MNOC_BIMC,
};

static const u16 mas_rotator_links[] = {
	MSM8998_SLV_MNOC_BIMC,
};

static const u16 mas_venus_links[] = {
	MSM8998_SLV_MNOC_BIMC,
};

static const u16 mas_vfe_links[] = {
	MSM8998_SLV_MNOC_BIMC,
};

static const u16 mas_venus_vmem_links[] = {
	MSM8998_SLV_VMEM,
};

static const u16 slv_mnoc_bimc_links[] = {
	MSM8998_MAS_MNOC_BIMC,
};


static struct qcom_icc_node mas_gnoc_bimc = {
	.name = "mas-gnoc-bimc",
	.id = MSM8998_MAS_GNOC_BIMC,
	.buswidth = 8,
	.channels = 2,
	.mas_rpm_id = 144,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 2,
	.links = mas_gnoc_bimc_links
};

static struct qcom_icc_node mas_oxili = {
	.name = "mas-oxili",
	.id = MSM8998_MAS_OXILI,
	.buswidth = 8,
	.channels = 2,
	.mas_rpm_id = 6,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 4,
	.links = mas_oxili_links
};

static struct qcom_icc_node mas_mnoc_bimc = {
	.name = "mas-mnoc-bimc",
	.id = MSM8998_MAS_MNOC_BIMC,
	.buswidth = 8,
	.channels = 2,
	.mas_rpm_id = 2,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 4,
	.links = mas_mnoc_bimc_links
};

static struct qcom_icc_node mas_snoc_bimc = {
	.name = "mas-snoc-bimc",
	.id = MSM8998_MAS_SNOC_BIMC,
	.buswidth = 8,
	.channels = 2,
	.mas_rpm_id = 3,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 2,
	.links = mas_snoc_bimc_links
};

static struct qcom_icc_node slv_ebi = {
	.name = "slv-ebi",
	.id = MSM8998_SLV_EBI,
	.buswidth = 8,
	.channels = 2,
	.mas_rpm_id = -1,
	.slv_rpm_id = 0,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_hmss_l3 = {
	.name = "slv-hmss-l3",
	.id = MSM8998_SLV_HMSS_L3,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 160,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_bimc_snoc_0 = {
	.name = "slv-bimc-snoc-0",
	.id = MSM8998_SLV_BIMC_SNOC_0,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 2,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_bimc_snoc_0_links
};

static struct qcom_icc_node slv_bimc_snoc_1 = {
	.name = "slv-bimc-snoc-1",
	.id = MSM8998_SLV_BIMC_SNOC_1,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 138,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_bimc_snoc_1_links
};

static struct qcom_icc_node mas_snoc_cnoc = {
	.name = "mas-snoc-cnoc",
	.id = MSM8998_MAS_SNOC_CNOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 52,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 37,
	.links = mas_snoc_cnoc_links
};

static struct qcom_icc_node mas_qdss_dap = {
	.name = "mas-qdss-dap",
	.id = MSM8998_MAS_QDSS_DAP,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 49,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 38,
	.links = mas_qdss_dap_links
};

static struct qcom_icc_node slv_cnoc_a2noc = {
	.name = "slv-cnoc-a2noc",
	.id = MSM8998_SLV_CNOC_A2NOC,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 208,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ssc_cfg = {
	.name = "slv-ssc-cfg",
	.id = MSM8998_SLV_SSC_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 177,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_mpm = {
	.name = "slv-mpm",
	.id = MSM8998_SLV_MPM,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 62,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pmic_arb = {
	.name = "slv-pmic-arb",
	.id = MSM8998_SLV_PMIC_ARB,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 59,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tlmm_north = {
	.name = "slv-tlmm-north",
	.id = MSM8998_SLV_TLMM_NORTH,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 214,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pimem_cfg = {
	.name = "slv-pimem-cfg",
	.id = MSM8998_SLV_PIMEM_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 167,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_imem_cfg = {
	.name = "slv-imem-cfg",
	.id = MSM8998_SLV_IMEM_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 54,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_message_ram = {
	.name = "slv-message-ram",
	.id = MSM8998_SLV_MESSAGE_RAM,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 55,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_skl = {
	.name = "slv-skl",
	.id = MSM8998_SLV_SKL,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 196,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_bimc_cfg = {
	.name = "slv-bimc-cfg",
	.id = MSM8998_SLV_BIMC_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 56,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_prng = {
	.name = "slv-prng",
	.id = MSM8998_SLV_PRNG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 44,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_a2noc_cfg = {
	.name = "slv-a2noc-cfg",
	.id = MSM8998_SLV_A2NOC_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 150,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ipa = {
	.name = "slv-ipa",
	.id = MSM8998_SLV_IPA,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 183,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tcsr = {
	.name = "slv-tcsr",
	.id = MSM8998_SLV_TCSR,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 50,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_snoc_cfg = {
	.name = "slv-snoc-cfg",
	.id = MSM8998_SLV_SNOC_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 70,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_clk_ctl = {
	.name = "slv-clk-ctl",
	.id = MSM8998_SLV_CLK_CTL,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 47,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_glm = {
	.name = "slv-glm",
	.id = MSM8998_SLV_GLM,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 209,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_spdm = {
	.name = "slv-spdm",
	.id = MSM8998_SLV_SPDM,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 60,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_gpuss_cfg = {
	.name = "slv-gpuss-cfg",
	.id = MSM8998_SLV_GPUSS_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 11,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_cnoc_mnoc_cfg = {
	.name = "slv-cnoc-mnoc-cfg",
	.id = MSM8998_SLV_CNOC_MNOC_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 66,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_cnoc_mnoc_cfg_links
};

static struct qcom_icc_node slv_qm_cfg = {
	.name = "slv-qm-cfg",
	.id = MSM8998_SLV_QM_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 212,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_mss_cfg = {
	.name = "slv-mss-cfg",
	.id = MSM8998_SLV_MSS_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 48,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ufs_cfg = {
	.name = "slv-ufs-cfg",
	.id = MSM8998_SLV_UFS_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 92,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tlmm_west = {
	.name = "slv-tlmm-west",
	.id = MSM8998_SLV_TLMM_WEST,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 215,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_a1noc_cfg = {
	.name = "slv-a1noc-cfg",
	.id = MSM8998_SLV_A1NOC_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 147,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ahb2phy = {
	.name = "slv-ahb2phy",
	.id = MSM8998_SLV_AHB2PHY,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 163,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_blsp_2 = {
	.name = "slv-blsp-2",
	.id = MSM8998_SLV_BLSP_2,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 37,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pdm = {
	.name = "slv-pdm",
	.id = MSM8998_SLV_PDM,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 41,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_usb3_0 = {
	.name = "slv-usb3-0",
	.id = MSM8998_SLV_USB3_0,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 22,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_a1noc_smmu_cfg = {
	.name = "slv-a1noc-smmu-cfg",
	.id = MSM8998_SLV_A1NOC_SMMU_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 149,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_blsp_1 = {
	.name = "slv-blsp-1",
	.id = MSM8998_SLV_BLSP_1,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 39,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_sdcc_2 = {
	.name = "slv-sdcc-2",
	.id = MSM8998_SLV_SDCC_2,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 33,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_sdcc_4 = {
	.name = "slv-sdcc-4",
	.id = MSM8998_SLV_SDCC_4,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 34,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tsif = {
	.name = "slv-tsif",
	.id = MSM8998_SLV_TSIF,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 35,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_qdss_cfg = {
	.name = "slv-qdss-cfg",
	.id = MSM8998_SLV_QDSS_CFG,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 63,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tlmm_east = {
	.name = "slv-tlmm-east",
	.id = MSM8998_SLV_TLMM_EAST,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 213,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_cnoc_mnoc_mmss_cfg = {
	.name = "slv-cnoc-mnoc-mmss-cfg",
	.id = MSM8998_SLV_CNOC_MNOC_MMSS_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 58,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_srvc_cnoc = {
	.name = "slv-srvc-cnoc",
	.id = MSM8998_SLV_SRVC_CNOC,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 76,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node mas_hmss = {
	.name = "mas-hmss",
	.id = MSM8998_MAS_HMSS,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 118,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 3,
	.num_links = 3,
	.links = mas_hmss_links
};

static struct qcom_icc_node mas_qdss_bam = {
	.name = "mas-qdss-bam",
	.id = MSM8998_MAS_QDSS_BAM,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 19,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 1,
	.num_links = 4,
	.links = mas_qdss_bam_links
};

static struct qcom_icc_node mas_snoc_cfg = {
	.name = "mas-snoc-cfg",
	.id = MSM8998_MAS_SNOC_CFG,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 20,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_snoc_cfg_links
};

static struct qcom_icc_node mas_bimc_snoc_0 = {
	.name = "mas-bimc-snoc-0",
	.id = MSM8998_MAS_BIMC_SNOC_0,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 21,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 7,
	.links = mas_bimc_snoc_0_links
};

static struct qcom_icc_node mas_bimc_snoc_1 = {
	.name = "mas-bimc-snoc-1",
	.id = MSM8998_MAS_BIMC_SNOC_1,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 109,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_bimc_snoc_1_links
};

static struct qcom_icc_node mas_a1noc_snoc = {
	.name = "mas-a1noc-snoc",
	.id = MSM8998_MAS_A1NOC_SNOC,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 111,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 8,
	.links = mas_a1noc_snoc_links
};

static struct qcom_icc_node mas_a2noc_snoc = {
	.name = "mas-a2noc-snoc",
	.id = MSM8998_MAS_A2NOC_SNOC,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 112,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 9,
	.links = mas_a2noc_snoc_links
};

static struct qcom_icc_node mas_qdss_etr = {
	.name = "mas-qdss-etr",
	.id = MSM8998_MAS_QDSS_ETR,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 31,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 2,
	.num_links = 4,
	.links = mas_qdss_etr_links
};

static struct qcom_icc_node slv_hmss = {
	.name = "slv-hmss",
	.id = MSM8998_SLV_HMSS,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 20,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_lpass = {
	.name = "slv-lpass",
	.id = MSM8998_SLV_LPASS,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 21,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_wlan = {
	.name = "slv-wlan",
	.id = MSM8998_SLV_WLAN,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 206,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_snoc_bimc = {
	.name = "slv-snoc-bimc",
	.id = MSM8998_SLV_SNOC_BIMC,
	.buswidth = 32,
	.channels = 2,
	.mas_rpm_id = -1,
	.slv_rpm_id = 24,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_snoc_bimc_links
};

static struct qcom_icc_node slv_snoc_cnoc = {
	.name = "slv-snoc-cnoc",
	.id = MSM8998_SLV_SNOC_CNOC,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 25,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_snoc_cnoc_links
};

static struct qcom_icc_node slv_imem = {
	.name = "slv-imem",
	.id = MSM8998_SLV_IMEM,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 26,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pimem = {
	.name = "slv-pimem",
	.id = MSM8998_SLV_PIMEM,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 166,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_qdss_stm = {
	.name = "slv-qdss-stm",
	.id = MSM8998_SLV_QDSS_STM,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 30,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pcie_0 = {
	.name = "slv-pcie-0",
	.id = MSM8998_SLV_PCIE_0,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 84,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_srvc_snoc = {
	.name = "slv-srvc-snoc",
	.id = MSM8998_SLV_SRVC_SNOC,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 29,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node mas_cnoc_mnoc_cfg = {
	.name = "mas-cnoc-mnoc-cfg",
	.id = MSM8998_MAS_CNOC_MNOC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 5,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_cnoc_mnoc_cfg_links
};

static struct qcom_icc_node mas_cpp = {
	.name = "mas-cpp",
	.id = MSM8998_MAS_CPP,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = 115,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_BYPASS,
	.qos.qos_port = 5,
	.num_links = 1,
	.links = mas_cpp_links
};

static struct qcom_icc_node mas_jpeg = {
	.name = "mas-jpeg",
	.id = MSM8998_MAS_JPEG,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = 7,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_BYPASS,
	.qos.qos_port = 7,
	.num_links = 1,
	.links = mas_jpeg_links
};

static struct qcom_icc_node mas_mdp_p0 = {
	.name = "mas-mdp-p0",
	.id = MSM8998_MAS_MDP_P0,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = 8,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_BYPASS,
	.qos.qos_port = 1,
	.num_links = 1,
	.links = mas_mdp_p0_links
};

static struct qcom_icc_node mas_mdp_p1 = {
	.name = "mas-mdp-p1",
	.id = MSM8998_MAS_MDP_P1,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = 61,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_BYPASS,
	.qos.qos_port = 2,
	.num_links = 1,
	.links = mas_mdp_p1_links
};

static struct qcom_icc_node mas_rotator = {
	.name = "mas-rotator",
	.id = MSM8998_MAS_ROTATOR,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = 120,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_BYPASS,
	.qos.qos_port = 0,
	.num_links = 1,
	.links = mas_rotator_links
};

static struct qcom_icc_node mas_venus = {
	.name = "mas-venus",
	.id = MSM8998_MAS_VENUS,
	.buswidth = 32,
	.channels = 2,
	.mas_rpm_id = 9,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_BYPASS,
	.qos.qos_port = 3,
	.num_links = 1,
	.links = mas_venus_links
};

static struct qcom_icc_node mas_vfe = {
	.name = "mas-vfe",
	.id = MSM8998_MAS_VFE,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = 11,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_BYPASS,
	.qos.qos_port = 6,
	.num_links = 1,
	.links = mas_vfe_links
};

static struct qcom_icc_node mas_venus_vmem = {
	.name = "mas-venus-vmem",
	.id = MSM8998_MAS_VENUS_VMEM,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = 121,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_venus_vmem_links
};

static struct qcom_icc_node slv_mnoc_bimc = {
	.name = "slv-mnoc-bimc",
	.id = MSM8998_SLV_MNOC_BIMC,
	.buswidth = 32,
	.channels = 2,
	.mas_rpm_id = -1,
	.slv_rpm_id = 16,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_mnoc_bimc_links
};

static struct qcom_icc_node slv_vmem = {
	.name = "slv-vmem",
	.id = MSM8998_SLV_VMEM,
	.buswidth = 32,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 179,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_srvc_mnoc = {
	.name = "slv-srvc-mnoc",
	.id = MSM8998_SLV_SRVC_MNOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 17,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static const u16 mas_pcie_0_links[] = {
	MSM8998_SLV_A1NOC_SNOC,
};

static struct qcom_icc_node mas_pcie_0 = {
	.name = "mas-pcie-0",
	.id = MSM8998_MAS_PCIE_0,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 65,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 1,
	.num_links = 1,
	.links = mas_pcie_0_links
};

static const u16 mas_ufs_links[] = {
	MSM8998_SLV_A1NOC_SNOC,
};

static struct qcom_icc_node mas_ufs = {
	.name = "mas-ufs",
	.id = MSM8998_MAS_UFS,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 68,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 0,
	.num_links = 1,
	.links = mas_ufs_links
};

static const u16 mas_usb3_links[] = {
	MSM8998_SLV_A1NOC_SNOC,
};

static struct qcom_icc_node mas_usb3 = {
	.name = "mas-usb3",
	.id = MSM8998_MAS_USB3,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 32,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 2,
	.num_links = 1,
	.links = mas_usb3_links
};

static const u16 mas_blsp_2_links[] = {
	MSM8998_SLV_A1NOC_SNOC,
};

static struct qcom_icc_node mas_blsp_2 = {
	.name = "mas-blsp-2",
	.id = MSM8998_MAS_BLSP_2,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 39,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 4,
	.num_links = 1,
	.links = mas_blsp_2_links
};

static const u16 slv_a1noc_snoc_links[] = {
	MSM8998_MAS_A1NOC_SNOC,
};

static struct qcom_icc_node slv_a1noc_snoc = {
	.name = "slv-a1noc-snoc",
	.id = MSM8998_SLV_A1NOC_SNOC,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 142,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_a1noc_snoc_links
};

static const u16 mas_ipa_links[] = {
	MSM8998_SLV_A2NOC_SNOC,
};

static struct qcom_icc_node mas_ipa = {
	.name = "mas-ipa",
	.id = MSM8998_MAS_IPA,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 59,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_ipa_links
};

static const u16 mas_cnoc_a2noc_links[] = {
	MSM8998_SLV_A2NOC_SNOC,
};

static struct qcom_icc_node mas_cnoc_a2noc = {
	.name = "mas-cnoc-a2noc",
	.id = MSM8998_MAS_CNOC_A2NOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 146,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_cnoc_a2noc_links
};

static const u16 mas_sdcc_2_links[] = {
	MSM8998_SLV_A2NOC_SNOC,
};

static struct qcom_icc_node mas_sdcc_2 = {
	.name = "mas-sdcc-2",
	.id = MSM8998_MAS_SDCC_2,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 35,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 6,
	.num_links = 1,
	.links = mas_sdcc_2_links
};

static const u16 mas_sdcc_4_links[] = {
	MSM8998_SLV_A2NOC_SNOC,
};

static struct qcom_icc_node mas_sdcc_4 = {
	.name = "mas-sdcc-4",
	.id = MSM8998_MAS_SDCC_4,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 36,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 7,
	.num_links = 1,
	.links = mas_sdcc_4_links
};

static const u16 mas_blsp_1_links[] = {
	MSM8998_SLV_A2NOC_SNOC,
};

static struct qcom_icc_node mas_blsp_1 = {
	.name = "mas-blsp-1",
	.id = MSM8998_MAS_BLSP_1,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = 41,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_FIXED,
	.qos.areq_prio = 1,
	.qos.prio_level = 1,
	.qos.qos_port = 8,
	.num_links = 1,
	.links = mas_blsp_1_links
};

static const u16 mas_tsif_links[] = {
	MSM8998_SLV_A2NOC_SNOC,
};

static struct qcom_icc_node mas_tsif = {
	.name = "mas-tsif",
	.id = MSM8998_MAS_TSIF,
	.buswidth = 4,
	.channels = 1,
	.mas_rpm_id = 37,
	.slv_rpm_id = -1,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_tsif_links
};

static const u16 mas_crypto_c0_links[] = {
	MSM8998_SLV_CR_VIRT_A2NOC,
};

static struct qcom_icc_node mas_crypto_c0 = {
	.name = "mas-crypto-c0",
	.id = MSM8998_MAS_CRYPTO_C0,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 23,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_crypto_c0_links
};

static const u16 mas_cr_virt_a2noc_links[] = {
	MSM8998_SLV_A2NOC_SNOC,
};

static struct qcom_icc_node mas_cr_virt_a2noc = {
	.name = "mas-cr-virt-a2noc",
	.id = MSM8998_MAS_CR_VIRT_A2NOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 145,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_cr_virt_a2noc_links
};

static const u16 slv_cr_virt_a2noc_links[] = {
	MSM8998_MAS_CR_VIRT_A2NOC,
};

static struct qcom_icc_node slv_cr_virt_a2noc = {
	.name = "slv-cr-virt-a2noc",
	.id = MSM8998_SLV_CR_VIRT_A2NOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 207,
	.qos.ap_owned = true,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_cr_virt_a2noc_links
};

static const u16 slv_a2noc_snoc_links[] = {
	MSM8998_MAS_A2NOC_SNOC,
};

static struct qcom_icc_node slv_a2noc_snoc = {
	.name = "slv-a2noc-snoc",
	.id = MSM8998_SLV_A2NOC_SNOC,
	.buswidth = 16,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 143,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_a2noc_snoc_links
};

static struct qcom_icc_node * const a1noc_nodes[] = {
	[0] = &mas_pcie_0,
	[1] = &mas_ufs,
	[2] = &mas_usb3,
	[3] = &mas_blsp_2,
	[4] = &slv_a1noc_snoc,
};

static struct qcom_icc_node * const a2noc_nodes[] = {
	[0] = &mas_ipa,
	[1] = &mas_cnoc_a2noc,
	[2] = &mas_sdcc_2,
	[3] = &mas_sdcc_4,
	[4] = &mas_blsp_1,
	[5] = &mas_tsif,
	[6] = &mas_crypto_c0,
	[7] = &mas_cr_virt_a2noc,
	[8] = &slv_cr_virt_a2noc,
	[9] = &slv_a2noc_snoc,
};

static struct qcom_icc_node * const bimc_nodes[] = {
	[0] = &mas_gnoc_bimc,
	[1] = &mas_oxili,
	[2] = &mas_mnoc_bimc,
	[3] = &mas_snoc_bimc,
	[4] = &slv_ebi,
	[5] = &slv_hmss_l3,
	[6] = &slv_bimc_snoc_0,
	[7] = &slv_bimc_snoc_1,
};

static struct qcom_icc_node * const cnoc_nodes[] = {
	[0] = &mas_snoc_cnoc,
	[1] = &mas_qdss_dap,
	[2] = &slv_cnoc_a2noc,
	[3] = &slv_ssc_cfg,
	[4] = &slv_mpm,
	[5] = &slv_pmic_arb,
	[6] = &slv_tlmm_north,
	[7] = &slv_pimem_cfg,
	[8] = &slv_imem_cfg,
	[9] = &slv_message_ram,
	[10] = &slv_skl,
	[11] = &slv_bimc_cfg,
	[12] = &slv_prng,
	[13] = &slv_a2noc_cfg,
	[14] = &slv_ipa,
	[15] = &slv_tcsr,
	[16] = &slv_snoc_cfg,
	[17] = &slv_clk_ctl,
	[18] = &slv_glm,
	[19] = &slv_spdm,
	[20] = &slv_gpuss_cfg,
	[21] = &slv_cnoc_mnoc_cfg,
	[22] = &slv_qm_cfg,
	[23] = &slv_mss_cfg,
	[24] = &slv_ufs_cfg,
	[25] = &slv_tlmm_west,
	[26] = &slv_a1noc_cfg,
	[27] = &slv_ahb2phy,
	[28] = &slv_blsp_2,
	[29] = &slv_pdm,
	[30] = &slv_usb3_0,
	[31] = &slv_a1noc_smmu_cfg,
	[32] = &slv_blsp_1,
	[33] = &slv_sdcc_2,
	[34] = &slv_sdcc_4,
	[35] = &slv_tsif,
	[36] = &slv_qdss_cfg,
	[37] = &slv_tlmm_east,
	[38] = &slv_cnoc_mnoc_mmss_cfg,
	[39] = &slv_srvc_cnoc,
};

static struct qcom_icc_node * const snoc_nodes[] = {
	[0] = &mas_hmss,
	[1] = &mas_qdss_bam,
	[2] = &mas_snoc_cfg,
	[3] = &mas_bimc_snoc_0,
	[4] = &mas_bimc_snoc_1,
	[5] = &mas_a1noc_snoc,
	[6] = &mas_a2noc_snoc,
	[7] = &mas_qdss_etr,
	[8] = &slv_hmss,
	[9] = &slv_lpass,
	[10] = &slv_wlan,
	[11] = &slv_snoc_bimc,
	[12] = &slv_snoc_cnoc,
	[13] = &slv_imem,
	[14] = &slv_pimem,
	[15] = &slv_qdss_stm,
	[16] = &slv_pcie_0,
	[17] = &slv_srvc_snoc,
};

static struct qcom_icc_node * const mnoc_nodes[] = {
	[0] = &mas_cnoc_mnoc_cfg,
	[1] = &mas_cpp,
	[2] = &mas_jpeg,
	[3] = &mas_mdp_p0,
	[4] = &mas_mdp_p1,
	[5] = &mas_rotator,
	[6] = &mas_venus,
	[7] = &mas_vfe,
	[8] = &mas_venus_vmem,
	[9] = &slv_mnoc_bimc,
	[10] = &slv_vmem,
	[11] = &slv_srvc_mnoc,
};

static const struct qcom_icc_desc msm8998_bimc = {
	.type = QCOM_ICC_BIMC,
	.nodes = bimc_nodes,
	.num_nodes = ARRAY_SIZE(bimc_nodes),
	.bus_clk_desc = &bimc_clk,
	.keep_alive = true,
	.ab_coeff = 153,
};

static const struct qcom_icc_desc msm8998_cnoc = {
	.type = QCOM_ICC_NOC,
	.nodes = cnoc_nodes,
	.num_nodes = ARRAY_SIZE(cnoc_nodes),
	.bus_clk_desc = &bus_0_clk,
	.keep_alive = true,
};

static const struct regmap_config msm8998_snoc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x3fffc,
	.fast_io	= true,
};

static const struct qcom_icc_desc msm8998_snoc = {
	.regmap_cfg = &msm8998_snoc_regmap_config,
	.qos_offset = 0x5000,
	.type = QCOM_ICC_NOC,
	.nodes = snoc_nodes,
	.num_nodes = ARRAY_SIZE(snoc_nodes),
	.bus_clk_desc = &bus_1_clk,
	.keep_alive = true,
};

static const struct regmap_config msm8998_mnoc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0xfffc,
	.fast_io	= true,
};

static const char * const msm8998_mnoc_intf_clocks[] = {
	"noc_cfg_ahb",
	"mnoc_ahb",
	"camss_ahb",
	"video_ahb",
	"video_axi",
	"mdss_ahb",
	"mdss_axi",
};

static const struct qcom_icc_desc msm8998_mnoc = {
	.regmap_cfg = &msm8998_mnoc_regmap_config,
	.qos_offset = 0x4000,
	.intf_clocks = msm8998_mnoc_intf_clocks,
	.num_intf_clocks = ARRAY_SIZE(msm8998_mnoc_intf_clocks),
	.type = QCOM_ICC_NOC,
	.nodes = mnoc_nodes,
	.num_nodes = ARRAY_SIZE(mnoc_nodes),
	.bus_clk_desc = &mmaxi_0_clk,
	.keep_alive = true,
};

/*
 * a1noc is registered but UNEXERCISED: nothing in DT consumes it yet.
 * It carries UFS, USB3, PCIe and BLSP2, and none of those declare
 * interconnects, so no consumer votes on it and keep_alive's floor is
 * the only rate it ever asks for. It is here for topology completeness
 * and because UFS becomes a real consumer if the rootfs ever moves to
 * internal flash. Its sibling a2noc is what carries the SD card and is
 * device-validated; a1noc is not.
 */
static const struct regmap_config msm8998_a1noc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x1fffc,
	.fast_io	= true,
};

static const char * const msm8998_a1noc_intf_clocks[] = {
	"ufs_axi",
	"aggre1_ufs_axi",
	"aggre1_usb3_axi",
	"blsp2_ahb",
};

static const struct qcom_icc_desc msm8998_a1noc = {
	.regmap_cfg = &msm8998_a1noc_regmap_config,
	.qos_offset = 0x9000,
	.intf_clocks = msm8998_a1noc_intf_clocks,
	.num_intf_clocks = ARRAY_SIZE(msm8998_a1noc_intf_clocks),
	.type = QCOM_ICC_NOC,
	.nodes = a1noc_nodes,
	.num_nodes = ARRAY_SIZE(a1noc_nodes),
	.bus_clk_desc = &aggre1_branch_clk,
	.keep_alive = true,
};

static const struct regmap_config msm8998_a2noc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0xfffc,
	.fast_io	= true,
};

/*
 * Downstream enables these before writing any a2noc QoS register
 * (qcom,node-qos-clks on fab-a2noc). icc-rpm wraps the QoS loop in
 * clk_bulk_prepare_enable() of desc->intf_clocks, so listing them
 * here is what makes those writes safe. Its fourth entry, the IPA
 * clock, has no mainline equivalent on msm8998 - only GCC_IPA_BCR,
 * a reset - so mas_ipa keeps qos_mode INVALID.
 */
static const char * const msm8998_a2noc_intf_clocks[] = {
	"sdcc2_ahb",
	"sdcc4_ahb",
	"blsp1_ahb",
};

static const struct qcom_icc_desc msm8998_a2noc = {
	.regmap_cfg = &msm8998_a2noc_regmap_config,
	.qos_offset = 0x5000,
	.intf_clocks = msm8998_a2noc_intf_clocks,
	.num_intf_clocks = ARRAY_SIZE(msm8998_a2noc_intf_clocks),
	.type = QCOM_ICC_NOC,
	.nodes = a2noc_nodes,
	.num_nodes = ARRAY_SIZE(a2noc_nodes),
	.bus_clk_desc = &aggre2_branch_clk,
	.keep_alive = true,
};

static const struct of_device_id qnoc_of_match[] = {
	{ .compatible = "qcom,msm8998-bimc", .data = &msm8998_bimc },
	{ .compatible = "qcom,msm8998-cnoc", .data = &msm8998_cnoc },
	{ .compatible = "qcom,msm8998-snoc", .data = &msm8998_snoc },
	{ .compatible = "qcom,msm8998-mnoc", .data = &msm8998_mnoc },
	{ .compatible = "qcom,msm8998-a1noc", .data = &msm8998_a1noc },
	{ .compatible = "qcom,msm8998-a2noc", .data = &msm8998_a2noc },
	{ }
};
MODULE_DEVICE_TABLE(of, qnoc_of_match);

static struct platform_driver qnoc_driver = {
	.probe = qnoc_probe,
	.remove = qnoc_remove,
	.driver = {
		.name = "qnoc-msm8998",
		.of_match_table = qnoc_of_match,
		.sync_state = icc_sync_state,
	}
};
static int __init qnoc_driver_init(void)
{
	return platform_driver_register(&qnoc_driver);
}
core_initcall(qnoc_driver_init);

static void __exit qnoc_driver_exit(void)
{
	platform_driver_unregister(&qnoc_driver);
}
module_exit(qnoc_driver_exit);

MODULE_DESCRIPTION("Qualcomm MSM8998 NoC driver");
MODULE_LICENSE("GPL v2");

