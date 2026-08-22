// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Stephan Gerhold
 *
 * Register definitions/sequences taken from various tfa98xx kernel drivers:
 * Copyright (C) 2014-2020 NXP Semiconductors, All Rights Reserved.
 * Copyright (C) 2013 Sony Mobile Communications Inc.
 */

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define TFA989X_STATUSREG		0x00
#define TFA989X_BATTERYVOLTAGE		0x01
#define TFA989X_TEMPERATURE		0x02
#define TFA989X_REVISIONNUMBER		0x03
#define TFA989X_REVISIONNUMBER_REV_MSK	GENMASK(7, 0)	/* device revision */
#define TFA989X_I2SREG			0x04
#define TFA989X_I2SREG_RCV		2	/* receiver mode */
#define TFA989X_I2SREG_CHSA		6	/* amplifier input select */
#define TFA989X_I2SREG_CHSA_MSK		GENMASK(7, 6)
#define TFA989X_I2SREG_I2SSR		12	/* sample rate */
#define TFA989X_I2SREG_I2SSR_MSK	GENMASK(15, 12)
#define TFA989X_BAT_PROT		0x05
#define TFA989X_AUDIO_CTR		0x06
#define TFA989X_DCDCBOOST		0x07
#define TFA989X_SPKR_CALIBRATION	0x08
#define TFA989X_SYS_CTRL		0x09
#define TFA989X_SYS_CTRL_PWDN		0	/* power down */
#define TFA989X_SYS_CTRL_I2CR		1	/* I2C reset */
#define TFA989X_SYS_CTRL_CFE		2	/* enable CoolFlux DSP */
#define TFA989X_SYS_CTRL_AMPE		3	/* enable amplifier */
#define TFA989X_SYS_CTRL_DCA		4	/* enable boost */
#define TFA989X_SYS_CTRL_SBSL		5	/* DSP configured */
#define TFA989X_SYS_CTRL_AMPC		6	/* amplifier enabled by DSP */
#define TFA989X_I2S_SEL_REG		0x0a
#define TFA989X_I2S_SEL_REG_SPKR_MSK	GENMASK(10, 9)	/* speaker impedance */
#define TFA989X_I2S_SEL_REG_DCFG_MSK	GENMASK(14, 11)	/* DCDC compensation */
#define TFA989X_HIDE_UNHIDE_KEY	0x40
#define TFA989X_PWM_CONTROL		0x41
#define TFA989X_CURRENTSENSE1		0x46
#define TFA989X_CURRENTSENSE2		0x47
#define TFA989X_CURRENTSENSE3		0x48
#define TFA989X_CURRENTSENSE4		0x49

#define TFA9890_REVISION		0x80
#define TFA9895_REVISION		0x12
#define TFA9897_REVISION		0x97
#define TFA9872_REVISION		0x72

/*
 * The TFA9872 reaches its hidden registers through a different key register
 * than the older parts (0x0f, not TFA989X_HIDE_UNHIDE_KEY at 0x40), and needs
 * a second, challenge-response key on top: read 0xfb, xor, write to 0xa0.
 */
#define TFA9872_KEY1			0x0f
#define TFA9872_KEY1_VAL		0x5a6b
#define TFA9872_KEY2_CHALLENGE		0xfb
#define TFA9872_KEY2			0xa0
#define TFA9872_KEY2_XOR		0x005a
#define TFA9872_MANAOOSC		0x01	/* bit 4: 1 MHz oscillator off */
#define TFA9872_OVP			0xb0	/* bit 3: bypass over-voltage protection */

/*
 * The TFA9872 ("Probus") does not merely repurpose the low registers: system
 * control, sample rate and status all live at different addresses than on the
 * older TFA1 parts, and there is no CoolFlux DSP at all.  Writing the TFA989X_*
 * offsets on this part is silently ignored - the amplifier then stays in its
 * power-on default (PWDN set) and never produces any sound.  Field positions
 * are from NXP's TFA9872N1B2 register definitions as shipped by LG.
 */
