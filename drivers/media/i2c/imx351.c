// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sony IMX351 16MP image sensor driver
 *
 * The register settings are LG's, recovered from the camera sensor library
 * (libmmcamera_imx351.so) of the LG V30's stock US998 30b firmware: the two
 * init lists and the full-resolution and 2x2 binned mode lists, as LG ships
 * them.  Timings (line and frame lengths, pixel and link rates) come from
 * the same library's output table and agree with the PLL settings.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define IMX351_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX351_CHIP_ID			0x0351

#define IMX351_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX351_MODE_STANDBY		0x00
#define IMX351_MODE_STREAMING		0x01

#define IMX351_REG_ORIENTATION		CCI_REG8(0x0101)
#define IMX351_REG_HOLD			CCI_REG8(0x0104)

#define IMX351_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX351_EXPOSURE_MIN		1
#define IMX351_EXPOSURE_OFFSET		20
#define IMX351_EXPOSURE_DEFAULT		1000

#define IMX351_REG_ANALOG_GAIN		CCI_REG16(0x0204)
#define IMX351_ANA_GAIN_MIN		0
#define IMX351_ANA_GAIN_MAX		960
#define IMX351_ANA_GAIN_DEFAULT		0

#define IMX351_REG_DIG_GAIN_GR		CCI_REG16(0x020e)
#define IMX351_REG_DIG_GAIN_R		CCI_REG16(0x0210)
#define IMX351_REG_DIG_GAIN_B		CCI_REG16(0x0212)
#define IMX351_REG_DIG_GAIN_GB		CCI_REG16(0x0214)
#define IMX351_DGTL_GAIN_MIN		0x0100
#define IMX351_DGTL_GAIN_MAX		0x0fff
#define IMX351_DGTL_GAIN_DEFAULT	0x0100

#define IMX351_REG_FRAME_LENGTH		CCI_REG16(0x0340)
#define IMX351_FRAME_LENGTH_MAX		0xffff

#define IMX351_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define IMX351_XCLK_FREQ		24000000

#define IMX351_NATIVE_WIDTH		4656
#define IMX351_NATIVE_HEIGHT		3496

enum imx351_link_freq {
	IMX351_LINK_FREQ_660MHZ,
	IMX351_LINK_FREQ_330MHZ,
};

static const s64 imx351_link_freqs[] = {
	[IMX351_LINK_FREQ_660MHZ] = 660000000,
	[IMX351_LINK_FREQ_330MHZ] = 330000000,
};

static const char * const imx351_supply_names[] = {
	"dovdd",
	"avdd",
	"dvdd",
};

static const char * const imx351_test_pattern_menu[] = {
	"Disabled",
	"Solid Colour",
	"Eight Vertical Colour Bars",
	"Colour Bars With Fade to Grey",
	"Pseudorandom Sequence (PN9)",
};

/* Media bus codes by flip: none, horizontal, vertical, both */
static const u32 imx351_mbus_codes[] = {
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,
};

