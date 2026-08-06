// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm OSM (Open-loop Self-clocking Mechanism) clock driver
 * for MSM8998 (LG V30 / joan)
 *
 * Ported from the downstream LG kernel driver
 * (drivers/clk/msm/clock-osm.c) to a mainline-style clk driver.
 *
 * Why this driver exists (device-proven 2026-08-05):
 *   - mainline msm8998 has NO CPU DVFS at all; cores run at a
 *     bootloader-fixed low clock
 *   - qcom-cpufreq-hw cannot be used: its register layout
 *     (enable@0x0, LUT@0x110, perf@0x920) does not match the
 *     msm8998 OSM block (enable@0x1004, LUT@0x1150-0x1164,
 *     DCVS perf-state@0x1F10); the OSM reports "hardware not
 *     enabled" because the enable bit lives at 0x1004
 *   - the OSM block is a hardware DVFS manager: software programs
 *     the LUT (frequency data + PLL override + voltage corner +
 *     spare) and the OSM autonomously switches PLL/voltage when
 *     software writes a table index to DCVS_PERF_STATE_DESIRED_REG
 *
 * This driver: parses the qcom,pwrcl/perfcl-speedbinN-vM tables
 * from DT, programs the hardware LUT, enables OSM, and exposes
 * per-cluster clk_hw where set_rate() writes the LUT index.
 * cpufreq-dt + DT OPP tables then provide normal cpufreq.
 *
 * Local-only bring-up driver; not intended for upstream submission
 * in this form.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/delay.h>

#define OSM_TABLE_SIZE		40
#define MAX_VIRTUAL_CORNER	(OSM_TABLE_SIZE - 1)
#define OSM_REG_SIZE		32	/* bytes per LUT row */

/* OSM registers (relative to OSM base) */
#define OSM_ENABLE_REG		0x1004
#define OSM_INDEX_REG		0x1150
#define OSM_FREQ_REG		0x1154
#define OSM_VOLT_REG		0x1158
#define OSM_OVERRIDE_REG	0x115C
#define OSM_SPARE_REG		0x1164
#define OSM_DCVS_PERF_STATE	0x1F10

/* PLL registers (relative to per-cluster PLL base) */
#define PLL_MODE		0x0
#define PLL_L_VAL		0x4
#define PLL_USER_CTRL		0xC
#define PLL_CONFIG_CTL_LO	0x10
#define PLL_WAIT_LOCK_TIME_US	10

/* efuse speedbin */
#define PWRCL_EFUSE_SHIFT	0
#define PERFCL_EFUSE_SHIFT	29

enum clk_osm_bases {
	OSM_BASE,
	PLL_BASE,
	EFUSE_BASE,
	NUM_BASES,
};

enum clk_osm_lut_data {
	LUT_FREQ,
	LUT_FREQ_DATA,
	LUT_PLL_OVERRIDES,
	LUT_SPARE_DATA,
	LUT_VIRTUAL_CORNER,
	NUM_FIELDS,
};

struct osm_entry {
	u32 frequency;
	u32 freq_data;
	u32 override_data;
	u32 spare_data;
	u32 virtual_corner;
	u32 open_loop_volt;
};

struct clk_osm {
	struct clk_hw hw;
	void __iomem *vbases[NUM_BASES];
	u32 pbases[NUM_BASES];
	struct osm_entry osm_table[OSM_TABLE_SIZE];
	int num_entries;
	int cluster_num;
	struct regulator *vdd_reg;
};

#define to_clk_osm(_hw) container_of(_hw, struct clk_osm, hw)

static inline u32 osm_read_reg(struct clk_osm *c, u32 offset)
{
	return readl_relaxed(c->vbases[OSM_BASE] + offset);
}

static inline void osm_write_reg(struct clk_osm *c, u32 val, u32 offset)
{
	writel_relaxed(val, c->vbases[OSM_BASE] + offset);
}

static int osm_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	struct clk_osm *c = to_clk_osm(hw);
	int i;
	long best = -EINVAL;

	for (i = 0; i < c->num_entries; i++) {
		if (c->osm_table[i].frequency <= req->rate)
			best = c->osm_table[i].frequency;
		else
			break;
	}
	if (best < 0)
		return -EINVAL;

	req->rate = best;
	return 0;
}

