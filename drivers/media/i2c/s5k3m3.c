// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung S5K3M3 13MP image sensor driver
 *
 * Based on the Samsung S5K3M5 driver (s5k3m5.c), Copyright (c) 2025 Linaro
 * Ltd, Vladimir Zapolskiy: the S5K3M3 is an older sibling of the S5K3M5 with
 * the same CCS-like register map, streaming/flip register and Bayer layout.
 *
 * The register settings are LG's, recovered from the camera sensor library
 * (libmmcamera_s5k3m3.so) of the LG V30's stock US998 30b firmware: the init
 * list and the mode lists, as LG ships them.  Line and frame lengths, the
 * output pixel clocks, the exposure and gain registers, the gain encoding,
 * the test pattern values and the Bayer order come from the same library and
 * agree with the PLL settings in the lists.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5K3M3_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K3M3_CHIP_ID			0x30d3

/* Mode select (high byte) and image orientation (low byte) in one word */
#define S5K3M3_REG_CTRL_MODE		CCI_REG16(0x0100)
#define S5K3M3_MODE_STREAMING		BIT(8)
#define S5K3M3_VFLIP			BIT(1)
#define S5K3M3_HFLIP			BIT(0)

#define S5K3M3_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K3M3_EXPOSURE_MIN		8
#define S5K3M3_EXPOSURE_MARGIN		8
#define S5K3M3_EXPOSURE_DEFAULT		256

/* Analogue gain is linear, 0x20 = 1x, up to 16x */
#define S5K3M3_REG_ANALOG_GAIN		CCI_REG16(0x0204)
#define S5K3M3_ANA_GAIN_MIN		0x0020
#define S5K3M3_ANA_GAIN_MAX		0x0200
#define S5K3M3_ANA_GAIN_DEFAULT		0x0020

#define S5K3M3_REG_FRAME_LENGTH		CCI_REG16(0x0340)
#define S5K3M3_FRAME_LENGTH_MAX		0xffff

#define S5K3M3_REG_TEST_PATTERN		CCI_REG16(0x0600)

/* Software reset; LG waits 3 ms after it */
#define S5K3M3_REG_SW_RESET		CCI_REG16(0x6010)
#define S5K3M3_REG_PAGE			CCI_REG16(0xfcfc)
#define S5K3M3_PAGE_REGS		0x4000

#define S5K3M3_XCLK_FREQ		(24 * HZ_PER_MHZ)
#define S5K3M3_DATA_LANES		4

/*
 * Video timing pixel rate: 24 MHz / 6 * 120 (0x0304, 0x0306) = 480 MHz in
 * every mode.  With LG's 4880-pixel lines and 3278-line frames that is the
 * 30.01 fps the library quotes.
 */
#define S5K3M3_PIXEL_RATE		(480 * HZ_PER_MHZ)

/*
 * The pixel array is at least as large as the area LG's windows address:
 * columns 0..0x105f and rows 0..0xc37.  LG's full-resolution window, the
 * 4160x3120 active area, starts at (32, 8).
 */
#define S5K3M3_NATIVE_WIDTH		4192
#define S5K3M3_NATIVE_HEIGHT		3128
#define S5K3M3_ACTIVE_LEFT		32
#define S5K3M3_ACTIVE_TOP		8
#define S5K3M3_ACTIVE_WIDTH		4160
#define S5K3M3_ACTIVE_HEIGHT		3120

enum s5k3m3_link_freq {
	S5K3M3_LINK_FREQ_594MHZ,
	S5K3M3_LINK_FREQ_306MHZ,
};

/* 24 MHz / 4 * 99 and 24 MHz / 4 * 51 (0x030c, 0x030e) */
static const s64 s5k3m3_link_freqs[] = {
	[S5K3M3_LINK_FREQ_594MHZ] = 594000000,
	[S5K3M3_LINK_FREQ_306MHZ] = 306000000,
};

/* In LG's power-up order; powered down in reverse */
static const char * const s5k3m3_supply_names[] = {
	"vddd",		/* Digital core */
	"vdda",		/* Analogue */
	"vddio",	/* Digital I/O */
};

#define S5K3M3_NUM_SUPPLIES		ARRAY_SIZE(s5k3m3_supply_names)

/* LG's values for 0x0600 are the menu indices */
static const char * const s5k3m3_test_pattern_menu[] = {
	"Disabled",
	"Solid colour",
	"Colour bars",
	"Fade to grey colour bars",
	"PN9",
};

/*
 * Media bus codes by flip: none, horizontal, vertical, both.  LG streams
 * with both flips set (0x0100 = 0x0103) and declares GBRG for that.
 */
static const u32 s5k3m3_mbus_codes[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
};

