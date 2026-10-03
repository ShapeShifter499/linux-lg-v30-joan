// SPDX-License-Identifier: GPL-2.0-only
/*
 * SLIMbus audio path for the Qualcomm WCN399x Bluetooth/FM combination
 * chip, as used on the LG V30 (joan).
 *
 * The WCN3990 exposes two SLIMbus devices on the msm8998's second NGD
 * engine: a ported generic device (PGD, device index 1) that owns the
 * slave ports, and its interface device (IFD, device index 0) carrying
 * the port configuration registers at offset 0x800.  The FM receiver
 * inside the chip digitises the analogue baseband at 48 kHz and streams
 * it out through the two FM TX slave ports (PGD ports 1 and 2) as a
 * two-channel group, which the ADSP receives on its SLIMBUS_8_TX AFE
 * port.  The remaining ports (TX 0, RX 16/17) carry SCO/A2DP audio.
 *
 * Ported from the Qualcomm/LG Android kernel sources
 * (android_kernel_lge_msm8998, Copyright (c) 2016-2017, The Linux
 * Foundation, GPL-2.0):
 *
 *   drivers/bluetooth/btfm_slim.c         - SLIMbus client (PGD/IFD element
 *                                           access, logical addresses)
 *   drivers/bluetooth/btfm_slim_codec.c   - ASoC DAIs and channel mapping
 *   drivers/bluetooth/btfm_slim_wcn3990.c - WCN3990 port map and port
 *                                           enable/disable programming
 *
 * adapted to the mainline slimbus stream API and the modern
 * snd_soc_component interface.  The chip must be powered (Bluetooth
 * controller up / FM receiver on) before a stream can run, exactly as
 * downstream; the FM receiver driver takes care of that part.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slimbus.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* WCN3990 interface-device register block (downstream btfm_slim_wcn3990.h) */
#define WCN3990_SLIM_REG_OFFSET			0x0800

#define WCN3990_SB_SLAVE_HW_REV_MSB		0x0001
#define WCN3990_SB_SLAVE_HW_REV_LSB		0x0002

#define WCN3990_SB_PGD_TX_PORT_CFG(n)		(0x0050 + (n))
#define WCN3990_SB_PGD_RX_PORT_CFG(n)		(0x0040 + (n))
#define WCN3990_SB_PGD_TX_PORT_MULTI_CH_0(n)	(0x0100 + 0x4 * (n))
#define WCN3990_SB_PGD_RX_PORT_MULTI_CH_0(n)	(0x0180 + 0x4 * (n))
#define WCN3990_SB_PGD_TX_PORT_OR_UR_CFG(n)	(0x01f0 + (n))

#define WCN3990_PORT_ENABLE			BIT(0)
#define WCN3990_PORT_WM_L1			(0x1 << 1)
#define WCN3990_PORT_WM_L8			(0x8 << 1)
#define WCN3990_PORT_WM_LB			(0xb << 1)
#define WCN3990_OVERRUN_AUTO_RECOVERY		BIT(1)
#define WCN3990_UNDERRUN_AUTO_RECOVERY		BIT(0)

/* WCN3990 PGD port assignment (downstream btfm_slim_wcn3990.h) */
#define WCN3990_PORT_TX_SCO			0
#define WCN3990_PORT_TX1_FM			1
#define WCN3990_PORT_TX2_FM			2
#define WCN3990_PORT_RX_SCO			16
#define WCN3990_PORT_RX_A2P			17

/* Enumeration address of the WCN3990 devices (downstream msm8998.dtsi) */
#define WCN3990_MANF_ID				0x0217
#define WCN3990_PROD_CODE			0x0220

enum {
	BTFM_FM_SLIM_TX = 0,
	BTFM_BT_SCO_SLIM_TX,
	BTFM_BT_SCO_A2DP_SLIM_RX,
	BTFM_BT_SPLIT_A2DP_SLIM_RX,
	BTFM_SLIM_NUM_DAIS,
};

struct btfm_slim_ch {
	u16 port;
	unsigned int ch_num;
};

struct btfm_slim {
	struct slim_device *sdev;	/* ported generic device */
	struct slim_device *sidev;	/* interface device */
	struct mutex io_lock;
	bool enabled;
	bool channels_set;
	/* ADSP channel allocation handed over by the machine driver */
	struct btfm_slim_ch tx_chs[3];
	struct btfm_slim_ch rx_chs[2];
	unsigned int rate;
	unsigned int ch_count;
	struct slim_stream_runtime *sruntime;
};