struct imx351_mode {
	u32 width;
	u32 height;
	struct v4l2_rect crop;
	u32 line_length;	/* in pixels */
	u32 frame_length;	/* default, in lines */
	u64 pixel_rate;
	enum imx351_link_freq link_freq;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

/* LG's register lists, in the order its mm-camera writes them */
static const struct cci_reg_sequence imx351_init_common[] = {
	{ CCI_REG8(0x0136), 0x18 },
	{ CCI_REG8(0x0137), 0x00 },
	{ CCI_REG8(0x0138), 0x01 },
	{ CCI_REG8(0x3c7d), 0x28 },
	{ CCI_REG8(0x3c7e), 0x02 },
	{ CCI_REG8(0x3c7f), 0x0b },
	{ CCI_REG8(0x3140), 0x02 },
	{ CCI_REG8(0x3f7f), 0x01 },
	{ CCI_REG8(0x4430), 0x05 },
	{ CCI_REG8(0x4431), 0xdc },
	{ CCI_REG8(0x4ed0), 0x01 },
	{ CCI_REG8(0x4ed1), 0x3e },
	{ CCI_REG8(0x4ede), 0x01 },
	{ CCI_REG8(0x4edf), 0x45 },
	{ CCI_REG8(0x5222), 0x02 },
	{ CCI_REG8(0x5617), 0x0a },
	{ CCI_REG8(0x562b), 0x0a },
	{ CCI_REG8(0x562d), 0x0c },
	{ CCI_REG8(0x56b7), 0x74 },
	{ CCI_REG8(0x6282), 0x82 },
	{ CCI_REG8(0x6283), 0x80 },
	{ CCI_REG8(0x6286), 0x07 },
	{ CCI_REG8(0x6287), 0xc0 },
	{ CCI_REG8(0x6288), 0x08 },
	{ CCI_REG8(0x628a), 0x18 },
	{ CCI_REG8(0x628b), 0x80 },
	{ CCI_REG8(0x628c), 0x20 },
	{ CCI_REG8(0x628e), 0x32 },
	{ CCI_REG8(0x6290), 0x40 },
	{ CCI_REG8(0x6292), 0x0a },
	{ CCI_REG8(0x6296), 0x50 },
	{ CCI_REG8(0x629a), 0xf8 },
	{ CCI_REG8(0x629b), 0x01 },
	{ CCI_REG8(0x629d), 0x03 },
	{ CCI_REG8(0x629f), 0x04 },
	{ CCI_REG8(0x62b1), 0x06 },
	{ CCI_REG8(0x62b5), 0x3c },
	{ CCI_REG8(0x62b9), 0xc8 },
	{ CCI_REG8(0x62bc), 0x02 },
	{ CCI_REG8(0x62bd), 0x70 },
	{ CCI_REG8(0x62d0), 0x06 },
	{ CCI_REG8(0x62d4), 0x38 },
	{ CCI_REG8(0x62d8), 0xb8 },
	{ CCI_REG8(0x62db), 0x02 },
	{ CCI_REG8(0x62dc), 0x40 },
	{ CCI_REG8(0x62dd), 0x03 },
	{ CCI_REG8(0x637a), 0x11 },
	{ CCI_REG8(0x7ba0), 0x01 },
	{ CCI_REG8(0x7ba9), 0x00 },
	{ CCI_REG8(0x7baa), 0x01 },
	{ CCI_REG8(0x7bad), 0x00 },
	{ CCI_REG8(0x9002), 0x00 },
	{ CCI_REG8(0x9003), 0x00 },
	{ CCI_REG8(0x9004), 0x09 },
	{ CCI_REG8(0x9006), 0x01 },
	{ CCI_REG8(0x9200), 0x93 },
	{ CCI_REG8(0x9201), 0x85 },
	{ CCI_REG8(0x9202), 0x93 },
	{ CCI_REG8(0x9203), 0x87 },
	{ CCI_REG8(0x9204), 0x93 },
	{ CCI_REG8(0x9205), 0x8d },
	{ CCI_REG8(0x9206), 0x93 },
	{ CCI_REG8(0x9207), 0x8f },
	{ CCI_REG8(0x9208), 0x6a },
	{ CCI_REG8(0x9209), 0x22 },
	{ CCI_REG8(0x920a), 0x6a },
	{ CCI_REG8(0x920b), 0x23 },
	{ CCI_REG8(0x920c), 0x6a },
	{ CCI_REG8(0x920d), 0x0f },
	{ CCI_REG8(0x920e), 0x71 },
	{ CCI_REG8(0x920f), 0x03 },
	{ CCI_REG8(0x9210), 0x71 },
	{ CCI_REG8(0x9211), 0x0b },
	{ CCI_REG8(0x935d), 0x01 },
	{ CCI_REG8(0x9389), 0x05 },
	{ CCI_REG8(0x938b), 0x05 },
	{ CCI_REG8(0x9391), 0x05 },
	{ CCI_REG8(0x9393), 0x05 },
	{ CCI_REG8(0x9395), 0x82 },
	{ CCI_REG8(0x9397), 0x78 },
	{ CCI_REG8(0x9399), 0x05 },
	{ CCI_REG8(0x939b), 0x05 },
	{ CCI_REG8(0xa91f), 0x04 },
	{ CCI_REG8(0xa921), 0x03 },
	{ CCI_REG8(0xa923), 0x02 },
	{ CCI_REG8(0xa93d), 0x05 },
	{ CCI_REG8(0xa93f), 0x03 },
	{ CCI_REG8(0xa941), 0x02 },
	{ CCI_REG8(0xa9af), 0x04 },
	{ CCI_REG8(0xa9b1), 0x03 },
	{ CCI_REG8(0xa9b3), 0x02 },
	{ CCI_REG8(0xa9cd), 0x05 },
	{ CCI_REG8(0xa9cf), 0x03 },
	{ CCI_REG8(0xa9d1), 0x02 },
	{ CCI_REG8(0xaa3f), 0x04 },
	{ CCI_REG8(0xaa41), 0x03 },
	{ CCI_REG8(0xaa43), 0x02 },
	{ CCI_REG8(0xaa5d), 0x05 },
	{ CCI_REG8(0xaa5f), 0x03 },
	{ CCI_REG8(0xaa61), 0x02 },
	{ CCI_REG8(0xaacf), 0x04 },
	{ CCI_REG8(0xaad1), 0x03 },
	{ CCI_REG8(0xaad3), 0x02 },
	{ CCI_REG8(0xaaed), 0x05 },
	{ CCI_REG8(0xaaef), 0x03 },
	{ CCI_REG8(0xaaf1), 0x02 },
	{ CCI_REG8(0xab87), 0x04 },
	{ CCI_REG8(0xab89), 0x03 },
	{ CCI_REG8(0xab8b), 0x02 },
	{ CCI_REG8(0xaba5), 0x05 },
	{ CCI_REG8(0xaba7), 0x03 },
	{ CCI_REG8(0xaba9), 0x02 },
	{ CCI_REG8(0xabb7), 0x04 },
	{ CCI_REG8(0xabb9), 0x03 },
	{ CCI_REG8(0xabbb), 0x02 },
	{ CCI_REG8(0xabd5), 0x05 },
	{ CCI_REG8(0xabd7), 0x03 },
	{ CCI_REG8(0xabd9), 0x02 },
	{ CCI_REG8(0xb388), 0x28 },
	{ CCI_REG8(0xbc40), 0x03 },
	{ CCI_REG8(0x3ff9), 0x00 },
};

static const struct cci_reg_sequence imx351_init_iq[] = {
	{ CCI_REG8(0x7b80), 0x00 },
	{ CCI_REG8(0x7b81), 0x00 },
	{ CCI_REG8(0x8d1f), 0x00 },
	{ CCI_REG8(0x8d27), 0x00 },
	{ CCI_REG8(0x9963), 0x64 },
	{ CCI_REG8(0x9964), 0x50 },
	{ CCI_REG8(0x9a00), 0x0c },
	{ CCI_REG8(0x9a01), 0x0c },
	{ CCI_REG8(0x9a06), 0x0c },
	{ CCI_REG8(0x9a18), 0x0c },
	{ CCI_REG8(0x9a19), 0x0c },
	{ CCI_REG8(0xa900), 0x20 },
	{ CCI_REG8(0xa901), 0x20 },
	{ CCI_REG8(0xa902), 0x20 },
	{ CCI_REG8(0xa903), 0x15 },
	{ CCI_REG8(0xa904), 0x15 },
	{ CCI_REG8(0xa905), 0x15 },
	{ CCI_REG8(0xa906), 0x20 },
	{ CCI_REG8(0xa907), 0x20 },
	{ CCI_REG8(0xa908), 0x20 },
	{ CCI_REG8(0xa909), 0x15 },
	{ CCI_REG8(0xa90a), 0x15 },
	{ CCI_REG8(0xa90b), 0x15 },
	{ CCI_REG8(0xa915), 0x3f },
	{ CCI_REG8(0xa916), 0x3f },
	{ CCI_REG8(0xa917), 0x3f },
	{ CCI_REG8(0xa949), 0x03 },
	{ CCI_REG8(0xa94b), 0x03 },
	{ CCI_REG8(0xa94d), 0x03 },
	{ CCI_REG8(0xa94f), 0x06 },
	{ CCI_REG8(0xa951), 0x06 },
	{ CCI_REG8(0xa953), 0x06 },
	{ CCI_REG8(0xa955), 0x03 },
	{ CCI_REG8(0xa957), 0x03 },
	{ CCI_REG8(0xa959), 0x03 },
	{ CCI_REG8(0xa95b), 0x06 },
	{ CCI_REG8(0xa95d), 0x06 },
	{ CCI_REG8(0xa95f), 0x06 },
	{ CCI_REG8(0xa98b), 0x1f },
	{ CCI_REG8(0xa98d), 0x1f },
	{ CCI_REG8(0xa98f), 0x1f },
	{ CCI_REG8(0xaa20), 0x3f },
	{ CCI_REG8(0xaa21), 0x20 },
	{ CCI_REG8(0xaa22), 0x20 },
	{ CCI_REG8(0xaa23), 0x3f },
	{ CCI_REG8(0xaa24), 0x15 },
	{ CCI_REG8(0xaa25), 0x15 },
	{ CCI_REG8(0xaa26), 0x20 },
	{ CCI_REG8(0xaa27), 0x20 },
	{ CCI_REG8(0xaa28), 0x20 },
	{ CCI_REG8(0xaa29), 0x15 },
	{ CCI_REG8(0xaa2a), 0x15 },
	{ CCI_REG8(0xaa2b), 0x15 },
	{ CCI_REG8(0xaa32), 0x3f },
	{ CCI_REG8(0xaa35), 0x3f },
	{ CCI_REG8(0xaa36), 0x3f },
	{ CCI_REG8(0xaa37), 0x3f },
	{ CCI_REG8(0xaa69), 0x3f },
	{ CCI_REG8(0xaa6b), 0x03 },
	{ CCI_REG8(0xaa6d), 0x03 },
	{ CCI_REG8(0xaa6f), 0x3f },
	{ CCI_REG8(0xaa71), 0x06 },
	{ CCI_REG8(0xaa73), 0x06 },
	{ CCI_REG8(0xaa75), 0x03 },
	{ CCI_REG8(0xaa77), 0x03 },
	{ CCI_REG8(0xaa79), 0x03 },
	{ CCI_REG8(0xaa7b), 0x06 },
	{ CCI_REG8(0xaa7d), 0x06 },
	{ CCI_REG8(0xaa7f), 0x06 },
	{ CCI_REG8(0xaaab), 0x1f },
	{ CCI_REG8(0xaaad), 0x1f },
	{ CCI_REG8(0xaaaf), 0x1f },
	{ CCI_REG8(0xaab0), 0x20 },
	{ CCI_REG8(0xaab1), 0x20 },
	{ CCI_REG8(0xaab2), 0x20 },
	{ CCI_REG8(0xaac2), 0x3f },
	{ CCI_REG8(0xab53), 0x20 },
	{ CCI_REG8(0xab54), 0x20 },
	{ CCI_REG8(0xab55), 0x20 },
	{ CCI_REG8(0xab57), 0x40 },
	{ CCI_REG8(0xab59), 0x40 },
	{ CCI_REG8(0xab5b), 0x40 },
	{ CCI_REG8(0xab63), 0x03 },
	{ CCI_REG8(0xab65), 0x03 },
	{ CCI_REG8(0xab67), 0x03 },
	{ CCI_REG8(0xac01), 0x0a },
	{ CCI_REG8(0xac03), 0x0a },
	{ CCI_REG8(0xac05), 0x0a },
	{ CCI_REG8(0xac06), 0x01 },
	{ CCI_REG8(0xac07), 0xc0 },
	{ CCI_REG8(0xac09), 0xc0 },
	{ CCI_REG8(0xac17), 0x0a },
	{ CCI_REG8(0xac19), 0x0a },
	{ CCI_REG8(0xac1b), 0x0a },
	{ CCI_REG8(0xac1c), 0x01 },
	{ CCI_REG8(0xac1d), 0xc0 },
	{ CCI_REG8(0xac1f), 0xc0 },
	{ CCI_REG8(0x422a), 0x00 },
	{ CCI_REG8(0x4246), 0x00 },
	{ CCI_REG8(0x4bd7), 0x15 },
	{ CCI_REG8(0x423e), 0xff },
	{ CCI_REG8(0x4239), 0x00 },
};

static const struct cci_reg_sequence imx351_mode_4656x3492[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x17 },
	{ CCI_REG8(0x0343), 0x90 },
	{ CCI_REG8(0x0340), 0x0d },
	{ CCI_REG8(0x0341), 0xe8 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x12 },
	{ CCI_REG8(0x0349), 0x2f },
	{ CCI_REG8(0x034a), 0x0d },
	{ CCI_REG8(0x034b), 0xa3 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x0902), 0x0a },
	{ CCI_REG8(0x3243), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x01 },
	{ CCI_REG8(0x4254), 0x7f },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x12 },
	{ CCI_REG8(0x040d), 0x30 },
	{ CCI_REG8(0x040e), 0x0d },
	{ CCI_REG8(0x040f), 0xa4 },
	{ CCI_REG8(0x034c), 0x12 },
	{ CCI_REG8(0x034d), 0x30 },
	{ CCI_REG8(0x034e), 0x0d },
	{ CCI_REG8(0x034f), 0xa4 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x02 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x87 },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xdc },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0820), 0x14 },
	{ CCI_REG8(0x0821), 0xa0 },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0xbc41), 0x01 },
	{ CCI_REG8(0x3e20), 0x01 },
	{ CCI_REG8(0x3e37), 0x01 },
	{ CCI_REG8(0x3e3b), 0x00 },
	{ CCI_REG8(0x0106), 0x00 },
	{ CCI_REG8(0x0b00), 0x00 },
	{ CCI_REG8(0x3230), 0x00 },
	{ CCI_REG8(0x3c00), 0x5b },
	{ CCI_REG8(0x3c01), 0x54 },
	{ CCI_REG8(0x3c02), 0x77 },
	{ CCI_REG8(0x3c03), 0x66 },
	{ CCI_REG8(0x3c04), 0x00 },
	{ CCI_REG8(0x3c05), 0xc8 },
	{ CCI_REG8(0x3c06), 0x14 },
	{ CCI_REG8(0x3c07), 0x00 },
	{ CCI_REG8(0x3c08), 0x01 },
	{ CCI_REG8(0x3f14), 0x01 },
	{ CCI_REG8(0x3f17), 0x00 },
	{ CCI_REG8(0x3f3c), 0x01 },
	{ CCI_REG8(0x3f78), 0x00 },
	{ CCI_REG8(0x3f79), 0xb4 },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x00 },
	{ CCI_REG8(0x97c1), 0x00 },
	{ CCI_REG8(0x97c5), 0x14 },
	{ CCI_REG8(0x0202), 0x0d },
	{ CCI_REG8(0x0203), 0xd4 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct cci_reg_sequence imx351_mode_2328x1744[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x03 },
	{ CCI_REG8(0x0342), 0x17 },
	{ CCI_REG8(0x0343), 0x90 },
	{ CCI_REG8(0x0340), 0x0a },
	{ CCI_REG8(0x0341), 0xdc },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x04 },
	{ CCI_REG8(0x0348), 0x12 },
	{ CCI_REG8(0x0349), 0x2f },
	{ CCI_REG8(0x034a), 0x0d },
	{ CCI_REG8(0x034b), 0xa3 },
	{ CCI_REG8(0x0220), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x0a },
	{ CCI_REG8(0x3243), 0x00 },
	{ CCI_REG8(0x3f4c), 0x01 },
	{ CCI_REG8(0x3f4d), 0x03 },
	{ CCI_REG8(0x4254), 0x7f },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x09 },
	{ CCI_REG8(0x040d), 0x18 },
	{ CCI_REG8(0x040e), 0x06 },
	{ CCI_REG8(0x040f), 0xd0 },
	{ CCI_REG8(0x034c), 0x09 },
	{ CCI_REG8(0x034d), 0x18 },
	{ CCI_REG8(0x034e), 0x06 },
	{ CCI_REG8(0x034f), 0xd0 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x02 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0x69 },
	{ CCI_REG8(0x030b), 0x02 },
	{ CCI_REG8(0x030d), 0x04 },
	{ CCI_REG8(0x030e), 0x00 },
	{ CCI_REG8(0x030f), 0xdc },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x0820), 0x0a },
	{ CCI_REG8(0x0821), 0x50 },
	{ CCI_REG8(0x0822), 0x00 },
	{ CCI_REG8(0x0823), 0x00 },
	{ CCI_REG8(0xbc41), 0x01 },
	{ CCI_REG8(0x3e20), 0x01 },
	{ CCI_REG8(0x3e37), 0x01 },
	{ CCI_REG8(0x3e3b), 0x00 },
	{ CCI_REG8(0x0106), 0x00 },
	{ CCI_REG8(0x0b00), 0x00 },
	{ CCI_REG8(0x3230), 0x00 },
	{ CCI_REG8(0x3c00), 0x6d },
	{ CCI_REG8(0x3c01), 0x5b },
	{ CCI_REG8(0x3c02), 0x77 },
	{ CCI_REG8(0x3c03), 0x66 },
	{ CCI_REG8(0x3c04), 0x00 },
	{ CCI_REG8(0x3c05), 0x10 },
	{ CCI_REG8(0x3c06), 0x14 },
	{ CCI_REG8(0x3c07), 0x00 },
	{ CCI_REG8(0x3c08), 0x01 },
	{ CCI_REG8(0x3f14), 0x01 },
	{ CCI_REG8(0x3f17), 0x00 },
	{ CCI_REG8(0x3f3c), 0x01 },
	{ CCI_REG8(0x3f78), 0x02 },
	{ CCI_REG8(0x3f79), 0xd4 },
	{ CCI_REG8(0x3f7c), 0x00 },
	{ CCI_REG8(0x3f7d), 0x00 },
	{ CCI_REG8(0x97c1), 0x04 },
	{ CCI_REG8(0x97c5), 0x0c },
	{ CCI_REG8(0x620d), 0xd0 },
	{ CCI_REG8(0x620e), 0x27 },
	{ CCI_REG8(0x6399), 0xf4 },
	{ CCI_REG8(0x6f6e), 0x02 },
	{ CCI_REG8(0x0202), 0x0a },
	{ CCI_REG8(0x0203), 0xc8 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
};