#define TFA9872_SYS_CONTROL0		0x00
#define TFA9872_SYS_CONTROL0_PWDN	0
#define TFA9872_SYS_CONTROL0_I2CR	1	/* I2C reset - auto clear */
#define TFA9872_SYS_CONTROL1		0x01
#define TFA9872_SYS_CONTROL1_MANSCONF	2	/* "I2C configured", starts the PLL */
#define TFA9872_AUDIO_CONTROL		0x02
#define TFA9872_AUDIO_CONTROL_AUDFS_MSK	GENMASK(3, 0)
#define TFA9872_STATUS_FLAGS0		0x10
#define TFA9872_STATUS_FLAGS0_SWS	10	/* amplifier engaged */
#define TFA9872_TDM_CONFIG1		0x21
#define TFA9872_TDM_CONFIG1_SLLN_MSK	GENMASK(8, 4)	/* bits per slot, minus one */

/* The manager needs a few tries to leave "wait for I2C settings" state. */
#define TFA9872_START_RETRIES		20

/*
 * TDMSPKG, the amplifier's own level control.  NXP's header calls it "total
 * gain", but it behaves as an attenuation on this part: 0 is loudest and 15 is
 * quietest, measured by ear on an LG V30.  Exposed inverted so the mixer
 * behaves like a volume.  The per-step size is not documented in anything we
 * have, so no dB scale is attached.
 */
#define TFA9872_TDM_SPKG		0x61
#define TFA9872_TDM_SPKG_SHIFT		6
#define TFA9872_TDM_SPKG_MAX		15

struct tfa989x_rev {
	unsigned int rev;
	int (*init)(struct regmap *regmap);
	/*
	 * Later parts (TFA9872) repurpose the registers below
	 * TFA989X_REVISIONNUMBER, which are status-only on the older ones.
	 */
	bool low_regs_writeable;
	/*
	 * Probus parts (TFA9872) use the register map above and are brought up
	 * by their own hardware manager rather than by writing AMPE directly.
	 */
	bool probus;
};

struct tfa989x {
	const struct tfa989x_rev *rev;
	struct regmap *regmap;
	struct regulator *vddd_supply;
	struct gpio_desc *rcv_gpiod;
};

static bool tfa989x_writeable_reg(struct device *dev, unsigned int reg)
{
	return reg > TFA989X_REVISIONNUMBER;
}

static bool tfa989x_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg < TFA989X_REVISIONNUMBER;
}

static bool tfa989x_writeable_reg_all(struct device *dev, unsigned int reg)
{
	return true;
}

static const struct regmap_config tfa989x_regmap = {
	.reg_bits = 8,
	.val_bits = 16,

	.writeable_reg	= tfa989x_writeable_reg,
	.volatile_reg	= tfa989x_volatile_reg,
	.cache_type	= REGCACHE_RBTREE,
};

/* Same, but for parts whose low registers are not status-only. */
static const struct regmap_config tfa989x_regmap_low_rw = {
	.reg_bits	= 8,
	.val_bits	= 16,

	.writeable_reg	= tfa989x_writeable_reg_all,
	.volatile_reg	= tfa989x_volatile_reg,
	.cache_type	= REGCACHE_RBTREE,
};

/*
 * The device manager clears PWDN and MANSCONF by itself whenever it falls back
 * to waiting for the host, so those registers must never be served from the
 * cache - otherwise regmap would skip the write that restarts the amplifier.
 */
static bool tfa9872_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg <= TFA989X_REVISIONNUMBER ||
	       (reg >= TFA9872_STATUS_FLAGS0 && reg <= 0x14);
}

static const struct regmap_config tfa9872_regmap = {
	.reg_bits	= 8,
	.val_bits	= 16,

	.writeable_reg	= tfa989x_writeable_reg_all,
	.volatile_reg	= tfa9872_volatile_reg,
	.cache_type	= REGCACHE_RBTREE,
};

