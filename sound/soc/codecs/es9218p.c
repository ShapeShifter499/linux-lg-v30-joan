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
 * STATUS: working on hardware (LG V30, 2026-08-21). The part answers on BLSP1
 * QUP1 with chip id 0xd0, the value LG's own driver checks for, and plays
 * stereo audio: regmap, volume, mute, power sequencing, the register init and
 * the analog amplifier power-up are all implemented and verified, and the DAC
 * reports a locked DPLL while a stream runs.
 *
 * On joan the I2S feed is QUATERNARY MI2S, not tertiary -- tertiary carries the
 * TFA9872 loudspeaker amplifier instead. Read the dai-link that names the codec
 * (LPASS_BE_QUAT_MI2S_RX / msm-dai-q6-mi2s.3 -> es9218-codec.1-0048) rather than
 * inferring the port from a neighbouring DT node.
 *
 * Not yet done: jack detection, and a sane default volume (full scale here is
 * 0 dB into headphones and is uncomfortably loud).
 *
 * There is no ES9218P support anywhere upstream to compare against (the only
 * ESS codec in tree, es9356, is a SoundWire part and shares no bus model with
 * this one).
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
#define ES9218P_CP_SS_DELAY		0x1b	/* charge pump soft-start delay */
#define ES9218P_GEN_CONFIG		0x1d
#define ES9218P_GPIO_INVERT_CLKGEAR1	0x1e
#define ES9218P_GPIO_INVERT_CLKGEAR2	0x1f
#define ES9218P_AMP_CONFIG		0x20
#define ES9218P_AMP_MODE_HIFI1		0x02

/*
 * Analog output block.  LG's driver drives these directly with no symbolic
 * names; the bit meanings below come from the comments in its power-up
 * sequence (es9218p.c, es9218p_sabre_bypass2hifi()).
 */
#define ES9218P_ANALOG_OVERRIDE		0x2d	/* "register 45" */
#define ES9218P_APDB			BIT(2)
#define ES9218P_CPH_WEAK		BIT(3)
#define ES9218P_CPH_STRONG		BIT(4)
#define ES9218P_AREG_PDB		BIT(5)
#define ES9218P_ENHPA			BIT(6)

#define ES9218P_DIGITAL_OVERRIDE	0x2e	/* "register 46" */
#define ES9218P_SHTINB			BIT(0)	/* release amp input shunt */
#define ES9218P_SHTOUTB			BIT(1)	/* release amp output shunt */
#define ES9218P_SEL1V			BIT(2)	/* external LDO */
#define ES9218P_OVERRIDE_EN		BIT(7)

#define ES9218P_CP_OVERRIDE		0x2f	/* "register 47" */
#define ES9218P_CPL_WEAK		BIT(3)
#define ES9218P_CPL_STRONG		BIT(4)
#define ES9218P_ENCP_OE			BIT(5)
#define ES9218P_ENAUX_OE		BIT(6)

#define ES9218P_HPA_CTRL		0x30	/* "register 48" */
#define ES9218P_STATE3_CTRL_SEL		0x07	/* minimum state-machine delay */
#define ES9218P_HPAHIQ			BIT(3)
#define ES9218P_ENHPA_OUT		BIT(6)

/* ATC (analog volume) floor used while the output stage is brought up. */
#define ES9218P_ATC_MIN			0x18
#define ES9218P_CHIP_ID			0x40

#define ES9218P_MAX_REGISTER		0x45

/* ES9218P_INPUT_SELECT */
#define ES9218P_INPUT_SEL_MASK		GENMASK(7, 6)
#define ES9218P_INPUT_SEL_I2S		0
/*
 * Serial word length lives in bit 7 of the input-select register, and LG's
 * es9218p_set_bit_width() writes the whole byte rather than masking: 0x00 for
 * 16-bit, 0x80 for 24- and 32-bit.  An earlier version of this driver put the
 * field in bits 1:0, so the register kept its 0x8c reset value -- 32-bit --
 * while 16-bit frames were being sent, and the misaligned frames crackled.
 */
#define ES9218P_SERIAL_LEN_16		0x00
#define ES9218P_SERIAL_LEN_32		0x80

/* ES9218P_FILTER_BAND_SYSTEM_MUTE */
#define ES9218P_SYSTEM_MUTE		BIT(0)

struct es9218p_priv {
	struct regmap *regmap;
	bool internal_ldo;
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

/*
 * The board's analog jack switch, which selects what the headphone jack is
 * connected to.  Exposed as a control so the routing can be moved without a
 * rebuild while the destination of each analog output is still being mapped.
 */
static int es9218p_hph_sw_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct es9218p_priv *es9218p = snd_soc_component_get_drvdata(component);

