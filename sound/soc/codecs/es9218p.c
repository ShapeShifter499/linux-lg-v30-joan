// SPDX-License-Identifier: GPL-2.0-only
/*
 * ESS Technology ES9218P "Sabre" headphone DAC/amplifier
 *
 * Found on the LG V30 (msm8998, joan) as the "Quad DAC", sitting on BLSP1 QUP1
 * at I2C 0x48, downstream of the WCD9340 codec on the headphone path.
 *
 * This is a fresh ASoC driver using the upstream idioms (regmap, DAPM,
 * component driver). It is NOT a copy of LG's downstream es9218p.c, which is
 * ~3700 lines dominated by vendor headphone-detection, impedance measurement,
 * custom sysfs and per-SKU tuning. The register semantics below are taken from
 * that driver's header, which is the only public description of the part.
 *
 * STATUS: skeleton. Probe, regmap, volume, mute and power sequencing are
 * implemented. Not yet probed on hardware -- there is no ES9218P support
 * anywhere upstream to compare against (the only ESS codec in tree, es9356,
 * is a SoundWire part and shares no bus model with this one).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define ES9218P_SYSTEM_REG		0x00
#define ES9218P_INPUT_SELECT		0x01
#define ES9218P_AUTOMUTE_CONFIG		0x02
#define ES9218P_ANALOG_VOL_CTRL		0x03
#define ES9218P_AUTOMUTE_TIME		0x04
#define ES9218P_AUTOMUTE_LEVEL		0x05
#define ES9218P_DOP_VOL_RAMP_RATE	0x06
#define ES9218P_FILTER_BAND_SYSTEM_MUTE	0x07
#define ES9218P_GPIO1_2_CONFIG		0x08
#define ES9218P_MASTERMODE_SYNC_CONFIG	0x0a
#define ES9218P_OVERCURRENT_PROTECT	0x0b
#define ES9218P_DPLL_BANDWIDTH		0x0c
#define ES9218P_THD_COMP_MONO_MODE	0x0d
#define ES9218P_SOFT_START_CONFIG	0x0e
#define ES9218P_VOL1_CTRL		0x0f
#define ES9218P_VOL2_CTRL		0x10
#define ES9218P_MASTERTRIM_4		0x11
#define ES9218P_MASTERTRIM_3		0x12
#define ES9218P_MASTERTRIM_2		0x13
#define ES9218P_MASTERTRIM_1		0x14
#define ES9218P_GPIO_INPUT_SEL		0x15
#define ES9218P_GEN_CONFIG		0x1b
#define ES9218P_CHIP_ID			0x40

#define ES9218P_MAX_REGISTER		0x45

/* ES9218P_INPUT_SELECT */
#define ES9218P_INPUT_SEL_MASK		GENMASK(7, 6)
#define ES9218P_INPUT_SEL_I2S		0
#define ES9218P_SERIAL_LEN_MASK		GENMASK(1, 0)
#define ES9218P_SERIAL_LEN_16		0
#define ES9218P_SERIAL_LEN_24		1
#define ES9218P_SERIAL_LEN_32		2

/* ES9218P_FILTER_BAND_SYSTEM_MUTE */
#define ES9218P_SYSTEM_MUTE		BIT(0)

struct es9218p_priv {
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *power_gpio;
	struct gpio_desc *hph_sw_gpio;
};

static const struct reg_default es9218p_reg_defaults[] = {
	{ ES9218P_SYSTEM_REG,		0x00 },
	{ ES9218P_INPUT_SELECT,		0x8c },
	{ ES9218P_ANALOG_VOL_CTRL,	0x40 },
	{ ES9218P_VOL1_CTRL,		0x50 },
	{ ES9218P_VOL2_CTRL,		0x50 },
};

static bool es9218p_volatile_reg(struct device *dev, unsigned int reg)
{
	/* status and chip-id space is read-only/volatile */
	return reg >= ES9218P_CHIP_ID;
}

static const struct regmap_config es9218p_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= ES9218P_MAX_REGISTER,
	.reg_defaults	= es9218p_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(es9218p_reg_defaults),
	.volatile_reg	= es9218p_volatile_reg,
	.cache_type	= REGCACHE_MAPLE,
};

/*
 * Digital volume, registers 0x0f/0x10, one per channel. Downstream treats the
 * value as attenuation in 0.5 dB steps with 0x00 = 0 dB, so the usable range is
 * 0 dB down to -127.5 dB.
 */
static const DECLARE_TLV_DB_SCALE(es9218p_vol_tlv, -12750, 50, 1);

static const struct snd_kcontrol_new es9218p_snd_controls[] = {
	SOC_DOUBLE_R_TLV("Headphone Playback Volume",
			 ES9218P_VOL1_CTRL, ES9218P_VOL2_CTRL,
			 0, 0xff, 1, es9218p_vol_tlv),
	SOC_SINGLE("Headphone Playback Switch",
		   ES9218P_FILTER_BAND_SYSTEM_MUTE, 0, 1, 1),
};