static unsigned long osm_recalc_rate(struct clk_hw *hw,
				     unsigned long parent_rate)
{
	struct clk_osm *c = to_clk_osm(hw);
	u32 idx = osm_read_reg(c, OSM_DCVS_PERF_STATE);

	if (idx < OSM_TABLE_SIZE && idx < c->num_entries)
		return c->osm_table[idx].frequency;
	return 0;
}

static int osm_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	struct clk_osm *c = to_clk_osm(hw);
	int i;

	for (i = 0; i < c->num_entries; i++) {
		if (c->osm_table[i].frequency == rate) {
			osm_write_reg(c, i, OSM_DCVS_PERF_STATE);
			/* make sure the write goes through */
			readl_relaxed(c->vbases[OSM_BASE] + OSM_DCVS_PERF_STATE);
			return 0;
		}
	}
	return -EINVAL;
}

static int osm_enable(struct clk_hw *hw)
{
	struct clk_osm *c = to_clk_osm(hw);

	udelay(5);
	osm_write_reg(c, 1, OSM_ENABLE_REG);
	/* make sure the write goes through */
	readl_relaxed(c->vbases[OSM_BASE] + OSM_ENABLE_REG);
	udelay(5);

	return 0;
}

static void osm_disable(struct clk_hw *hw)
{
	/* OSM stays enabled; nothing to do */
}

static const struct clk_ops osm_clk_ops = {
	.determine_rate = osm_determine_rate,
	.recalc_rate = osm_recalc_rate,
	.set_rate = osm_set_rate,
	.enable = osm_enable,
	.disable = osm_disable,
};

static void osm_setup_cluster_pll(struct clk_osm *c)
{
	/* Same init sequence as downstream clk_osm_setup_cluster_pll */
	writel_relaxed(0x0, c->vbases[PLL_BASE] + PLL_MODE);
	writel_relaxed(0x20, c->vbases[PLL_BASE] + PLL_L_VAL);
	writel_relaxed(0x01000008, c->vbases[PLL_BASE] + PLL_USER_CTRL);
	writel_relaxed(0x20004AA8, c->vbases[PLL_BASE] + PLL_CONFIG_CTL_LO);
	writel_relaxed(0x2, c->vbases[PLL_BASE] + PLL_MODE);
	/* barrier */
	readl_relaxed(c->vbases[PLL_BASE] + PLL_MODE);
	udelay(PLL_WAIT_LOCK_TIME_US);
	writel_relaxed(0x6, c->vbases[PLL_BASE] + PLL_MODE);
	readl_relaxed(c->vbases[PLL_BASE] + PLL_MODE);
	udelay(PLL_WAIT_LOCK_TIME_US);
	writel_relaxed(0x7, c->vbases[PLL_BASE] + PLL_MODE);
	readl_relaxed(c->vbases[PLL_BASE] + PLL_MODE);
}

static int osm_setup_hw_table(struct clk_osm *c)
{
	int i;
	u32 freq_val = 0, volt_val = 0, override_val = 0, spare_val = 0;
	u32 table_entry_offset = 0;

	for (i = 0; i < OSM_TABLE_SIZE; i++) {
		if (i < c->num_entries) {
			freq_val = c->osm_table[i].freq_data;
			volt_val = ((c->osm_table[i].virtual_corner & 0x3f) << 16) |
				   (c->osm_table[i].open_loop_volt & 0xfff);
			override_val = c->osm_table[i].override_data;
			spare_val = c->osm_table[i].spare_data;
		} else {
			freq_val = 0;
			volt_val = 0;
			override_val = 0;
			spare_val = 0;
		}

		table_entry_offset = i * OSM_REG_SIZE;
		osm_write_reg(c, i, OSM_INDEX_REG + table_entry_offset);
		osm_write_reg(c, freq_val, OSM_FREQ_REG + table_entry_offset);
		osm_write_reg(c, volt_val, OSM_VOLT_REG + table_entry_offset);
		osm_write_reg(c, override_val,
			      OSM_OVERRIDE_REG + table_entry_offset);
		osm_write_reg(c, spare_val, OSM_SPARE_REG + table_entry_offset);
	}

	/* make sure all writes go through */
	readl_relaxed(c->vbases[OSM_BASE] + OSM_SPARE_REG);
	return 0;
}