struct s5k3m3_mode {
	u32 width;
	u32 height;
	struct v4l2_rect crop;
	u32 line_length;	/* in pixels */
	u32 frame_length;	/* default, in lines */
	u32 frame_length_min;	/* shortest LG runs this mode with */
	enum s5k3m3_link_freq link_freq;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

/*
 * LG's init list (library offset 0xe278).  It opens with a software reset
 * (fcfc = 4000, 6010 = 0001, then 3 ms), which the driver issues itself.
 */
static const struct cci_reg_sequence s5k3m3_init[] = {
	{ S5K3M3_REG_PAGE, S5K3M3_PAGE_REGS },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0300), 0x0004 },
	{ CCI_REG16(0x0302), 0x0001 },
	{ CCI_REG16(0x0304), 0x0006 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x030a), 0x0001 },
	{ CCI_REG16(0x030c), 0x0004 },
	{ CCI_REG16(0x3644), 0x0060 },
	{ CCI_REG16(0x0216), 0x0000 },
	{ CCI_REG16(0xf496), 0x0048 },
	{ CCI_REG16(0x1118), 0x43c8 },
	{ CCI_REG16(0x1124), 0x43c8 },
	{ CCI_REG16(0x3058), 0x0000 },
};

/* 4160x3120, 30 fps, full resolution (library offset 0x603dc) */
static const struct cci_reg_sequence s5k3m3_mode_4160x3120[] = {
	{ S5K3M3_REG_PAGE, S5K3M3_PAGE_REGS },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x0306), 0x0078 },
	{ CCI_REG16(0x030e), 0x0063 },
	{ CCI_REG16(0x0344), 0x0020 },
	{ CCI_REG16(0x0346), 0x0008 },
	{ CCI_REG16(0x0348), 0x105f },
	{ CCI_REG16(0x034a), 0x0c37 },
	{ CCI_REG16(0x034c), 0x1040 },
	{ CCI_REG16(0x034e), 0x0c30 },
	{ CCI_REG16(0x0340), 0x0cce },
	{ CCI_REG16(0x0342), 0x1310 },
	{ CCI_REG16(0x3000), 0x0001 },
	{ CCI_REG16(0xf440), 0x002f },
	{ CCI_REG16(0xf494), 0x0030 },
	{ CCI_REG16(0x3604), 0x0000 },
	{ CCI_REG16(0xf4cc), 0x0029 },
	{ CCI_REG16(0xf4ce), 0x002c },
	{ CCI_REG16(0xf4d0), 0x0034 },
	{ CCI_REG16(0xf4d2), 0x0035 },
	{ CCI_REG16(0xf4d4), 0x0038 },
	{ CCI_REG16(0xf4d6), 0x0039 },
	{ CCI_REG16(0xf4d8), 0x0034 },
	{ CCI_REG16(0xf4da), 0x0035 },
	{ CCI_REG16(0xf4dc), 0x0038 },
	{ CCI_REG16(0xf4de), 0x0039 },
	{ CCI_REG16(0x0200), 0x0618 },
	{ CCI_REG16(0x0202), 0x0d27 },
	{ CCI_REG16(0x021e), 0x0400 },
	{ CCI_REG16(0x021c), 0x0000 },
	{ CCI_REG16(0x0900), 0x0011 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0001 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0001 },
	{ CCI_REG16(0x0400), 0x0000 },
	{ CCI_REG16(0x0404), 0x0010 },
	{ CCI_REG16(0x309c), 0x0001 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0216), 0x0000 },
};

/* 4160x2340, 30 fps, 16:9 crop (library offset 0x661ac) */
static const struct cci_reg_sequence s5k3m3_mode_4160x2340[] = {
	{ S5K3M3_REG_PAGE, S5K3M3_PAGE_REGS },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x0306), 0x0078 },
	{ CCI_REG16(0x030e), 0x0063 },
	{ CCI_REG16(0x0344), 0x0020 },
	{ CCI_REG16(0x0346), 0x0190 },
	{ CCI_REG16(0x0348), 0x105f },
	{ CCI_REG16(0x034a), 0x0ab7 },
	{ CCI_REG16(0x034c), 0x1040 },
	{ CCI_REG16(0x034e), 0x0924 },
	{ CCI_REG16(0x0340), 0x0cce },
	{ CCI_REG16(0x0342), 0x1310 },
	{ CCI_REG16(0x3000), 0x0001 },
	{ CCI_REG16(0xf440), 0x002f },
	{ CCI_REG16(0xf494), 0x0030 },
	{ CCI_REG16(0x3604), 0x0000 },
	{ CCI_REG16(0xf4cc), 0x0029 },
	{ CCI_REG16(0xf4ce), 0x002c },
	{ CCI_REG16(0xf4d0), 0x0034 },
	{ CCI_REG16(0xf4d2), 0x0035 },
	{ CCI_REG16(0xf4d4), 0x0038 },
	{ CCI_REG16(0xf4d6), 0x0039 },
	{ CCI_REG16(0xf4d8), 0x0034 },
	{ CCI_REG16(0xf4da), 0x0035 },
	{ CCI_REG16(0xf4dc), 0x0038 },
	{ CCI_REG16(0xf4de), 0x0039 },
	{ CCI_REG16(0x0200), 0x0618 },
	{ CCI_REG16(0x0202), 0x0d27 },
	{ CCI_REG16(0x021e), 0x0400 },
	{ CCI_REG16(0x021c), 0x0000 },
	{ CCI_REG16(0x0900), 0x0011 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0001 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0001 },
	{ CCI_REG16(0x0400), 0x0000 },
	{ CCI_REG16(0x0404), 0x0010 },
	{ CCI_REG16(0x309c), 0x0001 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0216), 0x0000 },
};