static const struct imx351_mode imx351_modes[] = {
	{
		.width = 4656,
		.height = 3492,
		.crop = { .left = 0, .top = 0, .width = 4656, .height = 3492 },
		.line_length = 6032,
		.frame_length = 3560,
		.pixel_rate = 648000000,
		.link_freq = IMX351_LINK_FREQ_660MHZ,
		.regs = imx351_mode_4656x3492,
		.num_regs = ARRAY_SIZE(imx351_mode_4656x3492),
	},
	{
		.width = 2328,
		.height = 1744,
		.crop = { .left = 0, .top = 4, .width = 4656, .height = 3488 },
		.line_length = 6032,
		.frame_length = 2780,
		.pixel_rate = 504000000,
		.link_freq = IMX351_LINK_FREQ_330MHZ,
		.regs = imx351_mode_2328x1744,
		.num_regs = ARRAY_SIZE(imx351_mode_2328x1744),
	},
};

struct imx351 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;

	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx351_supply_names)];

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

static inline struct imx351 *to_imx351(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx351, sd);
}

static bool imx351_mode_usable(struct imx351 *imx351,
			       const struct imx351_mode *mode)
{
	return imx351->link_freq_bitmap & BIT(mode->link_freq);
}

static u32 imx351_mbus_code(struct imx351 *imx351)
{
	unsigned int i = 0;

	if (imx351->hflip && imx351->hflip->val)
		i |= 1;
	if (imx351->vflip && imx351->vflip->val)
		i |= 2;

	return imx351_mbus_codes[i];
}