static int btfm_slim_write_ifd(struct btfm_slim *btfm, u16 reg, u8 val)
{
	int ret;

	mutex_lock(&btfm->io_lock);
	ret = slim_write(btfm->sidev, WCN3990_SLIM_REG_OFFSET + reg, 1, &val);
	mutex_unlock(&btfm->io_lock);

	if (ret)
		dev_err(&btfm->sdev->dev, "IFD write 0x%04x failed: %d\n",
			reg, ret);

	return ret;
}

static int btfm_slim_read_ifd(struct btfm_slim *btfm, u16 reg, u8 *val)
{
	int ret;

	mutex_lock(&btfm->io_lock);
	ret = slim_read(btfm->sidev, WCN3990_SLIM_REG_OFFSET + reg, 1, val);
	mutex_unlock(&btfm->io_lock);

	if (ret)
		dev_err(&btfm->sdev->dev, "IFD read 0x%04x failed: %d\n",
			reg, ret);

	return ret;
}

/*
 * Probe the slave (downstream btfm_slim_chrk_hw_init).  The chip only
 * answers once it is powered, so this runs at DAI startup, not at
 * slimbus device probe.
 */
static int btfm_slim_hw_init(struct btfm_slim *btfm)
{
	u8 rev_msb, rev_lsb;
	int ret;

	if (btfm->enabled)
		return 0;

	ret = btfm_slim_read_ifd(btfm, WCN3990_SB_SLAVE_HW_REV_MSB, &rev_msb);
	if (ret)
		return ret;

	ret = btfm_slim_read_ifd(btfm, WCN3990_SB_SLAVE_HW_REV_LSB, &rev_lsb);
	if (ret)
		return ret;

	dev_dbg(&btfm->sdev->dev, "WCN3990 HW rev %u.%u.%u\n",
		(rev_msb & 0xf0) >> 4, rev_msb & 0x0f, rev_lsb);

	btfm->enabled = true;

	return 0;
}

static void btfm_slim_hw_deinit(struct btfm_slim *btfm)
{
	btfm->enabled = false;
}

/*
 * Slave port programming, ported from downstream
 * btfm_slim_chrk_enable_port(): the multi-channel register selects the
 * local channel slots feeding the port (TX slots equal the port number,
 * RX slots count from port 16), the OR/UR register arms the automatic
 * underrun/overrun recovery, and the port config register sets the
 * watermark before enabling the port.
 */
static int btfm_slim_enable_fm_port(struct btfm_slim *btfm, u16 port,
				    bool enable)
{
	u8 val;
	int ret;

	/* the two FM TX ports share the two-channel group */
	val = BIT(WCN3990_PORT_TX1_FM) | BIT(WCN3990_PORT_TX2_FM);
	ret = btfm_slim_write_ifd(btfm,
				  WCN3990_SB_PGD_TX_PORT_MULTI_CH_0(port), val);
	if (ret)
		return ret;

	if (enable) {
		val = WCN3990_OVERRUN_AUTO_RECOVERY |
		      WCN3990_UNDERRUN_AUTO_RECOVERY;
		ret = btfm_slim_write_ifd(btfm,
				WCN3990_SB_PGD_TX_PORT_OR_UR_CFG(port), val);
		if (ret)
			return ret;

		val = WCN3990_PORT_ENABLE | WCN3990_PORT_WM_L8;
	} else {
		val = 0;
	}

	return btfm_slim_write_ifd(btfm, WCN3990_SB_PGD_TX_PORT_CFG(port), val);
}

static int btfm_slim_enable_sco_tx_port(struct btfm_slim *btfm, bool enable)
{
	u8 val;
	int ret;

	val = BIT(WCN3990_PORT_TX_SCO);
	ret = btfm_slim_write_ifd(btfm,
			WCN3990_SB_PGD_TX_PORT_MULTI_CH_0(WCN3990_PORT_TX_SCO),
			val);
	if (ret)
		return ret;

	if (enable) {
		val = WCN3990_OVERRUN_AUTO_RECOVERY |
		      WCN3990_UNDERRUN_AUTO_RECOVERY;
		ret = btfm_slim_write_ifd(btfm,
			WCN3990_SB_PGD_TX_PORT_OR_UR_CFG(WCN3990_PORT_TX_SCO),
			val);
		if (ret)
			return ret;

		val = WCN3990_PORT_ENABLE | WCN3990_PORT_WM_L1;
	} else {
		val = 0;
	}

	return btfm_slim_write_ifd(btfm,
			WCN3990_SB_PGD_TX_PORT_CFG(WCN3990_PORT_TX_SCO), val);
}