static const char * const chsa_text[] = { "Left", "Right", /* "DSP" */ };
static SOC_ENUM_SINGLE_DECL(chsa_enum, TFA989X_I2SREG, TFA989X_I2SREG_CHSA, chsa_text);
static const struct snd_kcontrol_new chsa_mux = SOC_DAPM_ENUM("Amp Input", chsa_enum);

static const struct snd_soc_dapm_widget tfa989x_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_SUPPLY("POWER", TFA989X_SYS_CTRL, TFA989X_SYS_CTRL_PWDN, 1, NULL, 0),
	SND_SOC_DAPM_OUT_DRV("AMPE", TFA989X_SYS_CTRL, TFA989X_SYS_CTRL_AMPE, 0, NULL, 0),

	SND_SOC_DAPM_MUX("Amp Input", SND_SOC_NOPM, 0, 0, &chsa_mux),
	SND_SOC_DAPM_AIF_IN("AIFINL", "HiFi Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIFINR", "HiFi Playback", 1, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route tfa989x_dapm_routes[] = {
	{"OUT", NULL, "AMPE"},
	{"AMPE", NULL, "POWER"},
	{"AMPE", NULL, "Amp Input"},
	{"Amp Input", "Left", "AIFINL"},
	{"Amp Input", "Right", "AIFINR"},
};

static const struct snd_kcontrol_new tfa9872_snd_controls[] = {
	SOC_SINGLE("Speaker Playback Volume", TFA9872_TDM_SPKG,
		   TFA9872_TDM_SPKG_SHIFT, TFA9872_TDM_SPKG_MAX, 1),
};

static const struct snd_soc_dapm_widget tfa9872_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_AIF_IN("AIFINL", "HiFi Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIFINR", "HiFi Playback", 1, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route tfa9872_dapm_routes[] = {
	{"OUT", NULL, "AIFINL"},
	{"OUT", NULL, "AIFINR"},
};

static int tfa989x_put_mode(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa989x *tfa989x = snd_soc_component_get_drvdata(component);

	gpiod_set_value_cansleep(tfa989x->rcv_gpiod, ucontrol->value.enumerated.item[0]);

	return snd_soc_put_enum_double(kcontrol, ucontrol);
}

static const char * const mode_text[] = { "Speaker", "Receiver" };
static SOC_ENUM_SINGLE_DECL(mode_enum, TFA989X_I2SREG, TFA989X_I2SREG_RCV, mode_text);
static const struct snd_kcontrol_new tfa989x_mode_controls[] = {
	SOC_ENUM_EXT("Mode", mode_enum, snd_soc_get_enum_double, tfa989x_put_mode),
};

static int tfa989x_probe(struct snd_soc_component *component)
{
	struct tfa989x *tfa989x = snd_soc_component_get_drvdata(component);

	if (tfa989x->rev->rev == TFA9897_REVISION)
		return snd_soc_add_component_controls(component, tfa989x_mode_controls,
						      ARRAY_SIZE(tfa989x_mode_controls));

	return 0;
}

static const struct snd_soc_component_driver tfa9872_component = {
	.probe			= tfa989x_probe,
	.controls		= tfa9872_snd_controls,
	.num_controls		= ARRAY_SIZE(tfa9872_snd_controls),
	.dapm_widgets		= tfa9872_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tfa9872_dapm_widgets),
	.dapm_routes		= tfa9872_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tfa9872_dapm_routes),
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const struct snd_soc_component_driver tfa989x_component = {
	.probe			= tfa989x_probe,
	.dapm_widgets		= tfa989x_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(tfa989x_dapm_widgets),
	.dapm_routes		= tfa989x_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(tfa989x_dapm_routes),
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static const unsigned int tfa989x_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

static int tfa989x_find_sample_rate(unsigned int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(tfa989x_rates); ++i)
		if (tfa989x_rates[i] == rate)
			return i;

	return -EINVAL;
}

static int tfa989x_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tfa989x *tfa989x = snd_soc_component_get_drvdata(component);
	int sr, ret;

	sr = tfa989x_find_sample_rate(params_rate(params));
	if (sr < 0)
		return sr;

	if (!tfa989x->rev->probus)
		return snd_soc_component_update_bits(component, TFA989X_I2SREG,
						     TFA989X_I2SREG_I2SSR_MSK,
						     sr << TFA989X_I2SREG_I2SSR);

	ret = regmap_update_bits(tfa989x->regmap, TFA9872_AUDIO_CONTROL,
				 TFA9872_AUDIO_CONTROL_AUDFS_MSK, sr);
	if (ret)
		return ret;

	/*
	 * The reset default is 32 bits per slot, which does not fit the two
	 * slots of the 32 BCK frame the CPU DAI sends for S16_LE stereo.  Left
	 * that way the amplifier raises TDMERR and drops out of its operating
	 * state roughly 80 ms after starting.
	 */
	return regmap_update_bits(tfa989x->regmap, TFA9872_TDM_CONFIG1,
				  TFA9872_TDM_CONFIG1_SLLN_MSK,
				  FIELD_PREP(TFA9872_TDM_CONFIG1_SLLN_MSK,
					     params_width(params) - 1));
}

