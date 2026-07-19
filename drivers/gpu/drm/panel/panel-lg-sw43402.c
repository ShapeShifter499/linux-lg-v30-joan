// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG SW43402 DSC command-mode DSI panel driver
 *
 * Used on the LG V30 (joan). Panel data derived from the downstream
 * LineageOS msm8998 tree (dsi-panel-sw43402-dsc-qhd-cmd-dv3_1.dtsi,
 * GPL-2.0). Structure adapted from panel-lg-sw43408.c (Linaro Ltd),
 * the closest in-tree sibling (same LG SW434xx family, DSC 1.1).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

static const struct regulator_bulk_data sw43402_supplies[] = {
	{ .supply = "vddio" },	/* 1.8 V, board gpio-enabled */
	{ .supply = "vpnl" },	/* panel rail, board gpio-enabled */
};

struct sw43402_panel {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
	struct drm_dsc_config dsc;
};

static inline struct sw43402_panel *to_sw43402(struct drm_panel *panel)
{
	return container_of(panel, struct sw43402_panel, base);
}

static void sw43402_reset(struct sw43402_panel *ctx)
{
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(9000, 10000);
	gpiod_set_value(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value(ctx->reset_gpio, 0);
	usleep_range(9000, 10000);
}

static int sw43402_prepare(struct drm_panel *panel)
{
	struct sw43402_panel *ctx = to_sw43402(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->link };
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(sw43402_supplies),
				    ctx->supplies);
	if (ret < 0)
		return ret;

	sw43402_reset(ctx);

	/* Downstream dsi_lp_mode on-command sequence (DV3.1) */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x20, 0x43);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x3d, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x03, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_TEAR_ON);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 60);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x55, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0xa5, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb2,
				     0x5d, 0x41, 0x04, 0x8c, 0x00, 0xff, 0xff,
				     0x15, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8, 0x08, 0x90, 0x10, 0x25);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd4,
				     0x10, 0x00, 0xff, 0x60, 0x30, 0x40, 0x50,
				     0x20, 0x20, 0x20, 0x20, 0xa0, 0x00, 0x20,
				     0x00, 0x34, 0xa0, 0x08, 0xda, 0xda, 0x4a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x03, 0x77);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x13, 0x00, 0x07, 0x00, 0x13);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe2,
				     0x20, 0x0d, 0x08, 0xa8, 0x0a, 0xaa, 0x04,
				     0xa4, 0x80, 0x80, 0x80, 0x5c, 0x5c, 0x5c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7,
				     0x00, 0x0d, 0x76, 0x1f, 0x00, 0x0d, 0x4a,
				     0x44, 0x0d, 0x76, 0x25, 0x00, 0x0d, 0x0d,
				     0x0d, 0x0d, 0x4a, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xce,
				     0x81, 0x1f, 0x0f, 0x01, 0x24, 0x68, 0x22,
				     0x20, 0x04, 0x01, 0x00, 0x80, 0xff, 0x88,
				     0x08, 0x02, 0x00, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 90);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7,
				     0x00, 0x0d, 0x76, 0x1f, 0x00, 0x0d, 0x0d,
				     0x44, 0x0d, 0x76, 0x25, 0x00, 0x0d, 0x0d,
				     0x0d, 0x0d, 0x4a, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 70);

	/* Deliver the PPS out of LPM, then re-enable LPM (as sw43408) */
	ctx->link->mode_flags &= ~MIPI_DSI_MODE_LPM;
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	ctx->link->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, true,
					    MIPI_DSI_COMPRESSION_DSC, 0);

	/* post-panel-on: display on */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0xa5, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 60);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	/*
	 * The downstream on-command sequence programs WRCTRLD (53h) with
	 * LG-custom bits (0x07) that leave the standard brightness gate
	 * (BCTRL) clear, and WRDISBV (51h) with the blmap floor value 3;
	 * with BCTRL clear the panel gates DBV writes (any width reads
	 * back 0 via 52h) and emits nothing visible. Stock relies on
	 * Android raising brightness through bl_ctrl_dcs right after
	 * boot. Enable the brightness-control block and go to full scale
	 * so the panel is usable without userspace; a proper backlight
	 * device can replace the hardcoded value later.
	 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				     0xff);

	if (dsi_ctx.accum_err)
		regulator_bulk_disable(ARRAY_SIZE(sw43402_supplies),
				       ctx->supplies);

	return dsi_ctx.accum_err;
}

static int sw43402_unprepare(struct drm_panel *panel)
{
	struct sw43402_panel *ctx = to_sw43402(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->link };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	gpiod_set_value(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(sw43402_supplies), ctx->supplies);

	return 0;
}

/* 1440x2880 command mode, 60 Hz; porches per downstream DV3.1 */
static const struct drm_display_mode sw43402_mode = {
	.clock = (1440 + 92 + 48 + 32) * (2880 + 10 + 25 + 1) * 60 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 92,
	.hsync_end = 1440 + 92 + 32,
	.htotal = 1440 + 92 + 48 + 32,
	.vdisplay = 2880,
	.vsync_start = 2880 + 10,
	.vsync_end = 2880 + 10 + 1,
	.vtotal = 2880 + 10 + 25 + 1,
	.width_mm = 68,
	.height_mm = 136,
};

static int sw43402_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &sw43402_mode);
}

static const struct drm_panel_funcs sw43402_funcs = {
	.prepare = sw43402_prepare,
	.unprepare = sw43402_unprepare,
	.get_modes = sw43402_get_modes,
};

static int sw43402_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct sw43402_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, __typeof(*ctx), base,
				   &sw43402_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(sw43402_supplies),
					    sw43402_supplies, &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->link = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	/* DSC 1.1, config3: 720x16 slices, 2 slices, 8bpc/8bpp */
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 16;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;
	dsi->dsc = &ctx->dsc;

	ctx->base.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->base);
	if (ret)
		return ret;

	drm_panel_add(&ctx->base);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->base);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void sw43402_remove(struct mipi_dsi_device *dsi)
{
	struct sw43402_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->base);
}

static const struct of_device_id sw43402_of_match[] = {
	{ .compatible = "lg,sw43402" },
	{ }
};
MODULE_DEVICE_TABLE(of, sw43402_of_match);

static struct mipi_dsi_driver sw43402_driver = {
	.driver = {
		.name = "panel-lg-sw43402",
		.of_match_table = sw43402_of_match,
	},
	.probe = sw43402_probe,
	.remove = sw43402_remove,
};
module_mipi_dsi_driver(sw43402_driver);

MODULE_DESCRIPTION("LG SW43402 DSC command-mode DSI panel driver");
MODULE_LICENSE("GPL");