static int btfm_slim_enable_rx_port(struct btfm_slim *btfm, u16 port,
				    bool enable)
{
	u8 val = 0;
	int ret;

	if (enable) {
		val = WCN3990_PORT_ENABLE | WCN3990_PORT_WM_LB;
		ret = btfm_slim_write_ifd(btfm,
			WCN3990_SB_PGD_RX_PORT_MULTI_CH_0(port - 16), BIT(0));
		if (ret)
			return ret;
	}

	return btfm_slim_write_ifd(btfm, WCN3990_SB_PGD_RX_PORT_CFG(port - 16),
				   val);
}

static int btfm_slim_enable_ports(struct btfm_slim *btfm, int dai_id,
				  bool enable)
{
	switch (dai_id) {
	case BTFM_FM_SLIM_TX:
		return btfm_slim_enable_fm_port(btfm, WCN3990_PORT_TX1_FM,
						enable) ?:
		       btfm_slim_enable_fm_port(btfm, WCN3990_PORT_TX2_FM,
						enable);
	case BTFM_BT_SCO_SLIM_TX:
		return btfm_slim_enable_sco_tx_port(btfm, enable);
	case BTFM_BT_SCO_A2DP_SLIM_RX:
		return btfm_slim_enable_rx_port(btfm, WCN3990_PORT_RX_SCO,
						enable);
	case BTFM_BT_SPLIT_A2DP_SLIM_RX:
		return btfm_slim_enable_rx_port(btfm, WCN3990_PORT_RX_A2P,
						enable);
	default:
		return -EINVAL;
	}
}

static int btfm_slim_dai_startup(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);

	dev_dbg(dai->dev, "%s: dai %s\n", __func__, dai->name);

	return btfm_slim_hw_init(btfm);
}

static void btfm_slim_dai_shutdown(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);

	if (btfm->sruntime) {
		slim_stream_disable(btfm->sruntime);
		slim_stream_unprepare(btfm->sruntime);
		slim_stream_free(btfm->sruntime);
		btfm->sruntime = NULL;
	}

	btfm_slim_enable_ports(btfm, dai->id, false);
	btfm_slim_hw_deinit(btfm);
}

static int btfm_slim_dai_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);

	btfm->rate = params_rate(params);

	dev_dbg(dai->dev, "%s: dai %s rate %u ch %u\n", __func__, dai->name,
		btfm->rate, params_channels(params));

	/*
	 * The FM group always carries both TX ports, exactly as
	 * downstream (grp = true, nchan = 2); a mono FE taps one of the
	 * two channels of the group at the ADM routing stage.
	 */
	btfm->ch_count = (dai->id == BTFM_FM_SLIM_TX) ? 2 : 1;

	return 0;
}

static int btfm_slim_dai_prepare(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);
	struct slim_stream_config cfg = {0};
	const struct btfm_slim_ch *chs;
	int nports, i, ret;

	switch (dai->id) {
	case BTFM_FM_SLIM_TX:
		chs = &btfm->tx_chs[0];
		nports = 2;
		break;
	case BTFM_BT_SCO_SLIM_TX:
		chs = &btfm->tx_chs[2];
		nports = 1;
		break;
	case BTFM_BT_SCO_A2DP_SLIM_RX:
		chs = &btfm->rx_chs[0];
		nports = 1;
		break;
	case BTFM_BT_SPLIT_A2DP_SLIM_RX:
		chs = &btfm->rx_chs[1];
		nports = 1;
		break;
	default:
		return -EINVAL;
	}

	if (!btfm->channels_set) {
		dev_err(dai->dev, "no channel map assigned for %s\n",
			dai->name);
		return -EINVAL;
	}

	/* one shared stream state: a second open would leak the first */
	if (btfm->sruntime) {
		dev_err(dai->dev, "%s: stream already active\n", dai->name);
		return -EBUSY;
	}

	cfg.rate = btfm->rate;
	cfg.bps = 16;
	cfg.ch_count = btfm->ch_count;
	cfg.direction = substream->stream;
	cfg.port_mask = 0;
	cfg.chs = kcalloc(btfm->ch_count, sizeof(*cfg.chs), GFP_KERNEL);
	if (!cfg.chs)
		return -ENOMEM;

	for (i = 0; i < btfm->ch_count && i < nports; i++) {
		cfg.chs[i] = chs[i].ch_num;
		cfg.port_mask |= BIT(chs[i].port);
	}

	ret = btfm_slim_enable_ports(btfm, dai->id, true);
	if (ret)
		goto err_free_chs;

	btfm->sruntime = slim_stream_allocate(btfm->sdev, "WCN399x-BTFM");
	if (IS_ERR(btfm->sruntime)) {
		ret = PTR_ERR(btfm->sruntime);
		btfm->sruntime = NULL;
		goto err_disable_ports;
	}

	ret = slim_stream_prepare(btfm->sruntime, &cfg);
	if (ret) {
		slim_stream_unprepare(btfm->sruntime);
		goto err_free_stream;
	}

	ret = slim_stream_enable(btfm->sruntime);
	if (ret)
		goto err_unprepare;

	kfree(cfg.chs);

	return 0;

