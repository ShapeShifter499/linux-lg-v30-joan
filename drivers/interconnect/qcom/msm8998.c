// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm MSM8998 Network-on-Chip (NoC) interconnect driver
 *
 * Node topology generated from the downstream msm8998-bus.dtsi;
 * RPM clock resources from the icc-rpm family. QoS registers are
 * not programmed yet (regmap absent), so all qos_mode values are
 * NOC_QOS_MODE_INVALID even for AP-owned nodes; the flags and
 * ports are carried for future QoS bring-up.
 */

#include <linux/device.h>
#include <linux/interconnect-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/interconnect/qcom,msm8998.h>

#include "icc-rpm.h"

static const u16 mas_gnoc_bimc_links[] = {
	SLV_EBI,
	SLV_BIMC_SNOC_0,
};

static const u16 mas_oxili_links[] = {
	SLV_BIMC_SNOC_1,
	SLV_HMSS_L3,
	SLV_EBI,
	SLV_BIMC_SNOC_0,
};

static const u16 mas_mnoc_bimc_links[] = {
	SLV_BIMC_SNOC_1,
	SLV_HMSS_L3,
	SLV_EBI,
	SLV_BIMC_SNOC_0,
};

static const u16 mas_snoc_bimc_links[] = {
	SLV_HMSS_L3,
	SLV_EBI,
};

static const u16 slv_bimc_snoc_0_links[] = {
	MAS_BIMC_SNOC_0,
};

static const u16 slv_bimc_snoc_1_links[] = {
	MAS_BIMC_SNOC_1,
};

static const u16 mas_snoc_cnoc_links[] = {
	SLV_SKL,
	SLV_BLSP_2,
	SLV_MESSAGE_RAM,
	SLV_TLMM_WEST,
	SLV_TSIF,
	SLV_MPM,
	SLV_BIMC_CFG,
	SLV_TLMM_EAST,
	SLV_SPDM,
	SLV_PIMEM_CFG,
	SLV_A1NOC_SMMU_CFG,
	SLV_BLSP_1,
	SLV_CLK_CTL,
	SLV_PRNG,
	SLV_USB3_0,
	SLV_QDSS_CFG,
	SLV_QM_CFG,
	SLV_A2NOC_CFG,
	SLV_PMIC_ARB,
	SLV_UFS_CFG,
	SLV_SRVC_CNOC,
	SLV_AHB2PHY,
	SLV_IPA,
	SLV_GLM,
	SLV_SNOC_CFG,
	SLV_SSC_CFG,
	SLV_SDCC_2,
	SLV_SDCC_4,
	SLV_PDM,
	SLV_CNOC_MNOC_MMSS_CFG,
	SLV_CNOC_MNOC_CFG,
	SLV_MSS_CFG,
	SLV_IMEM_CFG,
	SLV_A1NOC_CFG,
	SLV_GPUSS_CFG,
	SLV_TCSR,
	SLV_TLMM_NORTH,
};

static const u16 mas_qdss_dap_links[] = {
	SLV_SKL,
	SLV_BLSP_2,
	SLV_MESSAGE_RAM,
	SLV_TLMM_WEST,
	SLV_TSIF,
	SLV_MPM,
	SLV_BIMC_CFG,
	SLV_TLMM_EAST,
	SLV_SPDM,
	SLV_PIMEM_CFG,
	SLV_A1NOC_SMMU_CFG,
	SLV_BLSP_1,
	SLV_CLK_CTL,
	SLV_PRNG,
	SLV_USB3_0,
	SLV_QDSS_CFG,
	SLV_QM_CFG,
	SLV_A2NOC_CFG,
	SLV_PMIC_ARB,
	SLV_UFS_CFG,
	SLV_SRVC_CNOC,
	SLV_AHB2PHY,
	SLV_IPA,
	SLV_GLM,
	SLV_SNOC_CFG,
	SLV_SDCC_2,
	SLV_SDCC_4,
	SLV_PDM,
	SLV_CNOC_MNOC_MMSS_CFG,
	SLV_CNOC_MNOC_CFG,
	SLV_MSS_CFG,
	SLV_IMEM_CFG,
	SLV_A1NOC_CFG,
	SLV_GPUSS_CFG,
	SLV_SSC_CFG,
	SLV_TCSR,
	SLV_TLMM_NORTH,
	SLV_CNOC_A2NOC,
};