/*
 * Probus parts are started by their own hardware manager: clearing PWDN only
 * arms it, and setting MANSCONF ("I2C configured") is what lets it start the
 * PLL and engage the amplifier.  AMPE is deliberately never written here -
 * setting it before MANSCONF makes the manager clear it again and refuse to
 * leave the "wait for I2C settings" state.
 */
static int tfa9872_amp_start(struct tfa989x *tfa989x)
{
	struct regmap *regmap = tfa989x->regmap;
	unsigned int val;
	int ret, i;

	for (i = 0; i < TFA9872_START_RETRIES; i++) {
		ret = regmap_clear_bits(regmap, TFA9872_SYS_CONTROL0,
					BIT(TFA9872_SYS_CONTROL0_PWDN));
		if (ret)
			return ret;

		/* MANSCONF does not read back as written */
		ret = regmap_set_bits(regmap, TFA9872_SYS_CONTROL1,
				      BIT(TFA9872_SYS_CONTROL1_MANSCONF));
		if (ret)
			return ret;

		usleep_range(1000, 2000);

		ret = regmap_read(regmap, TFA9872_STATUS_FLAGS0, &val);
		if (ret)
			return ret;

		if (val & BIT(TFA9872_STATUS_FLAGS0_SWS))
			return 0;
	}

	return -ETIMEDOUT;
}

static int tfa9872_amp_stop(struct tfa989x *tfa989x)
{
	return regmap_set_bits(tfa989x->regmap, TFA9872_SYS_CONTROL0,
			       BIT(TFA9872_SYS_CONTROL0_PWDN));
}

static int tfa989x_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct tfa989x *tfa989x = snd_soc_component_get_drvdata(component);
	int ret;

	if (!tfa989x->rev->probus)
		return 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = tfa9872_amp_start(tfa989x);
		if (ret)
			dev_err(component->dev,
				"amplifier failed to start: %d\n", ret);
		return ret;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		return tfa9872_amp_stop(tfa989x);
	}

	return 0;
}

static const struct snd_soc_dai_ops tfa989x_dai_ops = {
	.hw_params = tfa989x_hw_params,
	.trigger = tfa989x_trigger,
};

static struct snd_soc_dai_driver tfa989x_dai = {
	.name = "tfa989x-hifi",
	.playback = {
		.stream_name	= "HiFi Playback",
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
		.rates		= SNDRV_PCM_RATE_8000_48000,
		.rate_min	= 8000,
		.rate_max	= 48000,
		.channels_min	= 1,
		.channels_max	= 2,
	},
	.ops = &tfa989x_dai_ops,
};