err_unprepare:
	slim_stream_unprepare(btfm->sruntime);
err_free_stream:
	slim_stream_free(btfm->sruntime);
	btfm->sruntime = NULL;
err_disable_ports:
	btfm_slim_enable_ports(btfm, dai->id, false);
err_free_chs:
	kfree(cfg.chs);

	return ret;
}

static int btfm_slim_set_channel_map(struct snd_soc_dai *dai,
				     unsigned int tx_num,
				     const unsigned int *tx_slot,
				     unsigned int rx_num,
				     const unsigned int *rx_slot)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);
	static const u16 tx_ports[3] = {
		WCN3990_PORT_TX1_FM, WCN3990_PORT_TX2_FM, WCN3990_PORT_TX_SCO,
	};
	static const u16 rx_ports[2] = {
		WCN3990_PORT_RX_SCO, WCN3990_PORT_RX_A2P,
	};
	unsigned int i;

	/*
	 * The machine driver hands us the ADSP channel allocation for the
	 * whole slave (downstream msm_wcn_init), in table order: two FM
	 * TX channels, one SCO TX channel, one SCO/A2DP and one split
	 * A2DP RX channel.
	 */
	if (tx_num > 3 || rx_num > 2 || !tx_slot || !rx_slot) {
		dev_err(dai->dev, "invalid channel map tx %u rx %u\n",
			tx_num, rx_num);
		return -EINVAL;
	}

	for (i = 0; i < tx_num; i++) {
		btfm->tx_chs[i].ch_num = tx_slot[i];
		btfm->tx_chs[i].port = tx_ports[i];
	}
	for (i = 0; i < rx_num; i++) {
		btfm->rx_chs[i].ch_num = rx_slot[i];
		btfm->rx_chs[i].port = rx_ports[i];
	}

	btfm->channels_set = true;

	return 0;
}