/*
 * 2080x1560, 30 fps: vertical 2x binning and horizontal 1/2 scaling of the
 * full window (library offset 0x6bf7c)
 */
static const struct cci_reg_sequence s5k3m3_mode_2080x1560[] = {
	{ S5K3M3_REG_PAGE, S5K3M3_PAGE_REGS },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x0306), 0x0078 },
	{ CCI_REG16(0x030e), 0x0033 },
	{ CCI_REG16(0x0344), 0x0020 },
	{ CCI_REG16(0x0346), 0x0008 },
	{ CCI_REG16(0x0348), 0x105f },
	{ CCI_REG16(0x034a), 0x0c37 },
	{ CCI_REG16(0x034c), 0x0820 },
	{ CCI_REG16(0x034e), 0x0618 },
	{ CCI_REG16(0x0340), 0x0cce },
	{ CCI_REG16(0x0342), 0x1310 },
	{ CCI_REG16(0x3000), 0x0001 },
	{ CCI_REG16(0xf440), 0x002f },
	{ CCI_REG16(0xf494), 0x0030 },
	{ CCI_REG16(0x3604), 0x0000 },
	{ CCI_REG16(0xf4cc), 0x0029 },
	{ CCI_REG16(0xf4ce), 0x002c },
	{ CCI_REG16(0xf4d0), 0x0034 },
	{ CCI_REG16(0xf4d2), 0x0035 },
	{ CCI_REG16(0xf4d4), 0x0038 },
	{ CCI_REG16(0xf4d6), 0x0039 },
	{ CCI_REG16(0xf4d8), 0x0034 },
	{ CCI_REG16(0xf4da), 0x0035 },
	{ CCI_REG16(0xf4dc), 0x0038 },
	{ CCI_REG16(0xf4de), 0x0039 },
	{ CCI_REG16(0x0200), 0x0618 },
	{ CCI_REG16(0x0202), 0x0d27 },
	{ CCI_REG16(0x021e), 0x0400 },
	{ CCI_REG16(0x021c), 0x0000 },
	{ CCI_REG16(0x0900), 0x0112 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0001 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0003 },
	{ CCI_REG16(0x0400), 0x0001 },
	{ CCI_REG16(0x0404), 0x0020 },
	{ CCI_REG16(0x309c), 0x0001 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0216), 0x0000 },
};

/*
 * 2080x1172, the same reduction of the 16:9 window.  LG has it at 30 fps
 * (library offset 0x71d4c) and at 60 fps (0x77b1c); the two lists differ
 * only in the frame length (0x0cce and 0x0667), so this is the 30 fps list
 * and vertical blanking reaches the 60 fps one.
 */
static const struct cci_reg_sequence s5k3m3_mode_2080x1172[] = {
	{ S5K3M3_REG_PAGE, S5K3M3_PAGE_REGS },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x0306), 0x0078 },
	{ CCI_REG16(0x030e), 0x0033 },
	{ CCI_REG16(0x0344), 0x0020 },
	{ CCI_REG16(0x0346), 0x0190 },
	{ CCI_REG16(0x0348), 0x105f },
	{ CCI_REG16(0x034a), 0x0ab7 },
	{ CCI_REG16(0x034c), 0x0820 },
	{ CCI_REG16(0x034e), 0x0494 },
	{ CCI_REG16(0x0340), 0x0cce },
	{ CCI_REG16(0x0342), 0x1310 },
	{ CCI_REG16(0x3000), 0x0001 },
	{ CCI_REG16(0xf440), 0x002f },
	{ CCI_REG16(0xf494), 0x0030 },
	{ CCI_REG16(0x3604), 0x0000 },
	{ CCI_REG16(0xf4cc), 0x0029 },
	{ CCI_REG16(0xf4ce), 0x002c },
	{ CCI_REG16(0xf4d0), 0x0034 },
	{ CCI_REG16(0xf4d2), 0x0035 },
	{ CCI_REG16(0xf4d4), 0x0038 },
	{ CCI_REG16(0xf4d6), 0x0039 },
	{ CCI_REG16(0xf4d8), 0x0034 },
	{ CCI_REG16(0xf4da), 0x0035 },
	{ CCI_REG16(0xf4dc), 0x0038 },
	{ CCI_REG16(0xf4de), 0x0039 },
	{ CCI_REG16(0x0200), 0x0618 },
	{ CCI_REG16(0x0202), 0x0d27 },
	{ CCI_REG16(0x021e), 0x0400 },
	{ CCI_REG16(0x021c), 0x0000 },
	{ CCI_REG16(0x0900), 0x0112 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0001 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0003 },
	{ CCI_REG16(0x0400), 0x0001 },
	{ CCI_REG16(0x0404), 0x0020 },
	{ CCI_REG16(0x309c), 0x0001 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0216), 0x0000 },
};