static const u16 slv_cnoc_mnoc_cfg_links[] = {
	MAS_CNOC_MNOC_CFG,
};

static const u16 mas_hmss_links[] = {
	SLV_PIMEM,
	SLV_IMEM,
	SLV_SNOC_BIMC,
};

static const u16 mas_qdss_bam_links[] = {
	SLV_IMEM,
	SLV_PIMEM,
	SLV_SNOC_CNOC,
	SLV_SNOC_BIMC,
};

static const u16 mas_snoc_cfg_links[] = {
	SLV_SRVC_SNOC,
};

static const u16 mas_bimc_snoc_0_links[] = {
	SLV_PIMEM,
	SLV_LPASS,
	SLV_HMSS,
	SLV_WLAN,
	SLV_SNOC_CNOC,
	SLV_IMEM,
	SLV_QDSS_STM,
};

static const u16 mas_bimc_snoc_1_links[] = {
	SLV_PCIE_0,
};

static const u16 mas_a1noc_snoc_links[] = {
	SLV_PIMEM,
	SLV_PCIE_0,
	SLV_LPASS,
	SLV_HMSS,
	SLV_SNOC_BIMC,
	SLV_SNOC_CNOC,
	SLV_IMEM,
	SLV_QDSS_STM,
};

static const u16 mas_a2noc_snoc_links[] = {
	SLV_PIMEM,
	SLV_PCIE_0,
	SLV_LPASS,
	SLV_HMSS,
	SLV_SNOC_BIMC,
	SLV_WLAN,
	SLV_SNOC_CNOC,
	SLV_IMEM,
	SLV_QDSS_STM,
};

static const u16 mas_qdss_etr_links[] = {
	SLV_IMEM,
	SLV_PIMEM,
	SLV_SNOC_CNOC,
	SLV_SNOC_BIMC,
};

static const u16 slv_snoc_bimc_links[] = {
	MAS_SNOC_BIMC,
};

static const u16 slv_snoc_cnoc_links[] = {
	MAS_SNOC_CNOC,
};

static const u16 mas_cnoc_mnoc_cfg_links[] = {
	SLV_SRVC_MNOC,
};

static const u16 mas_cpp_links[] = {
	SLV_MNOC_BIMC,
};

static const u16 mas_jpeg_links[] = {
	SLV_MNOC_BIMC,
};

static const u16 mas_mdp_p0_links[] = {
	SLV_MNOC_BIMC,
};

static const u16 mas_mdp_p1_links[] = {
	SLV_MNOC_BIMC,
};

static const u16 mas_rotator_links[] = {
	SLV_MNOC_BIMC,
};

static const u16 mas_venus_links[] = {
	SLV_MNOC_BIMC,
};

static const u16 mas_vfe_links[] = {
	SLV_MNOC_BIMC,
};

static const u16 mas_venus_vmem_links[] = {
	SLV_VMEM,
};

static const u16 slv_mnoc_bimc_links[] = {
	MAS_MNOC_BIMC,
};


static struct qcom_icc_node mas_gnoc_bimc = {
	.name = "mas-gnoc-bimc",
	.id = MAS_GNOC_BIMC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 144,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 2,
	.links = mas_gnoc_bimc_links
};

static struct qcom_icc_node mas_oxili = {
	.name = "mas-oxili",
	.id = MAS_OXILI,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 6,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 4,
	.links = mas_oxili_links
};

static struct qcom_icc_node mas_mnoc_bimc = {
	.name = "mas-mnoc-bimc",
	.id = MAS_MNOC_BIMC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 2,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 4,
	.links = mas_mnoc_bimc_links
};

static struct qcom_icc_node mas_snoc_bimc = {
	.name = "mas-snoc-bimc",
	.id = MAS_SNOC_BIMC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 3,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 2,
	.links = mas_snoc_bimc_links
};