static int tfa9890_init(struct regmap *regmap)
{
	int ret;

	/* temporarily allow access to hidden registers */
	ret = regmap_write(regmap, TFA989X_HIDE_UNHIDE_KEY, 0x5a6b);
	if (ret)
		return ret;

	/* update PLL registers */
	ret = regmap_set_bits(regmap, 0x59, 0x3);
	if (ret)
		return ret;

	/* hide registers again */
	ret = regmap_write(regmap, TFA989X_HIDE_UNHIDE_KEY, 0x0000);
	if (ret)
		return ret;

	return regmap_write(regmap, TFA989X_CURRENTSENSE2, 0x7BE1);
}

static const struct tfa989x_rev tfa9890_rev = {
	.rev	= TFA9890_REVISION,
	.init	= tfa9890_init,
};

static const struct reg_sequence tfa9895_reg_init[] = {
	/* some other registers must be set for optimal amplifier behaviour */
	{ TFA989X_BAT_PROT, 0x13ab },
	{ TFA989X_AUDIO_CTR, 0x001f },

	/* peak voltage protection is always on, but may be written */
	{ TFA989X_SPKR_CALIBRATION, 0x3c4e },

	/* TFA989X_SYSCTRL_DCA = 0 */
	{ TFA989X_SYS_CTRL, 0x024d },
	{ TFA989X_PWM_CONTROL, 0x0308 },
	{ TFA989X_CURRENTSENSE4, 0x0e82 },
};

static int tfa9895_init(struct regmap *regmap)
{
	return regmap_multi_reg_write(regmap, tfa9895_reg_init,
				      ARRAY_SIZE(tfa9895_reg_init));
}

static const struct tfa989x_rev tfa9895_rev = {
	.rev	= TFA9895_REVISION,
	.init	= tfa9895_init,
};

static int tfa9897_init(struct regmap *regmap)
{
	int ret;

	/* Reduce slewrate by clearing iddqtestbst to avoid booster damage */
	ret = regmap_write(regmap, TFA989X_CURRENTSENSE3, 0x0300);
	if (ret)
		return ret;

	/* Enable clipping */
	ret = regmap_clear_bits(regmap, TFA989X_CURRENTSENSE4, 0x1);
	if (ret)
		return ret;

	/* Set required TDM configuration */
	return regmap_write(regmap, 0x14, 0x0);
}

/*
 * Register defaults for the N1B2 die (revisions 0x1b72, 0x2b72, 0x3b72), from
 * tfa9872_specific() in NXP's vendor driver as shipped by LG.  These are the
 * deviations from power-on reset that the part needs for correct amplifier
 * behaviour; the vendor source lists the POR value beside each one.
 *
 * The table writes 0x02 and the tail touches 0x01, both of which are
 * status-only on the older parts; the 9872 repurposes them, which is what
 * low_regs_writeable selects a permissive regmap for.
 */
static const struct reg_sequence tfa9872_reg_init[] = {
	{ 0x02, 0x2dc8 },
	{ 0x20, 0x0890 },
	{ 0x22, 0x043c },
	{ 0x23, 0x0001 },
	{ 0x51, 0x0000 },
	{ 0x52, 0x5a1c },
	{ 0x61, 0x0198 },
	{ 0x63, 0x0a9a },
	{ 0x65, 0x0a82 },
	{ 0x6f, 0x01e3 },
	{ 0x70, 0x06fd },
	{ 0x71, 0x307e },
	{ 0x74, 0xcc84 },
	{ 0x75, 0x1132 },
	{ 0x82, 0x01ed },
	{ 0x83, 0x001a },
};

static int tfa9872_init(struct regmap *regmap)
{
	unsigned int val;
	int ret;

	/* Unlock the hidden registers: fixed key, then challenge-response. */
	ret = regmap_write(regmap, TFA9872_KEY1, TFA9872_KEY1_VAL);
	if (ret)
		return ret;

	ret = regmap_read(regmap, TFA9872_KEY2_CHALLENGE, &val);
	if (ret)
		return ret;

	ret = regmap_write(regmap, TFA9872_KEY2, val ^ TFA9872_KEY2_XOR);
	if (ret)
		return ret;

	ret = regmap_multi_reg_write(regmap, tfa9872_reg_init,
				     ARRAY_SIZE(tfa9872_reg_init));
	if (ret)
		return ret;

	/* Turn the 1 MHz oscillator off to save power. */
	ret = regmap_set_bits(regmap, TFA9872_MANAOOSC, BIT(4));
	if (ret)
		return ret;

	/* Bypass over-voltage protection, as the vendor driver does. */
	return regmap_set_bits(regmap, TFA9872_OVP, BIT(3));
}