/*
 * 1040x584, 120 fps: vertical 4x binning and horizontal 1/4 scaling of the
 * 16:9 window (library offset 0x7d8ec)
 */
static const struct cci_reg_sequence s5k3m3_mode_1040x584[] = {
	{ S5K3M3_REG_PAGE, S5K3M3_PAGE_REGS },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x0306), 0x0078 },
	{ CCI_REG16(0x030e), 0x0033 },
	{ CCI_REG16(0x0344), 0x0020 },
	{ CCI_REG16(0x0346), 0x0190 },
	{ CCI_REG16(0x0348), 0x105f },
	{ CCI_REG16(0x034a), 0x0aaf },
	{ CCI_REG16(0x034c), 0x0410 },
	{ CCI_REG16(0x034e), 0x0248 },
	{ CCI_REG16(0x0340), 0x0333 },
	{ CCI_REG16(0x0342), 0x1310 },
	{ CCI_REG16(0x3000), 0x0001 },
	{ CCI_REG16(0xf440), 0x002f },
	{ CCI_REG16(0xf494), 0x0030 },
	{ CCI_REG16(0x3604), 0x0000 },
	{ CCI_REG16(0xf4cc), 0x0029 },
	{ CCI_REG16(0xf4ce), 0x002c },
	{ CCI_REG16(0xf4d0), 0x0034 },
	{ CCI_REG16(0xf4d2), 0x0035 },
	{ CCI_REG16(0xf4d4), 0x0038 },
	{ CCI_REG16(0xf4d6), 0x0039 },
	{ CCI_REG16(0xf4d8), 0x0034 },
	{ CCI_REG16(0xf4da), 0x0035 },
	{ CCI_REG16(0xf4dc), 0x0038 },
	{ CCI_REG16(0xf4de), 0x0039 },
	{ CCI_REG16(0x0200), 0x0618 },
	{ CCI_REG16(0x0202), 0x0d27 },
	{ CCI_REG16(0x021e), 0x0400 },
	{ CCI_REG16(0x021c), 0x0000 },
	{ CCI_REG16(0x0900), 0x0114 },
	{ CCI_REG16(0x0380), 0x0001 },
	{ CCI_REG16(0x0382), 0x0001 },
	{ CCI_REG16(0x0384), 0x0001 },
	{ CCI_REG16(0x0386), 0x0007 },
	{ CCI_REG16(0x0400), 0x0001 },
	{ CCI_REG16(0x0404), 0x0040 },
	{ CCI_REG16(0x309c), 0x0001 },
	{ CCI_REG16(0x0114), 0x0300 },
	{ CCI_REG16(0x0216), 0x0000 },
};

/* Largest first: the first usable mode is the default one */
static const struct s5k3m3_mode s5k3m3_modes[] = {
	{
		.width = 4160,
		.height = 3120,
		.crop = {
			.left = S5K3M3_ACTIVE_LEFT,
			.top = 8,
			.width = S5K3M3_ACTIVE_WIDTH,
			.height = 3120,
		},
		.line_length = 4880,
		.frame_length = 3278,
		.frame_length_min = 3278,
		.link_freq = S5K3M3_LINK_FREQ_594MHZ,
		.regs = s5k3m3_mode_4160x3120,
		.num_regs = ARRAY_SIZE(s5k3m3_mode_4160x3120),
	},
	{
		.width = 4160,
		.height = 2340,
		.crop = {
			.left = S5K3M3_ACTIVE_LEFT,
			.top = 400,
			.width = S5K3M3_ACTIVE_WIDTH,
			.height = 2344,
		},
		.line_length = 4880,
		.frame_length = 3278,
		.frame_length_min = 3278,
		.link_freq = S5K3M3_LINK_FREQ_594MHZ,
		.regs = s5k3m3_mode_4160x2340,
		.num_regs = ARRAY_SIZE(s5k3m3_mode_4160x2340),
	},
	{
		.width = 2080,
		.height = 1560,
		.crop = {
			.left = S5K3M3_ACTIVE_LEFT,
			.top = 8,
			.width = S5K3M3_ACTIVE_WIDTH,
			.height = 3120,
		},
		.line_length = 4880,
		.frame_length = 3278,
		.frame_length_min = 3278,
		.link_freq = S5K3M3_LINK_FREQ_306MHZ,
		.regs = s5k3m3_mode_2080x1560,
		.num_regs = ARRAY_SIZE(s5k3m3_mode_2080x1560),
	},
	{
		.width = 2080,
		.height = 1172,
		.crop = {
			.left = S5K3M3_ACTIVE_LEFT,
			.top = 400,
			.width = S5K3M3_ACTIVE_WIDTH,
			.height = 2344,
		},
		.line_length = 4880,
		.frame_length = 3278,
		.frame_length_min = 1639,
		.link_freq = S5K3M3_LINK_FREQ_306MHZ,
		.regs = s5k3m3_mode_2080x1172,
		.num_regs = ARRAY_SIZE(s5k3m3_mode_2080x1172),
	},
	{
		.width = 1040,
		.height = 584,
		.crop = {
			.left = S5K3M3_ACTIVE_LEFT,
			.top = 400,
			.width = S5K3M3_ACTIVE_WIDTH,
			.height = 2336,
		},
		.line_length = 4880,
		.frame_length = 819,
		.frame_length_min = 819,
		.link_freq = S5K3M3_LINK_FREQ_306MHZ,
		.regs = s5k3m3_mode_1040x584,
		.num_regs = ARRAY_SIZE(s5k3m3_mode_1040x584),
	},
};