static struct qcom_icc_node slv_ebi = {
	.name = "slv-ebi",
	.id = SLV_EBI,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 0,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_hmss_l3 = {
	.name = "slv-hmss-l3",
	.id = SLV_HMSS_L3,
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
	.id = SLV_BIMC_SNOC_0,
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
	.id = SLV_BIMC_SNOC_1,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 138,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_bimc_snoc_1_links
};

static struct qcom_icc_node mas_snoc_cnoc = {
	.name = "mas-snoc-cnoc",
	.id = MAS_SNOC_CNOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 52,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 37,
	.links = mas_snoc_cnoc_links
};

static struct qcom_icc_node mas_qdss_dap = {
	.name = "mas-qdss-dap",
	.id = MAS_QDSS_DAP,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 49,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 38,
	.links = mas_qdss_dap_links
};

static struct qcom_icc_node slv_cnoc_a2noc = {
	.name = "slv-cnoc-a2noc",
	.id = SLV_CNOC_A2NOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 208,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ssc_cfg = {
	.name = "slv-ssc-cfg",
	.id = SLV_SSC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 177,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_mpm = {
	.name = "slv-mpm",
	.id = SLV_MPM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 62,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pmic_arb = {
	.name = "slv-pmic-arb",
	.id = SLV_PMIC_ARB,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 59,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tlmm_north = {
	.name = "slv-tlmm-north",
	.id = SLV_TLMM_NORTH,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 214,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pimem_cfg = {
	.name = "slv-pimem-cfg",
	.id = SLV_PIMEM_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 167,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_imem_cfg = {
	.name = "slv-imem-cfg",
	.id = SLV_IMEM_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 54,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_message_ram = {
	.name = "slv-message-ram",
	.id = SLV_MESSAGE_RAM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 55,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_skl = {
	.name = "slv-skl",
	.id = SLV_SKL,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 196,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_bimc_cfg = {
	.name = "slv-bimc-cfg",
	.id = SLV_BIMC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 56,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_prng = {
	.name = "slv-prng",
	.id = SLV_PRNG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 44,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_a2noc_cfg = {
	.name = "slv-a2noc-cfg",
	.id = SLV_A2NOC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 150,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ipa = {
	.name = "slv-ipa",
	.id = SLV_IPA,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 183,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tcsr = {
	.name = "slv-tcsr",
	.id = SLV_TCSR,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 50,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_snoc_cfg = {
	.name = "slv-snoc-cfg",
	.id = SLV_SNOC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 70,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_clk_ctl = {
	.name = "slv-clk-ctl",
	.id = SLV_CLK_CTL,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 47,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_glm = {
	.name = "slv-glm",
	.id = SLV_GLM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 209,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_spdm = {
	.name = "slv-spdm",
	.id = SLV_SPDM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 60,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_gpuss_cfg = {
	.name = "slv-gpuss-cfg",
	.id = SLV_GPUSS_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 11,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_cnoc_mnoc_cfg = {
	.name = "slv-cnoc-mnoc-cfg",
	.id = SLV_CNOC_MNOC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 66,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_cnoc_mnoc_cfg_links
};

static struct qcom_icc_node slv_qm_cfg = {
	.name = "slv-qm-cfg",
	.id = SLV_QM_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 212,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_mss_cfg = {
	.name = "slv-mss-cfg",
	.id = SLV_MSS_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 48,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ufs_cfg = {
	.name = "slv-ufs-cfg",
	.id = SLV_UFS_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 92,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tlmm_west = {
	.name = "slv-tlmm-west",
	.id = SLV_TLMM_WEST,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 215,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_a1noc_cfg = {
	.name = "slv-a1noc-cfg",
	.id = SLV_A1NOC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 147,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_ahb2phy = {
	.name = "slv-ahb2phy",
	.id = SLV_AHB2PHY,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 163,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_blsp_2 = {
	.name = "slv-blsp-2",
	.id = SLV_BLSP_2,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 37,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_pdm = {
	.name = "slv-pdm",
	.id = SLV_PDM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 41,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_usb3_0 = {
	.name = "slv-usb3-0",
	.id = SLV_USB3_0,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 22,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_a1noc_smmu_cfg = {
	.name = "slv-a1noc-smmu-cfg",
	.id = SLV_A1NOC_SMMU_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 149,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_blsp_1 = {
	.name = "slv-blsp-1",
	.id = SLV_BLSP_1,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 39,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_sdcc_2 = {
	.name = "slv-sdcc-2",
	.id = SLV_SDCC_2,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 33,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_sdcc_4 = {
	.name = "slv-sdcc-4",
	.id = SLV_SDCC_4,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 34,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tsif = {
	.name = "slv-tsif",
	.id = SLV_TSIF,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 35,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_qdss_cfg = {
	.name = "slv-qdss-cfg",
	.id = SLV_QDSS_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 63,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_tlmm_east = {
	.name = "slv-tlmm-east",
	.id = SLV_TLMM_EAST,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 213,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_cnoc_mnoc_mmss_cfg = {
	.name = "slv-cnoc-mnoc-mmss-cfg",
	.id = SLV_CNOC_MNOC_MMSS_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 58,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_srvc_cnoc = {
	.name = "slv-srvc-cnoc",
	.id = SLV_SRVC_CNOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 76,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node mas_hmss = {
	.name = "mas-hmss",
	.id = MAS_HMSS,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 118,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 3,
	.links = mas_hmss_links
};

static struct qcom_icc_node mas_qdss_bam = {
	.name = "mas-qdss-bam",
	.id = MAS_QDSS_BAM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 19,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 4,
	.links = mas_qdss_bam_links
};

static struct qcom_icc_node mas_snoc_cfg = {
	.name = "mas-snoc-cfg",
	.id = MAS_SNOC_CFG,
	.buswidth = 8,
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
	.id = MAS_BIMC_SNOC_0,
	.buswidth = 8,
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
	.id = MAS_BIMC_SNOC_1,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 109,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_bimc_snoc_1_links
};

static struct qcom_icc_node mas_a1noc_snoc = {
	.name = "mas-a1noc-snoc",
	.id = MAS_A1NOC_SNOC,
	.buswidth = 8,
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
	.id = MAS_A2NOC_SNOC,
	.buswidth = 8,
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
	.id = MAS_QDSS_ETR,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 31,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 4,
	.links = mas_qdss_etr_links
};

static struct qcom_icc_node slv_hmss = {
	.name = "slv-hmss",
	.id = SLV_HMSS,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 20,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_lpass = {
	.name = "slv-lpass",
	.id = SLV_LPASS,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 21,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_wlan = {
	.name = "slv-wlan",
	.id = SLV_WLAN,
	.buswidth = 8,
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
	.id = SLV_SNOC_BIMC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 24,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_snoc_bimc_links
};

static struct qcom_icc_node slv_snoc_cnoc = {
	.name = "slv-snoc-cnoc",
	.id = SLV_SNOC_CNOC,
	.buswidth = 8,
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
	.id = SLV_IMEM,
	.buswidth = 8,
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
	.id = SLV_PIMEM,
	.buswidth = 8,
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
	.id = SLV_QDSS_STM,
	.buswidth = 8,
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
	.id = SLV_PCIE_0,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 84,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_srvc_snoc = {
	.name = "slv-srvc-snoc",
	.id = SLV_SRVC_SNOC,
	.buswidth = 8,
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
	.id = MAS_CNOC_MNOC_CFG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 5,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_cnoc_mnoc_cfg_links
};

static struct qcom_icc_node mas_cpp = {
	.name = "mas-cpp",
	.id = MAS_CPP,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 115,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_cpp_links
};

static struct qcom_icc_node mas_jpeg = {
	.name = "mas-jpeg",
	.id = MAS_JPEG,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 7,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_jpeg_links
};

static struct qcom_icc_node mas_mdp_p0 = {
	.name = "mas-mdp-p0",
	.id = MAS_MDP_P0,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 8,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_mdp_p0_links
};

static struct qcom_icc_node mas_mdp_p1 = {
	.name = "mas-mdp-p1",
	.id = MAS_MDP_P1,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 61,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_mdp_p1_links
};

static struct qcom_icc_node mas_rotator = {
	.name = "mas-rotator",
	.id = MAS_ROTATOR,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 120,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_rotator_links
};

static struct qcom_icc_node mas_venus = {
	.name = "mas-venus",
	.id = MAS_VENUS,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 9,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_venus_links
};

static struct qcom_icc_node mas_vfe = {
	.name = "mas-vfe",
	.id = MAS_VFE,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 11,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_vfe_links
};

static struct qcom_icc_node mas_venus_vmem = {
	.name = "mas-venus-vmem",
	.id = MAS_VENUS_VMEM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = 121,
	.slv_rpm_id = -1,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = mas_venus_vmem_links
};

static struct qcom_icc_node slv_mnoc_bimc = {
	.name = "slv-mnoc-bimc",
	.id = SLV_MNOC_BIMC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 16,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 1,
	.links = slv_mnoc_bimc_links
};

static struct qcom_icc_node slv_vmem = {
	.name = "slv-vmem",
	.id = SLV_VMEM,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 179,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node slv_srvc_mnoc = {
	.name = "slv-srvc-mnoc",
	.id = SLV_SRVC_MNOC,
	.buswidth = 8,
	.channels = 1,
	.mas_rpm_id = -1,
	.slv_rpm_id = 17,
	.qos.ap_owned = false,
	.qos.qos_mode = NOC_QOS_MODE_INVALID,
	.num_links = 0,
	.links = NULL
};

static struct qcom_icc_node * const bimc_nodes[] = {
	[MAS_GNOC_BIMC] = &mas_gnoc_bimc,
	[MAS_OXILI] = &mas_oxili,
	[MAS_MNOC_BIMC] = &mas_mnoc_bimc,
	[MAS_SNOC_BIMC] = &mas_snoc_bimc,
	[SLV_EBI] = &slv_ebi,
	[SLV_HMSS_L3] = &slv_hmss_l3,
	[SLV_BIMC_SNOC_0] = &slv_bimc_snoc_0,
	[SLV_BIMC_SNOC_1] = &slv_bimc_snoc_1,
};

static struct qcom_icc_node * const cnoc_nodes[] = {
	[MAS_SNOC_CNOC] = &mas_snoc_cnoc,
	[MAS_QDSS_DAP] = &mas_qdss_dap,
	[SLV_CNOC_A2NOC] = &slv_cnoc_a2noc,
	[SLV_SSC_CFG] = &slv_ssc_cfg,
	[SLV_MPM] = &slv_mpm,
	[SLV_PMIC_ARB] = &slv_pmic_arb,
	[SLV_TLMM_NORTH] = &slv_tlmm_north,
	[SLV_PIMEM_CFG] = &slv_pimem_cfg,
	[SLV_IMEM_CFG] = &slv_imem_cfg,
	[SLV_MESSAGE_RAM] = &slv_message_ram,
	[SLV_SKL] = &slv_skl,
	[SLV_BIMC_CFG] = &slv_bimc_cfg,
	[SLV_PRNG] = &slv_prng,
	[SLV_A2NOC_CFG] = &slv_a2noc_cfg,
	[SLV_IPA] = &slv_ipa,
	[SLV_TCSR] = &slv_tcsr,
	[SLV_SNOC_CFG] = &slv_snoc_cfg,
	[SLV_CLK_CTL] = &slv_clk_ctl,
	[SLV_GLM] = &slv_glm,
	[SLV_SPDM] = &slv_spdm,
	[SLV_GPUSS_CFG] = &slv_gpuss_cfg,
	[SLV_CNOC_MNOC_CFG] = &slv_cnoc_mnoc_cfg,
	[SLV_QM_CFG] = &slv_qm_cfg,
	[SLV_MSS_CFG] = &slv_mss_cfg,
	[SLV_UFS_CFG] = &slv_ufs_cfg,
	[SLV_TLMM_WEST] = &slv_tlmm_west,
	[SLV_A1NOC_CFG] = &slv_a1noc_cfg,
	[SLV_AHB2PHY] = &slv_ahb2phy,
	[SLV_BLSP_2] = &slv_blsp_2,
	[SLV_PDM] = &slv_pdm,
	[SLV_USB3_0] = &slv_usb3_0,
	[SLV_A1NOC_SMMU_CFG] = &slv_a1noc_smmu_cfg,
	[SLV_BLSP_1] = &slv_blsp_1,
	[SLV_SDCC_2] = &slv_sdcc_2,
	[SLV_SDCC_4] = &slv_sdcc_4,
	[SLV_TSIF] = &slv_tsif,
	[SLV_QDSS_CFG] = &slv_qdss_cfg,
	[SLV_TLMM_EAST] = &slv_tlmm_east,
	[SLV_CNOC_MNOC_MMSS_CFG] = &slv_cnoc_mnoc_mmss_cfg,
	[SLV_SRVC_CNOC] = &slv_srvc_cnoc,
};

static struct qcom_icc_node * const snoc_nodes[] = {
	[MAS_HMSS] = &mas_hmss,
	[MAS_QDSS_BAM] = &mas_qdss_bam,
	[MAS_SNOC_CFG] = &mas_snoc_cfg,
	[MAS_BIMC_SNOC_0] = &mas_bimc_snoc_0,
	[MAS_BIMC_SNOC_1] = &mas_bimc_snoc_1,
	[MAS_A1NOC_SNOC] = &mas_a1noc_snoc,
	[MAS_A2NOC_SNOC] = &mas_a2noc_snoc,
	[MAS_QDSS_ETR] = &mas_qdss_etr,
	[SLV_HMSS] = &slv_hmss,
	[SLV_LPASS] = &slv_lpass,
	[SLV_WLAN] = &slv_wlan,
	[SLV_SNOC_BIMC] = &slv_snoc_bimc,
	[SLV_SNOC_CNOC] = &slv_snoc_cnoc,
	[SLV_IMEM] = &slv_imem,
	[SLV_PIMEM] = &slv_pimem,
	[SLV_QDSS_STM] = &slv_qdss_stm,
	[SLV_PCIE_0] = &slv_pcie_0,
	[SLV_SRVC_SNOC] = &slv_srvc_snoc,
};

static struct qcom_icc_node * const mnoc_nodes[] = {
	[MAS_CNOC_MNOC_CFG] = &mas_cnoc_mnoc_cfg,
	[MAS_CPP] = &mas_cpp,
	[MAS_JPEG] = &mas_jpeg,
	[MAS_MDP_P0] = &mas_mdp_p0,
	[MAS_MDP_P1] = &mas_mdp_p1,
	[MAS_ROTATOR] = &mas_rotator,
	[MAS_VENUS] = &mas_venus,
	[MAS_VFE] = &mas_vfe,
	[MAS_VENUS_VMEM] = &mas_venus_vmem,
	[SLV_MNOC_BIMC] = &slv_mnoc_bimc,
	[SLV_VMEM] = &slv_vmem,
	[SLV_SRVC_MNOC] = &slv_srvc_mnoc,
};

static const struct qcom_icc_desc msm8998_bimc = {
	.type = QCOM_ICC_BIMC,
	.nodes = bimc_nodes,
	.num_nodes = ARRAY_SIZE(bimc_nodes),
	.bus_clk_desc = &bimc_clk,
};

static const struct qcom_icc_desc msm8998_cnoc = {
	.type = QCOM_ICC_NOC,
	.nodes = cnoc_nodes,
	.num_nodes = ARRAY_SIZE(cnoc_nodes),
	.bus_clk_desc = &bus_0_clk,
};

static const struct qcom_icc_desc msm8998_snoc = {
	.type = QCOM_ICC_NOC,
	.nodes = snoc_nodes,
	.num_nodes = ARRAY_SIZE(snoc_nodes),
	.bus_clk_desc = &bus_1_clk,
};

static const struct qcom_icc_desc msm8998_mnoc = {
	.type = QCOM_ICC_NOC,
	.nodes = mnoc_nodes,
	.num_nodes = ARRAY_SIZE(mnoc_nodes),
	.bus_clk_desc = &mmaxi_0_clk,
};

static const struct of_device_id qnoc_of_match[] = {
	{ .compatible = "qcom,msm8998-bimc", .data = &msm8998_bimc },
	{ .compatible = "qcom,msm8998-cnoc", .data = &msm8998_cnoc },
	{ .compatible = "qcom,msm8998-snoc", .data = &msm8998_snoc },
	{ .compatible = "qcom,msm8998-mnoc", .data = &msm8998_mnoc },
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