	if (!es9218p->hph_sw_gpio)
		return -ENODEV;

	ucontrol->value.integer.value[0] =
		gpiod_get_value_cansleep(es9218p->hph_sw_gpio);

	return 0;
}

static int es9218p_hph_sw_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct es9218p_priv *es9218p = snd_soc_component_get_drvdata(component);
	int val = !!ucontrol->value.integer.value[0];

	if (!es9218p->hph_sw_gpio)
		return -ENODEV;

	if (gpiod_get_value_cansleep(es9218p->hph_sw_gpio) == val)
		return 0;

	gpiod_set_value_cansleep(es9218p->hph_sw_gpio, val);

	return 1;
}

static const struct snd_kcontrol_new es9218p_snd_controls[] = {
	SOC_DOUBLE_R_TLV("Headphone Playback Volume",
			 ES9218P_VOL1_CTRL, ES9218P_VOL2_CTRL,
			 0, 0xff, 1, es9218p_vol_tlv),
	SOC_SINGLE("Headphone Playback Switch",
		   ES9218P_FILTER_BAND_SYSTEM_MUTE, 0, 1, 1),
	SOC_SINGLE_BOOL_EXT("Headphone Analog Switch", 0,
			    es9218p_hph_sw_get, es9218p_hph_sw_put),
};

static int es9218p_dac_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kc, int event);

static const struct snd_soc_dapm_widget es9218p_dapm_widgets[] = {
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0,
			   es9218p_dac_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
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
	case 32:
		len = ES9218P_SERIAL_LEN_32;
		break;
	default:
		return -EINVAL;
	}

	return regmap_write(es9218p->regmap, ES9218P_INPUT_SELECT, len);
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

/*
 * Register init.  Values are LG's (es9218_common_init_registers and
 * es9218_PCM_init_register); only the entries its tables actually write are
 * carried over -- the rest of those tables is commented-out reset defaults.
 */
static const struct reg_sequence es9218p_init_seq[] = {
	{ ES9218P_OVERCURRENT_PROTECT,	0x90 },
	{ ES9218P_DPLL_BANDWIDTH,	0x8a },
	{ ES9218P_THD_COMP_MONO_MODE,	0x00 },
	{ ES9218P_SOFT_START_CONFIG,	0x07 },
	{ ES9218P_GPIO_INPUT_SEL,	0x0f },
	{ ES9218P_CP_SS_DELAY,		0xc4 },
	{ ES9218P_GPIO_INVERT_CLKGEAR1,	0x37 },
	{ ES9218P_GPIO_INVERT_CLKGEAR2,	0x30 },

	/* PCM path: DoP off, slave mode, clock divider M/4. */
	{ ES9218P_SYSTEM_REG,		0x00 },
	{ ES9218P_AUTOMUTE_CONFIG,	0x34 },
	{ ES9218P_DOP_VOL_RAMP_RATE,	0x43 },
	{ ES9218P_MASTERMODE_SYNC_CONFIG, 0x02 },
	{ ES9218P_GEN_CONFIG,		0x06 },
};

/*
 * Bring the headphone amplifier up.  This is ESS's ordering, transcribed from
 * LG's es9218p_sabre_bypass2hifi(): the charge pumps are staged weak before
 * strong, the amplifier is held shunted until its supplies are alive, and the
 * output stage is enabled last.  Skipping it leaves the part configured and
 * clocked but silent, which is exactly what we measured.
 *
 * LG steps this with a 500 ms delay per write, but that is its
 * "ESS pop-click debugging step time delay" knob, not a hardware requirement.
 * The only settle the source documents as required is the 5 ms before
 * AREG_PDB, so that one is honoured and the rest are short.
 */