static const struct imx351_mode *
imx351_find_mode(struct imx351 *imx351, u32 width, u32 height)
{
	const struct imx351_mode *best = NULL;
	unsigned int i;
	u32 dist, best_dist = U32_MAX;

	for (i = 0; i < ARRAY_SIZE(imx351_modes); i++) {
		const struct imx351_mode *mode = &imx351_modes[i];

		if (!imx351_mode_usable(imx351, mode))
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

static const struct imx351_mode *
imx351_state_mode(struct imx351 *imx351, struct v4l2_subdev_state *state)
{
	const struct v4l2_mbus_framefmt *fmt =
		v4l2_subdev_state_get_format(state, 0);

	return imx351_find_mode(imx351, fmt->width, fmt->height);
}

static void imx351_update_limits(struct imx351 *imx351,
				 const struct imx351_mode *mode)
{
	u32 vblank_def = mode->frame_length - mode->height;
	u32 hblank = mode->line_length - mode->width;

	__v4l2_ctrl_s_ctrl_int64(imx351->pixel_rate, mode->pixel_rate);
	__v4l2_ctrl_s_ctrl(imx351->link_freq, mode->link_freq);

	/* Never run a mode faster than LG does */
	__v4l2_ctrl_modify_range(imx351->vblank, vblank_def,
				 IMX351_FRAME_LENGTH_MAX - mode->height,
				 1, vblank_def);
	__v4l2_ctrl_s_ctrl(imx351->vblank, vblank_def);

	__v4l2_ctrl_modify_range(imx351->hblank, hblank, hblank, 1, hblank);

	__v4l2_ctrl_modify_range(imx351->exposure, IMX351_EXPOSURE_MIN,
				 mode->frame_length - IMX351_EXPOSURE_OFFSET,
				 1, IMX351_EXPOSURE_DEFAULT);
}

static int imx351_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx351 *imx351 = container_of(ctrl->handler, struct imx351,
					     ctrls);
	struct v4l2_subdev_state *state =
		v4l2_subdev_get_locked_active_state(&imx351->sd);
	const struct imx351_mode *mode = imx351_state_mode(imx351, state);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK && mode) {
		u32 max = mode->height + ctrl->val - IMX351_EXPOSURE_OFFSET;

		__v4l2_ctrl_modify_range(imx351->exposure,
					 imx351->exposure->minimum, max,
					 imx351->exposure->step,
					 min(imx351->exposure->default_value,
					     (s64)max));
	}

	/* Only touch the hardware while it is powered */
	if (!pm_runtime_get_if_active(imx351->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(imx351->regmap, IMX351_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(imx351->regmap, IMX351_REG_ANALOG_GAIN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		cci_write(imx351->regmap, IMX351_REG_DIG_GAIN_GR, ctrl->val,
			  &ret);
		cci_write(imx351->regmap, IMX351_REG_DIG_GAIN_R, ctrl->val,
			  &ret);
		cci_write(imx351->regmap, IMX351_REG_DIG_GAIN_B, ctrl->val,
			  &ret);
		cci_write(imx351->regmap, IMX351_REG_DIG_GAIN_GB, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_VBLANK:
		if (mode)
			cci_write(imx351->regmap, IMX351_REG_FRAME_LENGTH,
				  mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(imx351->regmap, IMX351_REG_TEST_PATTERN, ctrl->val,
			  &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(imx351->regmap, IMX351_REG_ORIENTATION,
			  imx351->hflip->val | imx351->vflip->val << 1, &ret);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(imx351->dev);

	return ret;
}

static const struct v4l2_ctrl_ops imx351_ctrl_ops = {
	.s_ctrl = imx351_set_ctrl,
};

static int imx351_init_controls(struct imx351 *imx351)
{
	struct v4l2_ctrl_handler *hdl = &imx351->ctrls;
	const struct imx351_mode *mode = imx351_find_mode(imx351, 0, 0);
	struct v4l2_fwnode_device_properties props;
	int ret;

	ret = v4l2_fwnode_device_parse(imx351->dev, &props);
	if (ret)
		return ret;

	v4l2_ctrl_handler_init(hdl, 12);

	imx351->pixel_rate = v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       mode->pixel_rate,
					       mode->pixel_rate, 1,
					       mode->pixel_rate);

	imx351->link_freq = v4l2_ctrl_new_int_menu(hdl, &imx351_ctrl_ops,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(imx351_link_freqs) - 1,
						   mode->link_freq,
						   imx351_link_freqs);

	imx351->vblank = v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops,
					   V4L2_CID_VBLANK,
					   mode->frame_length - mode->height,
					   IMX351_FRAME_LENGTH_MAX - mode->height,
					   1, mode->frame_length - mode->height);

	imx351->hblank = v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops,
					   V4L2_CID_HBLANK,
					   mode->line_length - mode->width,
					   mode->line_length - mode->width, 1,
					   mode->line_length - mode->width);

	imx351->exposure = v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX351_EXPOSURE_MIN,
					     mode->frame_length -
					     IMX351_EXPOSURE_OFFSET,
					     1, IMX351_EXPOSURE_DEFAULT);

	v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  IMX351_ANA_GAIN_MIN, IMX351_ANA_GAIN_MAX, 1,
			  IMX351_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  IMX351_DGTL_GAIN_MIN, IMX351_DGTL_GAIN_MAX, 1,
			  IMX351_DGTL_GAIN_DEFAULT);

	v4l2_ctrl_new_std_menu_items(hdl, &imx351_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx351_test_pattern_menu) - 1,
				     0, 0, imx351_test_pattern_menu);

	imx351->hflip = v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	imx351->vflip = v4l2_ctrl_new_std(hdl, &imx351_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_fwnode_properties(hdl, &imx351_ctrl_ops, &props);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	imx351->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	imx351->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	imx351->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	/* Flipping changes the Bayer order */
	imx351->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
	imx351->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	imx351->sd.ctrl_handler = hdl;

	return 0;
}

static int imx351_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct imx351 *imx351 = to_imx351(sd);
	const struct imx351_mode *mode = imx351_state_mode(imx351, state);
	int ret;

	if (!mode)
		return -EINVAL;

	ret = pm_runtime_resume_and_get(imx351->dev);
	if (ret)
		return ret;

	ret = cci_multi_reg_write(imx351->regmap, imx351_init_common,
				  ARRAY_SIZE(imx351_init_common), NULL);
	if (!ret)
		ret = cci_multi_reg_write(imx351->regmap, imx351_init_iq,
					  ARRAY_SIZE(imx351_init_iq), NULL);
	if (!ret)
		ret = cci_multi_reg_write(imx351->regmap, mode->regs,
					  mode->num_regs, NULL);
	if (ret) {
		dev_err(imx351->dev, "failed to program the sensor: %d\n", ret);
		goto err_put;
	}

	ret = __v4l2_ctrl_handler_setup(&imx351->ctrls);
	if (ret)
		goto err_put;

	ret = cci_write(imx351->regmap, IMX351_REG_MODE_SELECT,
			IMX351_MODE_STREAMING, NULL);
	if (ret)
		goto err_put;

	/* The Bayer order must not change under a running stream */
	__v4l2_ctrl_grab(imx351->hflip, true);
	__v4l2_ctrl_grab(imx351->vflip, true);

	return 0;

err_put:
	pm_runtime_put(imx351->dev);
	return ret;
}

static int imx351_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct imx351 *imx351 = to_imx351(sd);
	int ret;

	ret = cci_write(imx351->regmap, IMX351_REG_MODE_SELECT,
			IMX351_MODE_STANDBY, NULL);

	__v4l2_ctrl_grab(imx351->hflip, false);
	__v4l2_ctrl_grab(imx351->vflip, false);

	pm_runtime_put_autosuspend(imx351->dev);

	return ret;
}