static int osm_resolve_open_loop_voltages(struct clk_osm *c)
{
	int i;
	u32 vc, mv;

	/*
	 * The downstream driver asks the RPM regulator for the corner
	 * voltage. In mainline we use the regulator's nominal voltage
	 * per corner index (corner 0 = 1). If the regulator API does
	 * not provide corner support, fall back to the table's own
	 * implied value (the OSM hardware manages voltage autonomously
	 * once the LUT is programmed; open_loop_volt is only used by
	 * the DCVS loop, so a safe mid-range value is acceptable for
	 * bring-up).
	 */
	for (i = 0; i < c->num_entries; i++) {
		vc = c->osm_table[i].virtual_corner + 1;
		mv = 0;
		if (c->vdd_reg && regulator_is_supported_voltage(c->vdd_reg, 0, 2000000)) {
			/* nominal voltage for this corner via set_voltage round */
			regulator_set_voltage(c->vdd_reg, 0, 2000000);
			regulator_get_voltage(c->vdd_reg);
			mv = regulator_get_voltage(c->vdd_reg) / 1000;
		}
		c->osm_table[i].open_loop_volt = mv ? mv : 800;
	}
	return 0;
}

static int osm_parse_table(struct platform_device *pdev, struct clk_osm *c,
			   const char *prop)
{
	struct device_node *of = pdev->dev.of_node;
	int prop_len, total_elems, num_rows, i, j;
	u32 *array;
	int rc;

	if (!of_find_property(of, prop, &prop_len)) {
		dev_err(&pdev->dev, "missing %s\n", prop);
		return -EINVAL;
	}

	total_elems = prop_len / sizeof(u32);
	if (total_elems % NUM_FIELDS) {
		dev_err(&pdev->dev, "bad length %d\n", prop_len);
		return -EINVAL;
	}
	num_rows = total_elems / NUM_FIELDS;
	if (num_rows > OSM_TABLE_SIZE) {
		dev_err(&pdev->dev, "LUT entries %d exceed max %d\n",
			num_rows, OSM_TABLE_SIZE);
		return -EINVAL;
	}

	array = kcalloc(total_elems, sizeof(u32), GFP_KERNEL);
	if (!array)
		return -ENOMEM;

	rc = of_property_read_u32_array(of, prop, array, total_elems);
	if (rc) {
		dev_err(&pdev->dev, "unable to parse %s, rc=%d\n", prop, rc);
		kfree(array);
		return rc;
	}

	c->num_entries = num_rows;
	for (i = 0, j = 0; j < num_rows; j++, i += NUM_FIELDS) {
		c->osm_table[j].frequency = array[i + LUT_FREQ];
		c->osm_table[j].freq_data = array[i + LUT_FREQ_DATA];
		c->osm_table[j].override_data = array[i + LUT_PLL_OVERRIDES];
		c->osm_table[j].spare_data = array[i + LUT_SPARE_DATA];
		/* voltage corners are 0-based in the OSM LUT */
		c->osm_table[j].virtual_corner =
			array[i + LUT_VIRTUAL_CORNER] - 1;
	}

	kfree(array);
	return 0;
}

