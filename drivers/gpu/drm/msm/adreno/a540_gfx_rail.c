// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8998 (Adreno 540) VDD_GFX rail management when the GPU drives the rail
 * itself through vdd-supply and opp-microvolt, i.e. without a GFX CPR
 * controller in the kernel.
 *
 * Two things the downstream GFX CPR4 (cpr3-mmss-regulator.c) does for this
 * rail have to be reproduced for the rail to be correct on every part:
 *
 * 1. Per-part open-loop voltages. The GFX CPR fuses (qfprom row 65, plus
 *    the fusing revision, limitation and force-highest-corner fuses) give
 *    each part its own open-loop voltage per fuse corner. Downstream uses
 *    the scaled open-loop voltage as each corner's ceiling
 *    (qcom,cpr-scaled-open-loop-voltage-as-ceiling), so closed-loop CPR only
 *    ever lowers the rail from there. Running at the fused open-loop voltage
 *    is therefore the stock upper bound for the part, and is what the
 *    downstream driver itself falls back to when CPR is not allowed. A single
 *    voltage table copied from one phone is not: another part may be fused
 *    up to 150 mV higher.
 *
 * 2. MEM-ACC. The GFX memory accelerator select (TCSR acc-sel-l1, bit 0) must
 *    be 1 for GPU corners 1-3 (180/257/342 MHz) and 0 above, and downstream
 *    orders the write so the low-voltage setting is applied before the rail
 *    drops and the high-voltage setting only after it has risen. The OPP
 *    core sets the rail before config_clks() when scaling up and after it
 *    when scaling down, so writing the new corner's setting from
 *    config_clks() gives the same ordering.
 */

#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/regmap.h>

#include "adreno_gpu.h"

#define A540_CORNERS		8
#define A540_FUSE_CORNERS	4
#define A540_STEP_UV		4000	/* pm8005 s1 set-point step */
#define A540_FUSE_STEP_UV	10000
#define A540_FUSE_BITS		5
#define A540_FUSE_MIN_VAL	0x1f	/* most negative open-loop fuse value */

/* Partial binning markers in the FC0/FC1 open-loop fuse (msm8998 only) */
#define A540_PB_NEXT_CORNER	0xf
#define A540_PB_SAFE_CORNER	0xe
#define A540_PB_MAX_FUSE_CORNER	1

#define A540_LIMIT_NO_INTERP	3	/* no CPR, no interpolation */

/* Downstream corners 1..8; mainline has no OPP for the 180 MHz corner */
static const u32 a540_corner_hz[A540_CORNERS] = {
	180000000, 257000000, 342000000, 414000000,
	515000000, 596000000, 670000000, 710000000,
};

/* qcom,cpr-corner-fmax-map = <1 3 5 8>, 0-based */
static const int a540_fmax_corner[A540_FUSE_CORNERS] = { 0, 2, 4, 7 };

/* MSM8998 v2 open-loop reference voltages, fusing revision 0 and later */
static const int a540_ref_uv_rev0[A540_FUSE_CORNERS] = {
	616000, 740000, 828000, 1024000,
};
static const int a540_ref_uv[A540_FUSE_CORNERS] = {
	516000, 628000, 752000, 924000,
};

/* qcom,cpr-open-loop-voltage-fuse-adjustment, identical for every combo */
static const int a540_fuse_adj_uv[A540_FUSE_CORNERS] = { 60000, 0, 0, 0 };

/* qcom,cpr-voltage-ceiling: fuse combo 0, then combos 1-7 */
static const int a540_ceiling_uv_rev0[A540_CORNERS] = {
	716000, 716000, 772000, 880000, 908000, 948000, 1016000, 1088000,
};
static const int a540_ceiling_uv[A540_CORNERS] = {
	724000, 724000, 772000, 832000, 916000, 968000, 1024000, 1088000,
};

static const int a540_floor_uv[A540_CORNERS] = {
	516000, 516000, 532000, 584000, 632000, 672000, 712000, 756000,
};

/* Highest corner (0-based) that uses the low-voltage MEM-ACC setting */
#define A540_MEM_ACC_LOW_MAX_HZ	342000000

static int a540_fuse_to_uv(int ref_uv, u32 fuse)
{
	u32 sign = BIT(A540_FUSE_BITS - 1);
	int steps = fuse & (sign - 1);

	return ref_uv + ((fuse & sign) ? -steps : steps) * A540_FUSE_STEP_UV;
}