struct s5k3m3 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[S5K3M3_NUM_SUPPLIES];

	/* Link frequencies both the board and the driver support */
	unsigned long link_freq_bitmap;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
};

static inline struct s5k3m3 *to_s5k3m3(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k3m3, sd);
}

static bool s5k3m3_mode_usable(struct s5k3m3 *s5k3m3,
			       const struct s5k3m3_mode *mode)
{
	return s5k3m3->link_freq_bitmap & BIT(mode->link_freq);
}

static const struct s5k3m3_mode *s5k3m3_default_mode(struct s5k3m3 *s5k3m3)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s5k3m3_modes); i++)
		if (s5k3m3_mode_usable(s5k3m3, &s5k3m3_modes[i]))
			return &s5k3m3_modes[i];

	return NULL;
}

static const struct s5k3m3_mode *
s5k3m3_find_mode(struct s5k3m3 *s5k3m3, u32 width, u32 height)
{
	const struct s5k3m3_mode *best = NULL;
	u32 dist, best_dist = U32_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s5k3m3_modes); i++) {
		const struct s5k3m3_mode *mode = &s5k3m3_modes[i];

		if (!s5k3m3_mode_usable(s5k3m3, mode))
			continue;

		dist = abs((int)mode->width - (int)width) +
		       abs((int)mode->height - (int)height);
		if (dist < best_dist) {
			best_dist = dist;
			best = mode;
		}
	}

	return best;
}

static const struct s5k3m3_mode *
s5k3m3_state_mode(struct s5k3m3 *s5k3m3, struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *fmt =
		v4l2_subdev_state_get_format(state, 0);

	return s5k3m3_find_mode(s5k3m3, fmt->width, fmt->height);
}

static u32 s5k3m3_mbus_code(struct s5k3m3 *s5k3m3)
{
	unsigned int i = 0;

	if (s5k3m3->hflip && s5k3m3->hflip->val)
		i |= 1;
	if (s5k3m3->vflip && s5k3m3->vflip->val)
		i |= 2;

	return s5k3m3_mbus_codes[i];
}

static void s5k3m3_update_limits(struct s5k3m3 *s5k3m3,
				 const struct s5k3m3_mode *mode)
{
	u32 vblank_min = mode->frame_length_min - mode->height;
	u32 vblank_def = mode->frame_length - mode->height;
	u32 hblank = mode->line_length - mode->width;

	__v4l2_ctrl_s_ctrl(s5k3m3->link_freq, mode->link_freq);

	/* Never run a mode faster than LG does */
	__v4l2_ctrl_modify_range(s5k3m3->vblank, vblank_min,
				 S5K3M3_FRAME_LENGTH_MAX - mode->height,
				 1, vblank_def);
	__v4l2_ctrl_s_ctrl(s5k3m3->vblank, vblank_def);

	__v4l2_ctrl_modify_range(s5k3m3->hblank, hblank, hblank, 1, hblank);

	__v4l2_ctrl_modify_range(s5k3m3->exposure, S5K3M3_EXPOSURE_MIN,
				 mode->frame_length - S5K3M3_EXPOSURE_MARGIN,
				 1, S5K3M3_EXPOSURE_DEFAULT);
}