static void imx351_fill_format(struct imx351 *imx351,
			       struct v4l2_mbus_framefmt *fmt,
			       const struct imx351_mode *mode)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = imx351_mbus_code(imx351);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx351_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = imx351_mbus_code(to_imx351(sd));

	return 0;
}

static int imx351_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct imx351 *imx351 = to_imx351(sd);
	unsigned int i, n = 0;

	if (fse->code != imx351_mbus_code(imx351))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(imx351_modes); i++) {
		const struct imx351_mode *mode = &imx351_modes[i];

		if (!imx351_mode_usable(imx351, mode))
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

static int imx351_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *format)
{
	struct imx351 *imx351 = to_imx351(sd);
	const struct imx351_mode *mode;

	mode = imx351_find_mode(imx351, format->format.width,
				format->format.height);
	if (!mode)
		return -EINVAL;

	imx351_fill_format(imx351, &format->format, mode);
	*v4l2_subdev_state_get_format(state, 0) = format->format;
	*v4l2_subdev_state_get_crop(state, 0) = mode->crop;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		imx351_update_limits(imx351, mode);

	return 0;
}

static int imx351_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = IMX351_NATIVE_WIDTH;
		sel->r.height = IMX351_NATIVE_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int imx351_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct imx351 *imx351 = to_imx351(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.width = imx351_modes[0].width,
			.height = imx351_modes[0].height,
		},
	};

	return imx351_set_format(&imx351->sd, state, &fmt);
}