/* cpr3_interpolate(): y2 - (x2 - x)(y2 - y1) / (x2 - x1), truncating */
static int a540_interpolate(u64 x1, u64 y1, u64 x2, u64 y2, u64 x)
{
	u64 tmp;

	if (x1 >= x2 || y1 > y2 || x1 > x || x > x2)
		return y2;

	tmp = (x2 - x) * (y2 - y1);
	do_div(tmp, (u32)(x2 - x1));

	return y2 - tmp;
}

static int a540_fuse_corner(int corner)
{
	int i;

	for (i = 0; i < A540_FUSE_CORNERS; i++)
		if (corner <= a540_fmax_corner[i])
			return i;

	return A540_FUSE_CORNERS - 1;
}

static bool a540_pb_marker(u32 fuse)
{
	return fuse == A540_PB_NEXT_CORNER || fuse == A540_PB_SAFE_CORNER;
}

/*
 * Compute the per-corner open-loop voltages the way
 * cpr3_msm8996_mmss_calculate_open_loop_voltages(),
 * cpr3_limit_open_loop_voltages() and cpr3_msm8998_partial_binning_override()
 * do for "qcom,cpr4-msm8998-v2-mmss-regulator".
 */
static void a540_open_loop(struct device *dev, u32 rev, u32 limit, u32 force,
			   const u32 *init, int *ol)
{
	const int *ref = rev ? a540_ref_uv : a540_ref_uv_rev0;
	const int *ceil = rev ? a540_ceiling_uv : a540_ceiling_uv_rev0;
	int fuse_uv[A540_FUSE_CORNERS];
	int i, j;

	for (i = 0; i < A540_FUSE_CORNERS; i++) {
		u32 f = init[i];

		/* A partial binning marker is not a voltage */
		if (i <= A540_PB_MAX_FUSE_CORNER && a540_pb_marker(f))
			f = A540_FUSE_MIN_VAL;

		fuse_uv[i] = a540_fuse_to_uv(ref[i], f) + a540_fuse_adj_uv[i];
		if (i && fuse_uv[i] < fuse_uv[i - 1])
			fuse_uv[i] = fuse_uv[i - 1];
	}

	if (limit == A540_LIMIT_NO_INTERP) {
		for (i = 0; i < A540_CORNERS; i++)
			ol[i] = fuse_uv[a540_fuse_corner(i)];
	} else {
		for (i = 0; i <= a540_fmax_corner[0]; i++)
			ol[i] = fuse_uv[0];

		for (i = 1; i < A540_FUSE_CORNERS; i++) {
			int lo = a540_fmax_corner[i - 1];
			int hi = a540_fmax_corner[i];

			for (j = lo + 1; j <= hi; j++)
				ol[j] = a540_interpolate(a540_corner_hz[lo],
							 fuse_uv[i - 1],
							 a540_corner_hz[hi],
							 fuse_uv[i],
							 a540_corner_hz[j]);
		}
	}

	/* Round up to a set point, then clip to the corner's floor/ceiling */
	for (i = 0; i < A540_CORNERS; i++)
		ol[i] = clamp(roundup(ol[i], A540_STEP_UV),
			      a540_floor_uv[i], ceil[i]);

	if (force) {
		for (i = 0; i < A540_CORNERS - 1; i++)
			ol[i] = ol[A540_CORNERS - 1];
		return;
	}

	for (i = 0; i <= A540_PB_MAX_FUSE_CORNER; i++) {
		int safe, low, high;

		if (!a540_pb_marker(init[i]))
			continue;

		for (j = i + 1; j <= A540_PB_MAX_FUSE_CORNER; j++)
			if (!a540_pb_marker(init[j]))
				break;
		safe = j;

		low = i ? a540_fmax_corner[i] : 0;
		high = a540_fmax_corner[i + 1] - 1;
		for (j = low; j <= high; j++)
			ol[j] = ol[a540_fmax_corner[safe]];

		dev_info(dev, "GFX partial binning: corners %d-%d use corner %d voltage\n",
			 low + 1, high + 1, a540_fmax_corner[safe] + 1);
	}
}

static int a540_read_cell(struct device *dev, const char *name, u32 *val)
{
	int ret = nvmem_cell_read_variable_le_u32(dev, name, val);

	if (ret)
		dev_warn(dev, "cannot read GFX fuse %s: %d\n", name, ret);

	return ret;
}

/**
 * a540_gfx_apply_open_loop() - replace the OPP voltages with this part's
 *	fused open-loop voltages
 * @dev: the GPU device, with its OPP table populated
 *
 * Does nothing unless the GPU scales VDD_GFX itself (vdd-supply) and DT
 * provides the GFX CPR fuse cells. If a fuse cannot be read the DT voltages
 * are left alone.
 */