static int s5k3m3_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k3m3 *s5k3m3 = container_of(ctrl->handler, struct s5k3m3,
					     ctrls);
	struct v4l2_subdev_state *state =
		v4l2_subdev_get_locked_active_state(&s5k3m3->sd);
	const struct s5k3m3_mode *mode = s5k3m3_state_mode(s5k3m3, state);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK && mode) {
		u32 max = mode->height + ctrl->val - S5K3M3_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(s5k3m3->exposure,
					 s5k3m3->exposure->minimum, max,
					 s5k3m3->exposure->step,
					 min(s5k3m3->exposure->default_value,
					     (s64)max));
	}

	/* Only touch the hardware while it is powered */
	if (!pm_runtime_get_if_active(s5k3m3->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(s5k3m3->regmap, S5K3M3_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(s5k3m3->regmap, S5K3M3_REG_ANALOG_GAIN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_VBLANK:
		if (mode)
			cci_write(s5k3m3->regmap, S5K3M3_REG_FRAME_LENGTH,
				  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(s5k3m3->regmap, S5K3M3_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/* Written together with the streaming bit */
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(s5k3m3->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k3m3_ctrl_ops = {
	.s_ctrl = s5k3m3_set_ctrl,
};

static int s5k3m3_init_controls(struct s5k3m3 *s5k3m3,
				const struct s5k3m3_mode *mode)
{
	struct v4l2_ctrl_handler *hdl = &s5k3m3->ctrls;
	struct v4l2_fwnode_device_properties props;
	int ret;

	ret = v4l2_fwnode_device_parse(s5k3m3->dev, &props);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(hdl, 11);

	s5k3m3->pixel_rate = v4l2_ctrl_new_std(hdl, &s5k3m3_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       S5K3M3_PIXEL_RATE,
					       S5K3M3_PIXEL_RATE, 1,
					       S5K3M3_PIXEL_RATE);

	s5k3m3->link_freq = v4l2_ctrl_new_int_menu(hdl, &s5k3m3_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(s5k3m3_link_freqs) - 1,
						   mode->link_freq,
						   s5k3m3_link_freqs);

	s5k3m3->vblank = v4l2_ctrl_new_std(hdl, &s5k3m3_ctrl_ops,
					   V4L2_CID_VBLANK,
					   mode->frame_length_min - mode->height,
					   S5K3M3_FRAME_LENGTH_MAX - mode->height,
					   1, mode->frame_length - mode->height);

	s5k3m3->hblank = v4l2_ctrl_new_std(hdl, &s5k3m3_ctrl_ops,
					   V4L2_CID_HBLANK,
					   mode->line_length - mode->width,
					   mode->line_length - mode->width, 1,
					   mode->line_length - mode->width);

	s5k3m3->exposure = v4l2_ctrl_new_std(hdl, &s5k3m3_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5K3M3_EXPOSURE_MIN,
					     mode->frame_length -
					     S5K3M3_EXPOSURE_MARGIN,
					     1, S5K3M3_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(hdl, &s5k3m3_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K3M3_ANA_GAIN_MIN, S5K3M3_ANA_GAIN_MAX, 1,
			  S5K3M3_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std_menu_items(hdl, &s5k3m3_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k3m3_test_pattern_menu) - 1,
				     0, 0, s5k3m3_test_pattern_menu);

	s5k3m3->hflip = v4l2_ctrl_new_std(hdl, &s5k3m3_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	s5k3m3->vflip = v4l2_ctrl_new_std(hdl, &s5k3m3_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_fwnode_properties(hdl, &s5k3m3_ctrl_ops, &props);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	s5k3m3->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s5k3m3->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	s5k3m3->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	/* Flipping changes the Bayer order */
	s5k3m3->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	s5k3m3->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	s5k3m3->sd.ctrl_handler = hdl;

	return 0;
}

static int s5k3m3_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);
	const struct s5k3m3_mode *mode = s5k3m3_state_mode(s5k3m3, state);
	u64 ctrl_mode = S5K3M3_MODE_STREAMING;
	int ret = 0;

	if (!mode)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(s5k3m3->dev);
	if (ret)
		return ret;

	cci_write(s5k3m3->regmap, S5K3M3_REG_PAGE, S5K3M3_PAGE_REGS, &ret);
	cci_write(s5k3m3->regmap, S5K3M3_REG_SW_RESET, 0x0001, &ret);
	if (ret)
		goto err_put;

	usleep_range(3000, 4000);

	cci_multi_reg_write(s5k3m3->regmap, s5k3m3_init,
			    ARRAY_SIZE(s5k3m3_init), &ret);
	cci_multi_reg_write(s5k3m3->regmap, mode->regs, mode->num_regs, &ret);
	if (ret) {
		dev_err(s5k3m3->dev, "failed to program the sensor: %d\n", ret);
		goto err_put;
	}

	ret = __v4l2_ctrl_handler_setup(&s5k3m3->ctrls);
	if (ret)
		goto err_put;

	if (s5k3m3->hflip->val)
		ctrl_mode |= S5K3M3_HFLIP;
	if (s5k3m3->vflip->val)
		ctrl_mode |= S5K3M3_VFLIP;

	ret = cci_write(s5k3m3->regmap, S5K3M3_REG_CTRL_MODE, ctrl_mode, NULL);
	if (ret)
		goto err_put;

	/* The Bayer order must not change under a running stream */
	__v4l2_ctrl_grab(s5k3m3->hflip, true);
	__v4l2_ctrl_grab(s5k3m3->vflip, true);

	return 0;

err_put:
	pm_runtime_put(s5k3m3->dev);
	return ret;
}

static int s5k3m3_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);
	int ret;

	ret = cci_write(s5k3m3->regmap, S5K3M3_REG_CTRL_MODE, 0, NULL);

	__v4l2_ctrl_grab(s5k3m3->hflip, false);
	__v4l2_ctrl_grab(s5k3m3->vflip, false);

	pm_runtime_put(s5k3m3->dev);

	return ret;
}

static void s5k3m3_fill_format(struct s5k3m3 *s5k3m3,
			       struct v4l2_mbus_framefmt *fmt,
			       const struct s5k3m3_mode *mode)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = s5k3m3_mbus_code(s5k3m3);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k3m3_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = s5k3m3_mbus_code(to_s5k3m3(sd));

	return 0;
}

static int s5k3m3_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);
	unsigned int i, n = 0;

	if (fse->code != s5k3m3_mbus_code(s5k3m3))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(s5k3m3_modes); i++) {
		const struct s5k3m3_mode *mode = &s5k3m3_modes[i];

		if (!s5k3m3_mode_usable(s5k3m3, mode))
			continue;
		if (n++ != fse->index)
			continue;

		fse->min_width = mode->width;
		fse->max_width = mode->width;
		fse->min_height = mode->height;
		fse->max_height = mode->height;
		return 0;
	}

	return -EINVAL;
}

static int s5k3m3_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *format)
{
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);
	const struct s5k3m3_mode *mode;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	mode = s5k3m3_find_mode(s5k3m3, format->format.width,
				format->format.height);
	if (!mode)
		return -EINVAL;

	s5k3m3_fill_format(s5k3m3, &format->format, mode);
	*v4l2_subdev_state_get_format(state, 0) = format->format;
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		s5k3m3_update_limits(s5k3m3, mode);

	return 0;
}