static const struct v4l2_subdev_video_ops imx351_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx351_pad_ops = {
	.enum_mbus_code = imx351_enum_mbus_code,
	.enum_frame_size = imx351_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx351_set_format,
	.get_selection = imx351_get_selection,
	.enable_streams = imx351_enable_streams,
	.disable_streams = imx351_disable_streams,
};

static const struct v4l2_subdev_ops imx351_subdev_ops = {
	.video = &imx351_video_ops,
	.pad = &imx351_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx351_internal_ops = {
	.init_state = imx351_init_state,
};

static int imx351_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx351 *imx351 = to_imx351(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx351->supplies),
				    imx351->supplies);
	if (ret) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	usleep_range(1000, 2000);

	ret = clk_prepare_enable(imx351->xclk);
	if (ret) {
		dev_err(dev, "failed to enable clock: %d\n", ret);
		goto err_regulators;
	}

	gpiod_set_value_cansleep(imx351->reset_gpio, 0);
	usleep_range(10000, 11000);

	return 0;

err_regulators:
	regulator_bulk_disable(ARRAY_SIZE(imx351->supplies), imx351->supplies);
	return ret;
}

static int imx351_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx351 *imx351 = to_imx351(sd);

	gpiod_set_value_cansleep(imx351->reset_gpio, 1);
	clk_disable_unprepare(imx351->xclk);
	regulator_bulk_disable(ARRAY_SIZE(imx351->supplies), imx351->supplies);

	return 0;
}