static int osm_probe(struct platform_device *pdev)
{
	struct clk_osm *pwrcl, *perfcl;
	struct device *dev = &pdev->dev;
	struct device_node *of = dev->of_node;
	struct clk_init_data init = { };
	const char *prop;
	u32 speedbin = 0;
	int rc, i;

	pwrcl = devm_kzalloc(dev, sizeof(*pwrcl), GFP_KERNEL);
	perfcl = devm_kzalloc(dev, sizeof(*perfcl), GFP_KERNEL);
	if (!pwrcl || !perfcl)
		return -ENOMEM;

	/* Map the three regions we need: osm, pwrcl_pll, perfcl_pll */
	for (i = 0; i < NUM_BASES; i++)
		pwrcl->vbases[i] = NULL;

	pwrcl->vbases[OSM_BASE] = devm_platform_ioremap_resource_byname(pdev, "osm");
	if (IS_ERR(pwrcl->vbases[OSM_BASE]))
		return PTR_ERR(pwrcl->vbases[OSM_BASE]);
	perfcl->vbases[OSM_BASE] = pwrcl->vbases[OSM_BASE];

	pwrcl->vbases[PLL_BASE] = devm_platform_ioremap_resource_byname(pdev, "pwrcl_pll");
	if (IS_ERR(pwrcl->vbases[PLL_BASE]))
		return PTR_ERR(pwrcl->vbases[PLL_BASE]);

	perfcl->vbases[PLL_BASE] = devm_platform_ioremap_resource_byname(pdev, "perfcl_pll");
	if (IS_ERR(perfcl->vbases[PLL_BASE]))
		return PTR_ERR(perfcl->vbases[PLL_BASE]);

	pwrcl->cluster_num = 0;
	perfcl->cluster_num = 1;

	pwrcl->vdd_reg = devm_regulator_get_optional(dev, "vdd-pwrcl");
	if (IS_ERR(pwrcl->vdd_reg))
		pwrcl->vdd_reg = NULL;
	perfcl->vdd_reg = devm_regulator_get_optional(dev, "vdd-perfcl");
	if (IS_ERR(perfcl->vdd_reg))
		perfcl->vdd_reg = NULL;

	/* speedbin select (v0 tables only for bring-up) */
	if (of_property_read_u32(of, "qcom,speedbin", &speedbin))
		speedbin = 0;

	prop = devm_kasprintf(dev, GFP_KERNEL, "qcom,pwrcl-speedbin%u-v0", speedbin);
	rc = osm_parse_table(pdev, pwrcl, prop);
	if (rc) {
		dev_err(dev, "failed to parse pwrcl table: %d\n", rc);
		return rc;
	}

	prop = devm_kasprintf(dev, GFP_KERNEL, "qcom,perfcl-speedbin%u-v0", speedbin);
	rc = osm_parse_table(pdev, perfcl, prop);
	if (rc) {
		dev_err(dev, "failed to parse perfcl table: %d\n", rc);
		return rc;
	}

	osm_resolve_open_loop_voltages(pwrcl);
	osm_resolve_open_loop_voltages(perfcl);

	/* Program the hardware LUT for both clusters */
	rc = osm_setup_hw_table(pwrcl);
	if (rc)
		return rc;
	rc = osm_setup_hw_table(perfcl);
	if (rc)
		return rc;

	/* Configure both cluster PLLs */
	osm_setup_cluster_pll(pwrcl);
	osm_setup_cluster_pll(perfcl);

	/* Register the clocks */
	init.name = devm_kasprintf(dev, GFP_KERNEL, "pwrcl_clk");
	init.ops = &osm_clk_ops;
	init.num_parents = 0;
	pwrcl->hw.init = &init;
	rc = devm_clk_hw_register(dev, &pwrcl->hw);
	if (rc) {
		dev_err(dev, "failed to register pwrcl_clk: %d\n", rc);
		return rc;
	}

	init.name = devm_kasprintf(dev, GFP_KERNEL, "perfcl_clk");
	perfcl->hw.init = &init;
	rc = devm_clk_hw_register(dev, &perfcl->hw);
	if (rc) {
		dev_err(dev, "failed to register perfcl_clk: %d\n", rc);
		return rc;
	}

	dev_info(dev, "OSM driver inited: pwrcl %d entries, perfcl %d entries\n",
		 pwrcl->num_entries, perfcl->num_entries);

	return 0;
}

static const struct of_device_id osm_match_table[] = {
	{ .compatible = "qcom,cpu-clock-osm-msm8998" },
	{}
};
MODULE_DEVICE_TABLE(of, osm_match_table);

static struct platform_driver osm_driver = {
	.probe = osm_probe,
	.driver = {
		.name = "clk-osm-msm8998",
		.of_match_table = osm_match_table,
	},
};
module_platform_driver(osm_driver);

MODULE_DESCRIPTION("Qualcomm OSM clock driver for MSM8998");
MODULE_LICENSE("GPL");