static int es9218p_amp_power_up(struct es9218p_priv *es9218p)
{
	struct regmap *rm = es9218p->regmap;
	unsigned int r46 = ES9218P_OVERRIDE_EN;
	int ret;

	if (!es9218p->internal_ldo)
		r46 |= ES9218P_SEL1V;

	/* Minimum state-machine delay; HPAHiQ toggled as ESS specifies. */
	ret = regmap_write(rm, ES9218P_HPA_CTRL,
			   ES9218P_HPAHIQ | ES9218P_STATE3_CTRL_SEL);
	if (!ret)
		ret = regmap_write(rm, ES9218P_HPA_CTRL,
				   ES9218P_STATE3_CTRL_SEL);
	/* Take manual control; both amp shunts stay engaged for now. */
	if (!ret)
		ret = regmap_write(rm, ES9218P_DIGITAL_OVERRIDE, r46);
	if (!ret)
		ret = regmap_write(rm, ES9218P_AMP_CONFIG,
				   ES9218P_AMP_MODE_HIFI1);
	if (!ret)
		ret = regmap_write(rm, ES9218P_ANALOG_VOL_CTRL,
				   ES9218P_ATC_MIN);
	/* Charge pumps: weak first, then override-enable, then strong. */
	if (!ret)
		ret = regmap_write(rm, ES9218P_CP_OVERRIDE, ES9218P_CPL_WEAK);
	if (!ret)
		ret = regmap_write(rm, ES9218P_ANALOG_OVERRIDE,
				   ES9218P_CPH_WEAK);
	if (!ret)
		ret = regmap_write(rm, ES9218P_CP_OVERRIDE,
				   ES9218P_CPL_WEAK | ES9218P_ENCP_OE |
				   ES9218P_ENAUX_OE);
	if (!ret)
		ret = regmap_write(rm, ES9218P_ANALOG_OVERRIDE,
				   ES9218P_CPH_WEAK | ES9218P_APDB |
				   ES9218P_CPH_STRONG);
	if (!ret)
		ret = regmap_write(rm, ES9218P_CP_OVERRIDE,
				   ES9218P_CPL_WEAK | ES9218P_ENCP_OE |
				   ES9218P_ENAUX_OE | ES9218P_CPL_STRONG);
	if (ret)
		return ret;

	/* Vref (APDB) must settle before the AVCC_DAC regulator comes up. */
	usleep_range(5000, 6000);

	ret = regmap_write(rm, ES9218P_ANALOG_OVERRIDE,
			   ES9218P_CPH_WEAK | ES9218P_APDB |
			   ES9218P_CPH_STRONG | ES9218P_AREG_PDB |
			   ES9218P_ENHPA);
	if (!ret)
		ret = regmap_write(rm, ES9218P_DIGITAL_OVERRIDE,
				   r46 | ES9218P_SHTINB);
	if (!ret)
		ret = regmap_write(rm, ES9218P_HPA_CTRL,
				   ES9218P_STATE3_CTRL_SEL |
				   ES9218P_ENHPA_OUT);
	/* Restore the analog volume the ATC floor above displaced. */
	if (!ret)
		ret = regmap_write(rm, ES9218P_ANALOG_VOL_CTRL, 0x40);
	/*
	 * Release the output shunt and hand control back to AMP_CONFIG, which
	 * holds the part in HiFi1 from here.
	 */
	if (!ret)
		ret = regmap_write(rm, ES9218P_DIGITAL_OVERRIDE,
				   (r46 & ES9218P_SEL1V) | ES9218P_SHTINB |
				   ES9218P_SHTOUTB);
	return ret;
}

static int es9218p_amp_power_down(struct es9218p_priv *es9218p)
{
	struct regmap *rm = es9218p->regmap;
	unsigned int r46 = ES9218P_OVERRIDE_EN;

	if (!es9218p->internal_ldo)
		r46 |= ES9218P_SEL1V;

	/* Shunt the output, drop the amp, then the supplies and pumps. */
	regmap_write(rm, ES9218P_ANALOG_VOL_CTRL, ES9218P_ATC_MIN);
	regmap_write(rm, ES9218P_DIGITAL_OVERRIDE, r46);
	regmap_write(rm, ES9218P_HPA_CTRL, ES9218P_STATE3_CTRL_SEL);
	regmap_write(rm, ES9218P_ANALOG_OVERRIDE, 0);
	regmap_write(rm, ES9218P_CP_OVERRIDE, 0);
	regmap_write(rm, ES9218P_AMP_CONFIG, 0);

	return 0;
}

static int es9218p_dac_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kc, int event)
{
	struct snd_soc_component *comp = snd_soc_dapm_to_component(w->dapm);
	struct es9218p_priv *es9218p = snd_soc_component_get_drvdata(comp);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		return es9218p_amp_power_up(es9218p);
	case SND_SOC_DAPM_PRE_PMD:
		return es9218p_amp_power_down(es9218p);
	}

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

	es9218p->internal_ldo = device_property_read_bool(dev,
							  "ess,internal-ldo");

	ret = regmap_multi_reg_write(es9218p->regmap, es9218p_init_seq,
				     ARRAY_SIZE(es9218p_init_seq));
	if (ret)
		return dev_err_probe(dev, ret, "failed to apply init sequence\n");

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