static int imx351_parse_endpoint(struct imx351 *imx351)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_next_endpoint(dev_fwnode(imx351->dev), NULL);
	if (!ep)
		return dev_err_probe(imx351->dev, -ENXIO, "no endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return dev_err_probe(imx351->dev, ret,
				     "failed to parse endpoint\n");

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != 4) {
		ret = dev_err_probe(imx351->dev, -EINVAL,
				    "only 4 data lanes are supported\n");
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(imx351->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       imx351_link_freqs,
				       ARRAY_SIZE(imx351_link_freqs),
				       &imx351->link_freq_bitmap);

out:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int imx351_identify(struct imx351 *imx351)
{
	u64 id;
	int ret;

	ret = cci_read(imx351->regmap, IMX351_REG_CHIP_ID, &id, NULL);
	if (ret)
		return dev_err_probe(imx351->dev, ret,
				     "failed to read the chip ID\n");

	if (id != IMX351_CHIP_ID)
		return dev_err_probe(imx351->dev, -ENODEV,
				     "unexpected chip ID 0x%04llx\n", id);

	return 0;
}

static int imx351_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx351 *imx351;
	unsigned int i;
	int ret;

	imx351 = devm_kzalloc(dev, sizeof(*imx351), GFP_KERNEL);
	if (!imx351)
		return -ENOMEM;

	imx351->dev = dev;

	ret = imx351_parse_endpoint(imx351);
	if (ret)
		return ret;

	imx351->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx351->regmap))
		return dev_err_probe(dev, PTR_ERR(imx351->regmap),
				     "failed to initialize CCI\n");

	imx351->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(imx351->xclk))
		return dev_err_probe(dev, PTR_ERR(imx351->xclk),
				     "failed to get the clock\n");

	if (clk_get_rate(imx351->xclk) != IMX351_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "the clock must run at %u Hz\n",
				     IMX351_XCLK_FREQ);

	for (i = 0; i < ARRAY_SIZE(imx351_supply_names); i++)
		imx351->supplies[i].supply = imx351_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(imx351->supplies),
				      imx351->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	imx351->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx351->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(imx351->reset_gpio),
				     "failed to get the reset GPIO\n");

	v4l2_i2c_subdev_init(&imx351->sd, client, &imx351_subdev_ops);
	imx351->sd.internal_ops = &imx351_internal_ops;

	ret = imx351_power_on(dev);
	if (ret)
		return ret;

	ret = imx351_identify(imx351);
	if (ret)
		goto err_power_off;

	ret = imx351_init_controls(imx351);
	if (ret)
		goto err_power_off;

	imx351->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx351->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx351->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&imx351->sd.entity, 1, &imx351->pad);
	if (ret)
		goto err_ctrls;

	imx351->sd.state_lock = imx351->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&imx351->sd);
	if (ret)
		goto err_entity;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	ret = v4l2_async_register_subdev_sensor(&imx351->sd);
	if (ret)
		goto err_pm;

	pm_runtime_idle(dev);

	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&imx351->sd);
err_entity:
	media_entity_cleanup(&imx351->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&imx351->ctrls);
err_power_off:
	imx351_power_off(dev);
	return ret;
}

static void imx351_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx351 *imx351 = to_imx351(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&imx351->ctrls);

	pm_runtime_disable(imx351->dev);
	if (!pm_runtime_status_suspended(imx351->dev))
		imx351_power_off(imx351->dev);
	pm_runtime_set_suspended(imx351->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(imx351_pm_ops, imx351_power_off,
				 imx351_power_on, NULL);

static const struct of_device_id imx351_of_match[] = {
	{ .compatible = "sony,imx351" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx351_of_match);

static struct i2c_driver imx351_i2c_driver = {
	.driver = {
		.name = "imx351",
		.of_match_table = imx351_of_match,
		.pm = pm_ptr(&imx351_pm_ops),
	},
	.probe = imx351_probe,
	.remove = imx351_remove,
};
module_i2c_driver(imx351_i2c_driver);

MODULE_DESCRIPTION("Sony IMX351 image sensor driver");
MODULE_LICENSE("GPL");