static int s5k3m3_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = S5K3M3_NATIVE_WIDTH;
		sel->r.height = S5K3M3_NATIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = S5K3M3_ACTIVE_LEFT;
		sel->r.top = S5K3M3_ACTIVE_TOP;
		sel->r.width = S5K3M3_ACTIVE_WIDTH;
		sel->r.height = S5K3M3_ACTIVE_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int s5k3m3_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);
	const struct s5k3m3_mode *mode = s5k3m3_default_mode(s5k3m3);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.width = mode->width,
			.height = mode->height,
		},
	};

	return s5k3m3_set_format(sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops s5k3m3_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k3m3_pad_ops = {
	.enum_mbus_code = s5k3m3_enum_mbus_code,
	.enum_frame_size = s5k3m3_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = s5k3m3_set_format,
	.get_selection = s5k3m3_get_selection,
	.enable_streams = s5k3m3_enable_streams,
	.disable_streams = s5k3m3_disable_streams,
};

static const struct v4l2_subdev_ops s5k3m3_subdev_ops = {
	.video = &s5k3m3_video_ops,
	.pad = &s5k3m3_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k3m3_internal_ops = {
	.init_state = s5k3m3_init_state,
};

static const struct media_entity_operations s5k3m3_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

/*
 * LG's power-up, from the library's power setting table: digital core and
 * analogue rails (both GPIO-switched LDOs on the V30), the I/O rail, 1 ms,
 * reset released, 1 ms, MCLK, 2 ms.  Power-down is the reverse, with 1 ms
 * after stopping the clock and after asserting reset.
 */
static int s5k3m3_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);
	unsigned int i;
	int ret;

	for (i = 0; i < S5K3M3_NUM_SUPPLIES; i++) {
		ret = regulator_enable(s5k3m3->supplies[i].consumer);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				s5k3m3->supplies[i].supply, ret);
			goto err_regulators;
		}
	}

	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(s5k3m3->reset_gpio, 0);
	usleep_range(1000, 2000);

	ret = clk_prepare_enable(s5k3m3->xclk);
	if (ret) {
		dev_err(dev, "failed to enable clock: %d\n", ret);
		goto err_reset;
	}

	usleep_range(2000, 3000);

	return 0;

err_reset:
	gpiod_set_value_cansleep(s5k3m3->reset_gpio, 1);
err_regulators:
	while (i--)
		regulator_disable(s5k3m3->supplies[i].consumer);
	return ret;
}

static int s5k3m3_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);
	unsigned int i;

	clk_disable_unprepare(s5k3m3->xclk);
	usleep_range(1000, 2000);

	gpiod_set_value_cansleep(s5k3m3->reset_gpio, 1);
	usleep_range(1000, 2000);

	for (i = S5K3M3_NUM_SUPPLIES; i--; )
		regulator_disable(s5k3m3->supplies[i].consumer);

	return 0;
}