static const struct tfa989x_rev tfa9872_rev = {
	.rev			= TFA9872_REVISION,
	.init			= tfa9872_init,
	.low_regs_writeable	= true,
	.probus			= true,
};

static const struct tfa989x_rev tfa9897_rev = {
	.rev	= TFA9897_REVISION,
	.init	= tfa9897_init,
};

/*
 * Note: At the moment this driver bypasses the "CoolFlux DSP" built into the
 * TFA989X amplifiers. Unfortunately, there seems to be absolutely
 * no documentation for it - the public "short datasheets" do not provide
 * any information about the DSP or available registers.
 *
 * Usually the TFA989X amplifiers are configured through proprietary userspace
 * libraries. There are also some (rather complex) kernel drivers but even those
 * rely on obscure firmware blobs for configuration (so-called "containers").
 * They seem to contain different "profiles" with tuned speaker settings, sample
 * rates and volume steps (which would be better exposed as separate ALSA mixers).
 *
 * Bypassing the DSP disables volume control (and perhaps some speaker
 * optimization?), but at least allows using the speaker without obscure
 * kernel drivers and firmware.
 *
 * Ideally NXP (or now Goodix) should release proper documentation for these
 * amplifiers so that support for the "CoolFlux DSP" can be implemented properly.
 *
 * None of the above applies to the TFA9872, which has no CoolFlux DSP to
 * bypass in the first place: it is one of NXP's "Probus" parts, and its
 * register map contains none of the DSP interface fields (CFE, SBSL, ACS,
 * DMEM, MADD, MEMA, RST) that the parts with a DSP expose.  Its amplifier is
 * brought up by an on-chip manager instead - see tfa9872_amp_start().  On
 * Probus parts the speaker protection algorithm runs off-chip, which on
 * Qualcomm platforms means a module inside the ADSP firmware, so there is no
 * on-chip DSP for this driver to talk to either way.
 */
static int tfa989x_dsp_bypass(struct regmap *regmap)
{
	int ret;

	/* Clear CHSA to bypass DSP and take input from I2S 1 left channel */
	ret = regmap_clear_bits(regmap, TFA989X_I2SREG, TFA989X_I2SREG_CHSA_MSK);
	if (ret)
		return ret;

	/* Set DCDC compensation to off and speaker impedance to 8 ohm */
	ret = regmap_update_bits(regmap, TFA989X_I2S_SEL_REG,
				 TFA989X_I2S_SEL_REG_DCFG_MSK |
				 TFA989X_I2S_SEL_REG_SPKR_MSK,
				 TFA989X_I2S_SEL_REG_SPKR_MSK);
	if (ret)
		return ret;

	/* Set DCDC to follower mode and disable CoolFlux DSP */
	return regmap_clear_bits(regmap, TFA989X_SYS_CTRL,
				 BIT(TFA989X_SYS_CTRL_DCA) |
				 BIT(TFA989X_SYS_CTRL_CFE) |
				 BIT(TFA989X_SYS_CTRL_AMPC));
}

static void tfa989x_regulator_disable(void *data)
{
	struct tfa989x *tfa989x = data;

	regulator_disable(tfa989x->vddd_supply);
}