void a540_gfx_apply_open_loop(struct device *dev)
{
	u32 rev, limit, force, init_raw, init[A540_FUSE_CORNERS];
	int ol[A540_CORNERS];
	int i, ret;

	if (!device_property_present(dev, "vdd-supply") ||
	    of_property_match_string(dev->of_node, "nvmem-cell-names",
				     "gfx_cpr_init") < 0)
		return;

	if (a540_read_cell(dev, "gfx_cpr_init", &init_raw) ||
	    a540_read_cell(dev, "gfx_cpr_rev", &rev) ||
	    a540_read_cell(dev, "gfx_cpr_limit", &limit) ||
	    a540_read_cell(dev, "gfx_cpr_force", &force))
		return;

	/* Row 65 bits 39:43, 34:38, 29:33, 24:28 = fuse corners 0..3 */
	for (i = 0; i < A540_FUSE_CORNERS; i++)
		init[i] = (init_raw >> ((A540_FUSE_CORNERS - 1 - i) *
					A540_FUSE_BITS)) & GENMASK(4, 0);

	if (limit == 2) {
		/*
		 * MSM8996_CPR_LIMITATION_UNSUPPORTED: downstream refuses the
		 * rail. Keep the DT voltages rather than invent a table.
		 */
		dev_err(dev, "GFX CPR fuses mark this part unsupported, keeping DT voltages\n");
		return;
	}

	a540_open_loop(dev, rev, limit, force, init, ol);

	dev_info(dev, "GFX open-loop mV %d %d %d %d %d %d %d %d (fusing rev %u, fuses %u/%u/%u/%u, limit %u, force %u)\n",
		 ol[0] / 1000, ol[1] / 1000, ol[2] / 1000, ol[3] / 1000,
		 ol[4] / 1000, ol[5] / 1000, ol[6] / 1000, ol[7] / 1000,
		 rev, init[0], init[1], init[2], init[3], limit, force);

	for (i = 0; i < A540_CORNERS; i++) {
		unsigned long hz = a540_corner_hz[i];
		struct dev_pm_opp *opp;

		/* The OPP rates carry the clock's exact rate, e.g. 710000097 */
		opp = dev_pm_opp_find_freq_ceil(dev, &hz);
		if (IS_ERR(opp))
			continue;
		dev_pm_opp_put(opp);
		if (hz - a540_corner_hz[i] >= 1000000)
			continue;

		ret = dev_pm_opp_adjust_voltage(dev, hz, ol[i], ol[i], ol[i]);
		if (ret)
			dev_warn(dev, "cannot set %lu Hz to %d uV: %d\n",
				 hz, ol[i], ret);
	}
}

static struct regmap *a540_mem_acc_map;
static unsigned int a540_mem_acc_reg;

static int a540_config_clks(struct device *dev, struct opp_table *opp_table,
			    struct dev_pm_opp *opp, void *data,
			    bool scaling_down)
{
	unsigned int acc = dev_pm_opp_get_freq(opp) <= A540_MEM_ACC_LOW_MAX_HZ;
	int ret;

	/* Scaling up: the rail is already up, switch ACC before the clock */
	if (!scaling_down)
		regmap_update_bits(a540_mem_acc_map, a540_mem_acc_reg, BIT(0), acc);

	ret = dev_pm_opp_config_clks_simple(dev, opp_table, opp, data,
					    scaling_down);
	if (ret)
		return ret;

	/* Scaling down: the rail has not dropped yet, switch ACC now */
	if (scaling_down)
		regmap_update_bits(a540_mem_acc_map, a540_mem_acc_reg, BIT(0), acc);

	return 0;
}

/**
 * a540_gfx_set_opp_config() - register the core clock with the OPP core,
 *	adding MEM-ACC sequencing when DT describes the select register
 * @dev: the GPU device
 *
 * Return: 0 if the core clock was registered, -ENOENT if DT has no
 * qcom,mem-acc (the caller registers the clock the usual way), or an error.
 */
int a540_gfx_set_opp_config(struct device *dev)
{
	static const char * const clk_names[] = { "core", NULL };
	struct dev_pm_opp_config config = {
		.clk_names = clk_names,
		.config_clks = a540_config_clks,
	};
	struct regmap *map;
	unsigned int reg;
	int ret;

	map = syscon_regmap_lookup_by_phandle_args(dev->of_node, "qcom,mem-acc",
						   1, &reg);
	if (IS_ERR(map))
		return PTR_ERR(map);	/* -ENOENT: no qcom,mem-acc in DT */

	a540_mem_acc_map = map;
	a540_mem_acc_reg = reg;

	ret = devm_pm_opp_set_config(dev, &config);
	if (ret < 0)
		return ret;

	return 0;
}