static int s5k3m3_parse_endpoint(struct s5k3m3 *s5k3m3)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(s5k3m3->dev), NULL);
	if (!ep)
		return dev_err_probe(s5k3m3->dev, -ENXIO, "no endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(s5k3m3->dev, ret,
				     "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K3M3_DATA_LANES) {
		ret = dev_err_probe(s5k3m3->dev, -EINVAL,
				    "only %u data lanes are supported\n",
				    S5K3M3_DATA_LANES);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(s5k3m3->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k3m3_link_freqs,
				       ARRAY_SIZE(s5k3m3_link_freqs),
				       &s5k3m3->link_freq_bitmap);

out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int s5k3m3_identify(struct s5k3m3 *s5k3m3)
{
	u64 id;
	int ret;

	ret = cci_read(s5k3m3->regmap, S5K3M3_REG_CHIP_ID, &id, NULL);
	if (ret)
		return dev_err_probe(s5k3m3->dev, ret,
				     "failed to read the chip ID\n");

	if (id != S5K3M3_CHIP_ID)
		return dev_err_probe(s5k3m3->dev, -ENODEV,
				     "unexpected chip ID 0x%04llx\n", id);

	return 0;
}

static int s5k3m3_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	const struct s5k3m3_mode *mode;
	struct s5k3m3 *s5k3m3;
	unsigned int i;
	int ret;

	s5k3m3 = devm_kzalloc(dev, sizeof(*s5k3m3), GFP_KERNEL);
	if (!s5k3m3)
		return -ENOMEM;

	s5k3m3->dev = dev;

	ret = s5k3m3_parse_endpoint(s5k3m3);
	if (ret)
		return ret;

	mode = s5k3m3_default_mode(s5k3m3);
	if (!mode)
		return dev_err_probe(dev, -EINVAL, "no usable link frequency\n");

	s5k3m3->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k3m3->regmap))
		return dev_err_probe(dev, PTR_ERR(s5k3m3->regmap),
				     "failed to initialize CCI\n");

	s5k3m3->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(s5k3m3->xclk))
		return dev_err_probe(dev, PTR_ERR(s5k3m3->xclk),
				     "failed to get the clock\n");

	if (clk_get_rate(s5k3m3->xclk) != S5K3M3_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "the clock must run at %lu Hz\n",
				     S5K3M3_XCLK_FREQ);

	for (i = 0; i < S5K3M3_NUM_SUPPLIES; i++)
		s5k3m3->supplies[i].supply = s5k3m3_supply_names[i];

	ret = devm_regulator_bulk_get(dev, S5K3M3_NUM_SUPPLIES,
				      s5k3m3->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	s5k3m3->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(s5k3m3->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(s5k3m3->reset_gpio),
				     "failed to get the reset GPIO\n");

	v4l2_i2c_subdev_init(&s5k3m3->sd, client, &s5k3m3_subdev_ops);
	s5k3m3->sd.internal_ops = &s5k3m3_internal_ops;

	ret = s5k3m3_power_on(dev);
	if (ret)
		return ret;

	ret = s5k3m3_identify(s5k3m3);
	if (ret)
		goto err_power_off;

	ret = s5k3m3_init_controls(s5k3m3, mode);
	if (ret)
		goto err_power_off;

	s5k3m3->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5k3m3->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s5k3m3->sd.entity.ops = &s5k3m3_entity_ops;
	s5k3m3->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s5k3m3->sd.entity, 1, &s5k3m3->pad);
	if (ret)
		goto err_ctrls;

	s5k3m3->sd.state_lock = s5k3m3->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&s5k3m3->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(dev);
	/*
	 * No autosuspend, as in the IMX351 and HI553 drivers for the same
	 * phone: the sensor powers down as soon as its stream stops.  Kept
	 * powered after a stream, sensor and CCI suspend together while the
	 * next stream is being set up, and on the LG V30 that overlap hangs
	 * the SoC.
	 */
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&s5k3m3->sd);
	if (ret)
		goto err_pm;

	pm_runtime_idle(dev);

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&s5k3m3->sd);
err_entity:
	media_entity_cleanup(&s5k3m3->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&s5k3m3->ctrls);
err_power_off:
	s5k3m3_power_off(dev);
	return ret;
}

static void s5k3m3_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k3m3 *s5k3m3 = to_s5k3m3(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s5k3m3->ctrls);

	pm_runtime_disable(s5k3m3->dev);
	if (!pm_runtime_status_suspended(s5k3m3->dev))
		s5k3m3_power_off(s5k3m3->dev);
	pm_runtime_set_suspended(s5k3m3->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(s5k3m3_pm_ops, s5k3m3_power_off,
				 s5k3m3_power_on, NULL);

static const struct of_device_id s5k3m3_of_match[] = {
	{ .compatible = "samsung,s5k3m3" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5k3m3_of_match);

static struct i2c_driver s5k3m3_i2c_driver = {
	.driver = {
		.name = "s5k3m3",
		.of_match_table = s5k3m3_of_match,
		.pm = pm_ptr(&s5k3m3_pm_ops),
	},
	.probe = s5k3m3_probe,
	.remove = s5k3m3_remove,
};
module_i2c_driver(s5k3m3_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K3M3 image sensor driver");
MODULE_LICENSE("GPL");