static int tfa989x_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	const struct tfa989x_rev *rev;
	struct tfa989x *tfa989x;
	struct regmap *regmap;
	unsigned int val;
	int ret;

	rev = device_get_match_data(dev);
	if (!rev) {
		dev_err(dev, "unknown device revision\n");
		return -ENODEV;
	}

	tfa989x = devm_kzalloc(dev, sizeof(*tfa989x), GFP_KERNEL);
	if (!tfa989x)
		return -ENOMEM;

	tfa989x->rev = rev;
	i2c_set_clientdata(i2c, tfa989x);

	tfa989x->vddd_supply = devm_regulator_get(dev, "vddd");
	if (IS_ERR(tfa989x->vddd_supply))
		return dev_err_probe(dev, PTR_ERR(tfa989x->vddd_supply),
				     "Failed to get vddd regulator\n");

	if (tfa989x->rev->rev == TFA9897_REVISION) {
		tfa989x->rcv_gpiod = devm_gpiod_get_optional(dev, "rcv", GPIOD_OUT_LOW);
		if (IS_ERR(tfa989x->rcv_gpiod))
			return PTR_ERR(tfa989x->rcv_gpiod);
	}

	if (rev->probus)
		regmap = devm_regmap_init_i2c(i2c, &tfa9872_regmap);
	else
		regmap = devm_regmap_init_i2c(i2c,
					      rev->low_regs_writeable ?
					      &tfa989x_regmap_low_rw :
					      &tfa989x_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	tfa989x->regmap = regmap;

	ret = regulator_enable(tfa989x->vddd_supply);
	if (ret) {
		dev_err(dev, "Failed to enable vddd regulator: %d\n", ret);
		return ret;
	}

	ret = devm_add_action_or_reset(dev, tfa989x_regulator_disable, tfa989x);
	if (ret)
		return ret;

	/* Bypass regcache for reset and init sequence */
	regcache_cache_bypass(regmap, true);

	/* Dummy read to generate i2c clocks, required on some devices */
	regmap_read(regmap, TFA989X_REVISIONNUMBER, &val);

	ret = regmap_read(regmap, TFA989X_REVISIONNUMBER, &val);
	if (ret) {
		dev_err(dev, "failed to read revision number: %d\n", ret);
		return ret;
	}

	val &= TFA989X_REVISIONNUMBER_REV_MSK;
	if (val != rev->rev) {
		dev_err(dev, "invalid revision number, expected %#x, got %#x\n",
			rev->rev, val);
		return -ENODEV;
	}

	if (rev->probus)
		ret = regmap_write(regmap, TFA9872_SYS_CONTROL0,
				   BIT(TFA9872_SYS_CONTROL0_I2CR));
	else
		ret = regmap_write(regmap, TFA989X_SYS_CTRL,
				   BIT(TFA989X_SYS_CTRL_I2CR));
	if (ret) {
		dev_err(dev, "failed to reset I2C registers: %d\n", ret);
		return ret;
	}

	ret = rev->init(regmap);
	if (ret) {
		dev_err(dev, "failed to initialize registers: %d\n", ret);
		return ret;
	}

	/* Probus parts have no CoolFlux DSP, and none of these registers. */
	if (!rev->probus) {
		ret = tfa989x_dsp_bypass(regmap);
		if (ret) {
			dev_err(dev, "failed to enable DSP bypass: %d\n", ret);
			return ret;
		}
	}
	regcache_cache_bypass(regmap, false);

	return devm_snd_soc_register_component(dev,
					       rev->probus ? &tfa9872_component
						           : &tfa989x_component,
					       &tfa989x_dai, 1);
}

static const struct of_device_id tfa989x_of_match[] = {
	{ .compatible = "nxp,tfa9872", .data = &tfa9872_rev },
	{ .compatible = "nxp,tfa9890", .data = &tfa9890_rev },
	{ .compatible = "nxp,tfa9895", .data = &tfa9895_rev },
	{ .compatible = "nxp,tfa9897", .data = &tfa9897_rev },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa989x_of_match);

static struct i2c_driver tfa989x_i2c_driver = {
	.driver = {
		.name = "tfa989x",
		.of_match_table = tfa989x_of_match,
	},
	.probe = tfa989x_i2c_probe,
};
module_i2c_driver(tfa989x_i2c_driver);

MODULE_DESCRIPTION("ASoC NXP/Goodix TFA989X (TFA1) driver");
MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_LICENSE("GPL");