static const struct snd_soc_dapm_widget es9218p_dapm_widgets[] = {
	SND_SOC_DAPM_DAC("DAC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("HPOUTL"),
	SND_SOC_DAPM_OUTPUT("HPOUTR"),
};

static const struct snd_soc_dapm_route es9218p_dapm_routes[] = {
	{ "DAC", NULL, "Playback" },
	{ "HPOUTL", NULL, "DAC" },
	{ "HPOUTR", NULL, "DAC" },
};

static int es9218p_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct es9218p_priv *es9218p = snd_soc_component_get_drvdata(dai->component);
	unsigned int len;

	switch (params_width(params)) {
	case 16:
		len = ES9218P_SERIAL_LEN_16;
		break;
	case 24:
		len = ES9218P_SERIAL_LEN_24;
		break;
	case 32:
		len = ES9218P_SERIAL_LEN_32;
		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(es9218p->regmap, ES9218P_INPUT_SELECT,
				  ES9218P_SERIAL_LEN_MASK, len);
}

static int es9218p_mute_stream(struct snd_soc_dai *dai, int mute, int direction)
{
	struct es9218p_priv *es9218p = snd_soc_component_get_drvdata(dai->component);

	return regmap_update_bits(es9218p->regmap,
				  ES9218P_FILTER_BAND_SYSTEM_MUTE,
				  ES9218P_SYSTEM_MUTE,
				  mute ? ES9218P_SYSTEM_MUTE : 0);
}

static int es9218p_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	/* Only I2S slave is described by the downstream register tables. */
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) !=
	    SND_SOC_DAIFMT_CBC_CFC)
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops es9218p_dai_ops = {
	.hw_params	= es9218p_hw_params,
	.mute_stream	= es9218p_mute_stream,
	.set_fmt	= es9218p_set_fmt,
	.no_capture_mute = 1,
};

#define ES9218P_RATES	(SNDRV_PCM_RATE_8000_192000)
#define ES9218P_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
			 SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver es9218p_dai = {
	.name = "es9218p-hifi",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 2,
		.channels_max	= 2,
		.rates		= ES9218P_RATES,
		.formats	= ES9218P_FORMATS,
	},
	.ops = &es9218p_dai_ops,
};

static const struct snd_soc_component_driver es9218p_component_driver = {
	.controls		= es9218p_snd_controls,
	.num_controls		= ARRAY_SIZE(es9218p_snd_controls),
	.dapm_widgets		= es9218p_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(es9218p_dapm_widgets),
	.dapm_routes		= es9218p_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(es9218p_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static void es9218p_power_off(void *data)
{
	struct es9218p_priv *es9218p = data;

	gpiod_set_value_cansleep(es9218p->reset_gpio, 1);
	gpiod_set_value_cansleep(es9218p->power_gpio, 0);
}

static int es9218p_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct es9218p_priv *es9218p;
	unsigned int chipid;
	int ret;

	es9218p = devm_kzalloc(dev, sizeof(*es9218p), GFP_KERNEL);
	if (!es9218p)
		return -ENOMEM;

	i2c_set_clientdata(i2c, es9218p);

	es9218p->power_gpio = devm_gpiod_get_optional(dev, "power",
						      GPIOD_OUT_LOW);
	if (IS_ERR(es9218p->power_gpio))
		return dev_err_probe(dev, PTR_ERR(es9218p->power_gpio),
				     "failed to get power GPIO\n");

	es9218p->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						      GPIOD_OUT_HIGH);
	if (IS_ERR(es9218p->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(es9218p->reset_gpio),
				     "failed to get reset GPIO\n");

	/*
	 * Headphone path switch: routes the jack between the WCD9340's own
	 * output and this DAC. Left asserted while the DAC is powered.
	 */
	es9218p->hph_sw_gpio = devm_gpiod_get_optional(dev, "hph-sw",
						       GPIOD_OUT_LOW);
	if (IS_ERR(es9218p->hph_sw_gpio))
		return dev_err_probe(dev, PTR_ERR(es9218p->hph_sw_gpio),
				     "failed to get hph-sw GPIO\n");

	gpiod_set_value_cansleep(es9218p->power_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(es9218p->reset_gpio, 0);
	usleep_range(1000, 2000);

	ret = devm_add_action_or_reset(dev, es9218p_power_off, es9218p);
	if (ret)
		return ret;

	es9218p->regmap = devm_regmap_init_i2c(i2c, &es9218p_regmap_config);
	if (IS_ERR(es9218p->regmap))
		return dev_err_probe(dev, PTR_ERR(es9218p->regmap),
				     "failed to init regmap\n");

	ret = regmap_read(es9218p->regmap, ES9218P_CHIP_ID, &chipid);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip id\n");

	dev_info(dev, "ES9218P chip id 0x%02x\n", chipid);

	return devm_snd_soc_register_component(dev, &es9218p_component_driver,
					       &es9218p_dai, 1);
}

static const struct of_device_id es9218p_of_match[] = {
	{ .compatible = "ess,es9218p" },
	{ }
};
MODULE_DEVICE_TABLE(of, es9218p_of_match);

static const struct i2c_device_id es9218p_i2c_id[] = {
	{ "es9218p" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, es9218p_i2c_id);

static struct i2c_driver es9218p_i2c_driver = {
	.driver = {
		.name		= "es9218p",
		.of_match_table	= es9218p_of_match,
	},
	.probe		= es9218p_i2c_probe,
	.id_table	= es9218p_i2c_id,
};
module_i2c_driver(es9218p_i2c_driver);

MODULE_DESCRIPTION("ASoC ES9218P driver");
MODULE_LICENSE("GPL");