static int btfm_slim_get_channel_map(const struct snd_soc_dai *dai,
				     unsigned int *tx_num,
				     unsigned int *tx_slot,
				     unsigned int *rx_num,
				     unsigned int *rx_slot)
{
	struct btfm_slim *btfm = snd_soc_component_get_drvdata(dai->component);

	if (!btfm->channels_set)
		return -EINVAL;

	switch (dai->id) {
	case BTFM_FM_SLIM_TX:
		tx_slot[0] = btfm->tx_chs[0].ch_num;
		tx_slot[1] = btfm->tx_chs[1].ch_num;
		*tx_num = 2;
		*rx_num = 0;
		break;
	case BTFM_BT_SCO_SLIM_TX:
		tx_slot[0] = btfm->tx_chs[2].ch_num;
		*tx_num = 1;
		*rx_num = 0;
		break;
	case BTFM_BT_SCO_A2DP_SLIM_RX:
		rx_slot[0] = btfm->rx_chs[0].ch_num;
		*tx_num = 0;
		*rx_num = 1;
		break;
	case BTFM_BT_SPLIT_A2DP_SLIM_RX:
		rx_slot[0] = btfm->rx_chs[1].ch_num;
		*tx_num = 0;
		*rx_num = 1;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dai_ops btfm_slim_dai_ops = {
	.startup	= btfm_slim_dai_startup,
	.shutdown	= btfm_slim_dai_shutdown,
	.hw_params	= btfm_slim_dai_hw_params,
	.prepare	= btfm_slim_dai_prepare,
	.set_channel_map = btfm_slim_set_channel_map,
	.get_channel_map = btfm_slim_get_channel_map,
};

static struct snd_soc_dai_driver btfm_slim_dais[] = {
	{
		/* FM audio, WCN3990 -> ADSP (two-channel group) */
		.name = "btfm_fm_slim_tx",
		.id = BTFM_FM_SLIM_TX,
		.capture = {
			.stream_name = "FM TX Capture",
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min = 48000,
			.rate_max = 48000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &btfm_slim_dai_ops,
	},
	{
		/* Bluetooth SCO voice uplink: bt -> modem */
		.name = "btfm_bt_sco_slim_tx",
		.id = BTFM_BT_SCO_SLIM_TX,
		.capture = {
			.stream_name = "SCO TX Capture",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min = 8000,
			.rate_max = 16000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.ops = &btfm_slim_dai_ops,
	},
	{
		/* Bluetooth SCO voice downlink: modem -> bt, or A2DP */
		.name = "btfm_bt_sco_a2dp_slim_rx",
		.id = BTFM_BT_SCO_A2DP_SLIM_RX,
		.playback = {
			.stream_name = "SCO A2DP RX Playback",
			.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
				 SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min = 8000,
			.rate_max = 48000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.ops = &btfm_slim_dai_ops,
	},
	{
		/* Bluetooth split A2DP data: ADSP -> bt */
		.name = "btfm_bt_split_a2dp_slim_rx",
		.id = BTFM_BT_SPLIT_A2DP_SLIM_RX,
		.playback = {
			.stream_name = "SPLIT A2DP Playback",
			.rates = SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
			.rate_min = 48000,
			.rate_max = 48000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.ops = &btfm_slim_dai_ops,
	},
};

static const struct snd_soc_component_driver btfm_slim_comp = {
	.name = "btfm-slim",
};

static void btfm_slim_sidev_put(void *data)
{
	put_device(data);
}

static int btfm_slim_probe(struct slim_device *sdev)
{
	struct device_node *ifc_np;
	struct btfm_slim *btfm;
	int ret;

	btfm = devm_kzalloc(&sdev->dev, sizeof(*btfm), GFP_KERNEL);
	if (!btfm)
		return -ENOMEM;

	ifc_np = of_parse_phandle(sdev->dev.of_node, "slim-ifc-dev", 0);
	if (!ifc_np) {
		dev_err(&sdev->dev, "slim-ifc-dev phandle missing\n");
		return -EINVAL;
	}

	btfm->sidev = of_slim_get_device(sdev->ctrl, ifc_np);
	of_node_put(ifc_np);
	if (!btfm->sidev) {
		dev_err(&sdev->dev, "interface device not found\n");
		return -EINVAL;
	}

	ret = devm_add_action_or_reset(&sdev->dev, btfm_slim_sidev_put,
				       &btfm->sidev->dev);
	if (ret)
		return ret;

	btfm->sdev = sdev;
	mutex_init(&btfm->io_lock);
	dev_set_drvdata(&sdev->dev, btfm);

	ret = devm_snd_soc_register_component(&sdev->dev, &btfm_slim_comp,
					      btfm_slim_dais,
					      ARRAY_SIZE(btfm_slim_dais));
	if (ret)
		dev_err(&sdev->dev, "failed to register component: %d\n", ret);

	return ret;
}

static void btfm_slim_remove(struct slim_device *sdev)
{
	struct btfm_slim *btfm = dev_get_drvdata(&sdev->dev);

	mutex_destroy(&btfm->io_lock);
}

static const struct slim_device_id btfm_slim_id[] = {
	/*
	 * The PGD only (device index 1); the interface device stays
	 * driverless and is looked up through the slim-ifc-dev phandle.
	 */
	{ .manf_id = WCN3990_MANF_ID, .prod_code = WCN3990_PROD_CODE,
	  .dev_index = 1, .instance = 0 },
	{ }
};
MODULE_DEVICE_TABLE(slim, btfm_slim_id);

static struct slim_driver btfm_slim_driver = {
	.driver = {
		.name = "btfm-slim-driver",
	},
	.probe = btfm_slim_probe,
	.remove = btfm_slim_remove,
	.id_table = btfm_slim_id,
};

static int __init btfm_slim_init(void)
{
	return slim_driver_register(&btfm_slim_driver);
}
module_init(btfm_slim_init);

static void __exit btfm_slim_exit(void)
{
	slim_driver_unregister(&btfm_slim_driver);
}
module_exit(btfm_slim_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm WCN399x Bluetooth/FM SLIMbus audio codec");
