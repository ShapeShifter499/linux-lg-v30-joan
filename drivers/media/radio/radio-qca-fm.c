// SPDX-License-Identifier: GPL-2.0-only
/*
 * FM receiver of Qualcomm WCN399x Bluetooth/FM controllers
 *
 * The receiver is driven over the Bluetooth UART (see hci_qca). Command
 * opcodes, parameter blocks and event layouts follow Qualcomm's radio-iris
 * driver (Copyright (c) 2011-2016, The Linux Foundation) and its helium FM
 * HAL, and were checked against the WCN3990 in an LG V30.
 */

#include <linux/auxiliary_bus.h>
#include <linux/completion.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <net/bluetooth/qca_fm.h>

#define FM_OP(ogf, ocf)			(((ogf) << 10) | (ocf))
#define FM_OGF_RECV			0x13
#define FM_OGF_COMMON			0x15

#define FM_RECV_ENABLE			FM_OP(FM_OGF_RECV, 0x01)
#define FM_RECV_DISABLE			FM_OP(FM_OGF_RECV, 0x02)
#define FM_RECV_SET_CONF		FM_OP(FM_OGF_RECV, 0x04)
#define FM_RECV_SET_MUTE		FM_OP(FM_OGF_RECV, 0x05)
#define FM_RECV_GET_STATION		FM_OP(FM_OGF_RECV, 0x0a)
#define FM_RECV_SEARCH			FM_OP(FM_OGF_RECV, 0x0e)
#define FM_RECV_CANCEL_SEARCH		FM_OP(FM_OGF_RECV, 0x11)
#define FM_RECV_RDS_GROUPS		FM_OP(FM_OGF_RECV, 0x12)
#define FM_RECV_RDS_PROCESS		FM_OP(FM_OGF_RECV, 0x13)
#define FM_RECV_EVENT_MASK		FM_OP(FM_OGF_RECV, 0x16)
#define FM_TUNE				FM_OP(FM_OGF_COMMON, 0x01)

#define FM_EV_TUNE_STATUS		0x01
#define FM_EV_RDS_GROUP			0x08
#define FM_EV_CMD_COMPLETE		0x0f
#define FM_EV_CMD_STATUS		0x10
#define FM_EV_SEARCH_COMPLETE		0x12

/* recv conf: emphasis */
#define FM_EMPHASIS_75US		0
#define FM_EMPHASIS_50US		1
/* recv conf: channel spacing */
#define FM_SPACING_200KHZ		0
#define FM_SPACING_100KHZ		1
#define FM_SPACING_50KHZ		2
/* recv conf: RDS standard */
#define FM_RDS_STD_RDS			1

/* event mask: signal level, RDS sync, audio (Qualcomm's default) */
#define FM_EVENT_MASK_DEFAULT		0x07
/* RDS processing: all of it, as Qualcomm's HAL does outside low power */
#define FM_RDS_PROCESS_ALL		0xff

#define FM_SEARCH_SEEK			0
#define FM_SEARCH_UP			0
#define FM_SEARCH_DOWN			1

#define FM_CMD_TIMEOUT_MS		2000
#define FM_TUNE_TIMEOUT_MS		2000
/* A seek across the whole band takes about ten seconds */
#define FM_SEARCH_TIMEOUT_MS		30000

/* Frequencies in kHz; V4L2 counts in 62.5 Hz units (V4L2_TUNER_CAP_LOW) */
#define FM_KHZ_TO_V4L2(khz)		((khz) * 16)
#define FM_V4L2_TO_KHZ(f)		((f) / 16)
#define FM_FREQ_MIN			76000
#define FM_FREQ_MAX			108000
#define FM_FREQ_STEP			50

/* Station parameters: frequency, service, RSSI, stereo, RDS, mute, SINR */
#define FM_STATION_LEN			11

/*
 * An RDS group event: a count (always 1 with a one-group buffer), the four
 * blocks least significant byte first, then four bytes of per-block status
 * that are not decoded here.
 */
#define FM_RDS_GROUP_LEN		9
#define FM_RDS_FIFO_BLOCKS		256

#define FM_TUNER_CAPS	(V4L2_TUNER_CAP_LOW | V4L2_TUNER_CAP_STEREO | \
			 V4L2_TUNER_CAP_RDS | V4L2_TUNER_CAP_RDS_BLOCK_IO | \
			 V4L2_TUNER_CAP_FREQ_BANDS | \
			 V4L2_TUNER_CAP_HWSEEK_BOUNDED | \
			 V4L2_TUNER_CAP_HWSEEK_WRAP | \
			 V4L2_TUNER_CAP_HWSEEK_PROG_LIM)

static const struct v4l2_frequency_band qca_radio_bands[] = {
	{
		.type = V4L2_TUNER_RADIO,
		.index = 0,
		.capability = FM_TUNER_CAPS,
		.rangelow = FM_KHZ_TO_V4L2(87500),
		.rangehigh = FM_KHZ_TO_V4L2(108000),
		.modulation = V4L2_BAND_MODULATION_FM,
	},
	{
		/* Japan */
		.type = V4L2_TUNER_RADIO,
		.index = 1,
		.capability = FM_TUNER_CAPS,
		.rangelow = FM_KHZ_TO_V4L2(76000),
		.rangehigh = FM_KHZ_TO_V4L2(95000),
		.modulation = V4L2_BAND_MODULATION_FM,
	},
};

struct fm_recv_conf {
	u8 emphasis;
	u8 spacing;
	u8 rds_std;
	u8 hlsi;
	__le32 band_low;
	__le32 band_high;
} __packed;

struct fm_mute {
	u8 hard_mute;
	u8 soft_mute;
} __packed;

struct fm_search {
	u8 mode;
	u8 scan_time;
	u8 direction;
} __packed;

/* Laid out as the helium HAL sends it, padding included */
struct fm_rds_groups {
	__le32 enable_mask;
	__le32 buffer_size;
	u8 change_filter;
	u8 reserved[3];
} __packed;

struct qca_radio_station {
	u32 freq;
	s8 rssi;
	bool stereo;
	bool rds;
};

struct qca_radio {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *mute;
	struct v4l2_ctrl *deemphasis;
	struct v4l2_ctrl *rds;
	struct mutex lock;		/* serialises commands and ioctls */
	struct qca_fm_device *fm;	/* NULL once unbound */
	struct device *dev;

	bool on;
	int generation;
	u32 freq;			/* kHz */
	u32 band_low, band_high;	/* kHz, as configured */
	u8 spacing;

	spinlock_t evlock;		/* protects the fields below */
	u16 cmd_op;
	u8 cmd_status;
	u8 rsp[32];
	u8 rsp_len;
	struct qca_radio_station station;
	struct completion cmd_done;
	struct completion tuned;
	struct completion searched;

	/* RDS blocks: filled by the event handler, drained by read() */
	DECLARE_KFIFO(rds_fifo, struct v4l2_rds_data, FM_RDS_FIFO_BLOCKS);
	wait_queue_head_t rds_wait;
	struct mutex rds_lock;		/* serialises readers */
};

static void qca_radio_parse_station(struct qca_radio_station *st, const u8 *p)
{
	/* p[4] is "service available", not yet valid right after a seek */
	st->freq = get_unaligned_le32(p);
	st->rssi = (s8)p[5];
	st->stereo = p[6];
	st->rds = p[7];
}

static void qca_radio_queue_rds(struct qca_radio *radio, const u8 *p)
{
	struct v4l2_rds_data blk[4];
	int i;

	for (i = 0; i < 4; i++) {
		blk[i].lsb = p[1 + 2 * i];
		blk[i].msb = p[2 + 2 * i];
		blk[i].block = i;
	}
	/* Version B groups carry C' instead of C */
	if (blk[1].msb & BIT(3))
		blk[2].block = V4L2_RDS_BLOCK_C_ALT;
	/* Bits 3-5 are deprecated and have to repeat bits 0-2 */
	for (i = 0; i < 4; i++)
		blk[i].block |= blk[i].block << 3;

	/* Drop the group rather than a part of it when the reader lags */
	if (kfifo_avail(&radio->rds_fifo) < ARRAY_SIZE(blk))
		return;

	kfifo_in(&radio->rds_fifo, blk, ARRAY_SIZE(blk));
	wake_up_interruptible(&radio->rds_wait);
}

/* Called by hci_qca with a spinlock held; must not sleep */
static void qca_radio_event(void *priv, struct sk_buff *skb)
{
	struct qca_radio *radio = priv;
	unsigned long flags;
	const u8 *p;
	u8 code, plen;

	if (skb->len < 2)
		goto out;

	code = skb->data[0];
	plen = skb->data[1];
	p = skb->data + 2;
	if (plen > skb->len - 2)
		goto out;

	dev_dbg(radio->dev, "event %#04x: %*ph\n", code, plen, p);

	spin_lock_irqsave(&radio->evlock, flags);

	switch (code) {
	case FM_EV_CMD_COMPLETE:
		/* num commands, opcode, status, return parameters */
		if (plen < 4 || !radio->cmd_op ||
		    get_unaligned_le16(p + 1) != radio->cmd_op)
			break;
		radio->cmd_status = p[3];
		radio->rsp_len = min_t(u8, plen - 4, sizeof(radio->rsp));
		memcpy(radio->rsp, p + 4, radio->rsp_len);
		radio->cmd_op = 0;
		complete(&radio->cmd_done);
		break;

	case FM_EV_CMD_STATUS:
		/* status, num commands, opcode */
		if (plen < 4 || !radio->cmd_op ||
		    get_unaligned_le16(p + 2) != radio->cmd_op)
			break;
		radio->cmd_status = p[0];
		radio->rsp_len = 0;
		radio->cmd_op = 0;
		complete(&radio->cmd_done);
		break;

	case FM_EV_TUNE_STATUS:
		/* sub-event, station parameters */
		if (plen < 1 + FM_STATION_LEN)
			break;
		qca_radio_parse_station(&radio->station, p + 1);
		complete(&radio->tuned);
		break;

	case FM_EV_SEARCH_COMPLETE:
		complete(&radio->searched);
		break;

	case FM_EV_RDS_GROUP:
		if (plen >= FM_RDS_GROUP_LEN)
			qca_radio_queue_rds(radio, p);
		break;
	}

	spin_unlock_irqrestore(&radio->evlock, flags);
out:
	kfree_skb(skb);
}

static int qca_radio_cmd(struct qca_radio *radio, u16 op,
			 const void *param, u8 plen)
{
	struct device *dev = &radio->fm->adev.dev;
	unsigned long flags;
	u8 buf[3 + 16];
	int ret;

	lockdep_assert_held(&radio->lock);

	if (WARN_ON(plen > sizeof(buf) - 3))
		return -EINVAL;

	put_unaligned_le16(op, buf);
	buf[2] = plen;
	if (plen)
		memcpy(buf + 3, param, plen);

	spin_lock_irqsave(&radio->evlock, flags);
	radio->cmd_op = op;
	reinit_completion(&radio->cmd_done);
	spin_unlock_irqrestore(&radio->evlock, flags);

	ret = qca_fm_send(radio->fm, buf, 3 + plen);
	if (ret == -ENETDOWN) {
		dev_warn_ratelimited(dev, "Bluetooth is off; FM needs it on\n");
		ret = -EIO;
		goto fail;
	} else if (ret) {
		goto fail;
	}

	if (!wait_for_completion_timeout(&radio->cmd_done,
					 msecs_to_jiffies(FM_CMD_TIMEOUT_MS))) {
		dev_err(dev, "command %#06x timed out\n", op);
		ret = -ETIMEDOUT;
		goto fail;
	}

	if (radio->cmd_status) {
		dev_err(dev, "command %#06x failed: status %#04x\n",
			op, radio->cmd_status);
		return -EIO;
	}

	return 0;

fail:
	spin_lock_irqsave(&radio->evlock, flags);
	radio->cmd_op = 0;
	spin_unlock_irqrestore(&radio->evlock, flags);
	return ret;
}

static int qca_radio_set_conf(struct qca_radio *radio, u32 low, u32 high,
			      u8 spacing)
{
	struct fm_recv_conf conf = {
		.spacing = spacing,
		.rds_std = FM_RDS_STD_RDS,
		.band_low = cpu_to_le32(low),
		.band_high = cpu_to_le32(high),
	};
	int ret;

	if (radio->deemphasis->val == V4L2_DEEMPHASIS_50_uS)
		conf.emphasis = FM_EMPHASIS_50US;
	else
		conf.emphasis = FM_EMPHASIS_75US;

	ret = qca_radio_cmd(radio, FM_RECV_SET_CONF, &conf, sizeof(conf));
	if (ret)
		return ret;

	radio->band_low = low;
	radio->band_high = high;
	radio->spacing = spacing;
	return 0;
}

static int qca_radio_set_mute(struct qca_radio *radio, bool mute)
{
	struct fm_mute m = { .hard_mute = mute };

	return qca_radio_cmd(radio, FM_RECV_SET_MUTE, &m, sizeof(m));
}

static int qca_radio_set_rds(struct qca_radio *radio, bool on)
{
	struct fm_rds_groups groups = {
		.enable_mask = cpu_to_le32(on ? 0xffffffff : 0),
		.buffer_size = cpu_to_le32(1),
	};

	return qca_radio_cmd(radio, FM_RECV_RDS_GROUPS, &groups,
			     sizeof(groups));
}

static int qca_radio_get_station(struct qca_radio *radio,
				 struct qca_radio_station *st)
{
	unsigned long flags;
	int ret;

	ret = qca_radio_cmd(radio, FM_RECV_GET_STATION, NULL, 0);
	if (ret)
		return ret;

	if (radio->rsp_len < FM_STATION_LEN)
		return -EIO;

	spin_lock_irqsave(&radio->evlock, flags);
	qca_radio_parse_station(&radio->station, radio->rsp);
	*st = radio->station;
	spin_unlock_irqrestore(&radio->evlock, flags);

	return 0;
}

/* The band to program for a frequency: the default one if it covers it */
static const struct v4l2_frequency_band *qca_radio_band_of(u32 khz)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qca_radio_bands); i++)
		if (khz >= FM_V4L2_TO_KHZ(qca_radio_bands[i].rangelow) &&
		    khz <= FM_V4L2_TO_KHZ(qca_radio_bands[i].rangehigh))
			return &qca_radio_bands[i];

	return &qca_radio_bands[0];
}

static int qca_radio_tune(struct qca_radio *radio, u32 khz)
{
	const struct v4l2_frequency_band *band;
	__le32 param = cpu_to_le32(khz);
	unsigned long flags;
	int ret;

	if (khz < radio->band_low || khz > radio->band_high) {
		band = qca_radio_band_of(khz);
		ret = qca_radio_set_conf(radio, FM_V4L2_TO_KHZ(band->rangelow),
					 FM_V4L2_TO_KHZ(band->rangehigh),
					 radio->spacing);
		if (ret)
			return ret;
	}

	reinit_completion(&radio->tuned);
	ret = qca_radio_cmd(radio, FM_TUNE, &param, sizeof(param));
	if (ret)
		return ret;

	if (!wait_for_completion_timeout(&radio->tuned,
					 msecs_to_jiffies(FM_TUNE_TIMEOUT_MS)))
		return -ETIMEDOUT;

	spin_lock_irqsave(&radio->evlock, flags);
	radio->freq = radio->station.freq;
	spin_unlock_irqrestore(&radio->evlock, flags);

	return 0;
}

static int qca_radio_power_up(struct qca_radio *radio)
{
	const struct v4l2_frequency_band *band = qca_radio_band_of(radio->freq);
	int generation = atomic_read(&radio->fm->generation);
	__le32 rds_process = cpu_to_le32(FM_RDS_PROCESS_ALL);
	u8 mask;
	int ret;

	radio->on = false;

	ret = qca_radio_cmd(radio, FM_RECV_ENABLE, NULL, 0);
	if (ret)
		return ret;

	ret = qca_radio_set_conf(radio, FM_V4L2_TO_KHZ(band->rangelow),
				 FM_V4L2_TO_KHZ(band->rangehigh),
				 FM_SPACING_100KHZ);
	if (ret)
		goto disable;

	ret = qca_radio_set_mute(radio, radio->mute->val);
	if (ret)
		goto disable;

	mask = FM_EVENT_MASK_DEFAULT;
	ret = qca_radio_cmd(radio, FM_RECV_EVENT_MASK, &mask, sizeof(mask));
	if (ret)
		goto disable;

	ret = qca_radio_cmd(radio, FM_RECV_RDS_PROCESS, &rds_process,
			    sizeof(rds_process));
	if (ret)
		goto disable;

	ret = qca_radio_set_rds(radio, radio->rds->val);
	if (ret)
		goto disable;

	ret = qca_radio_tune(radio, radio->freq);
	if (ret)
		goto disable;

	radio->generation = generation;
	radio->on = true;
	return 0;

disable:
	qca_radio_cmd(radio, FM_RECV_DISABLE, NULL, 0);
	return ret;
}

static void qca_radio_power_down(struct qca_radio *radio)
{
	if (radio->on)
		qca_radio_cmd(radio, FM_RECV_DISABLE, NULL, 0);
	radio->on = false;
}

/* The receiver state is lost whenever Bluetooth is switched off */
static int qca_radio_ensure_on(struct qca_radio *radio)
{
	if (!radio->fm)
		return -ENODEV;

	if (radio->on &&
	    radio->generation == atomic_read(&radio->fm->generation))
		return 0;

	return qca_radio_power_up(radio);
}

static int qca_radio_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	struct qca_radio *radio = video_drvdata(file);

	strscpy(cap->driver, "radio-qca-fm", sizeof(cap->driver));
	strscpy(cap->card, "Qualcomm WCN399x FM receiver", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(radio->dev));
	return 0;
}

static int qca_radio_g_tuner(struct file *file, void *priv,
			     struct v4l2_tuner *tuner)
{
	struct qca_radio *radio = video_drvdata(file);
	struct qca_radio_station st;
	int ret;

	if (tuner->index != 0)
		return -EINVAL;

	strscpy(tuner->name, "FM", sizeof(tuner->name));
	tuner->type = V4L2_TUNER_RADIO;
	tuner->capability = FM_TUNER_CAPS;
	tuner->rangelow = FM_KHZ_TO_V4L2(FM_FREQ_MIN);
	tuner->rangehigh = FM_KHZ_TO_V4L2(FM_FREQ_MAX);
	tuner->audmode = V4L2_TUNER_MODE_STEREO;

	ret = qca_radio_ensure_on(radio);
	if (ret)
		return ret;

	ret = qca_radio_get_station(radio, &st);
	if (ret)
		return ret;

	tuner->rxsubchans = st.stereo ? V4L2_TUNER_SUB_STEREO :
					V4L2_TUNER_SUB_MONO;
	if (st.rds)
		tuner->rxsubchans |= V4L2_TUNER_SUB_RDS;
	/* -110 dBm (noise) .. -60 dBm (strong) onto 0 .. 65535 */
	tuner->signal = (clamp_t(int, st.rssi, -110, -60) + 110) * 65535 / 50;

	return 0;
}

static int qca_radio_s_tuner(struct file *file, void *priv,
			     const struct v4l2_tuner *tuner)
{
	/* Stereo/mono selection is left to the receiver's own blending */
	return tuner->index ? -EINVAL : 0;
}

static int qca_radio_g_frequency(struct file *file, void *priv,
				 struct v4l2_frequency *f)
{
	struct qca_radio *radio = video_drvdata(file);

	if (f->tuner != 0)
		return -EINVAL;

	f->type = V4L2_TUNER_RADIO;
	f->frequency = FM_KHZ_TO_V4L2(radio->freq);
	return 0;
}

static int qca_radio_s_frequency(struct file *file, void *priv,
				 const struct v4l2_frequency *f)
{
	struct qca_radio *radio = video_drvdata(file);
	u32 khz;
	int ret;

	if (f->tuner != 0 || f->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	khz = clamp_t(u32, FM_V4L2_TO_KHZ(f->frequency),
		      FM_FREQ_MIN, FM_FREQ_MAX);
	khz = rounddown(khz + FM_FREQ_STEP / 2, FM_FREQ_STEP);

	ret = qca_radio_ensure_on(radio);
	if (ret) {
		/* Remember it for when the receiver comes up */
		radio->freq = khz;
		return ret;
	}

	return qca_radio_tune(radio, khz);
}

static int qca_radio_s_hw_freq_seek(struct file *file, void *priv,
				    const struct v4l2_hw_freq_seek *seek)
{
	struct qca_radio *radio = video_drvdata(file);
	const struct v4l2_frequency_band *band;
	struct qca_radio_station st;
	struct fm_search search = { .mode = FM_SEARCH_SEEK };
	u32 low, high, start;
	unsigned long timeout;
	u8 spacing;
	long left;
	int ret;

	if (seek->tuner != 0 || seek->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	if (file->f_flags & O_NONBLOCK)
		return -EWOULDBLOCK;

	if (seek->rangelow || seek->rangehigh) {
		low = FM_V4L2_TO_KHZ(seek->rangelow);
		high = FM_V4L2_TO_KHZ(seek->rangehigh);
		if (low < FM_FREQ_MIN || high > FM_FREQ_MAX || low >= high)
			return -EINVAL;
	} else {
		band = qca_radio_band_of(radio->freq);
		low = FM_V4L2_TO_KHZ(band->rangelow);
		high = FM_V4L2_TO_KHZ(band->rangehigh);
	}

	if (!seek->spacing)
		spacing = FM_SPACING_100KHZ;
	else if (FM_V4L2_TO_KHZ(seek->spacing) <= 50)
		spacing = FM_SPACING_50KHZ;
	else if (FM_V4L2_TO_KHZ(seek->spacing) <= 100)
		spacing = FM_SPACING_100KHZ;
	else
		spacing = FM_SPACING_200KHZ;

	ret = qca_radio_ensure_on(radio);
	if (ret)
		return ret;

	ret = qca_radio_set_conf(radio, low, high, spacing);
	if (ret)
		return ret;

	start = radio->freq;
	if (start < low || start > high) {
		start = seek->seek_upward ? low : high;
		ret = qca_radio_tune(radio, start);
		if (ret)
			return ret;
	}

	search.direction = seek->seek_upward ? FM_SEARCH_UP : FM_SEARCH_DOWN;
	reinit_completion(&radio->searched);
	ret = qca_radio_cmd(radio, FM_RECV_SEARCH, &search, sizeof(search));
	if (ret)
		return ret;

	timeout = msecs_to_jiffies(FM_SEARCH_TIMEOUT_MS);
	left = wait_for_completion_interruptible_timeout(&radio->searched,
							 timeout);
	if (left <= 0) {
		qca_radio_cmd(radio, FM_RECV_CANCEL_SEARCH, NULL, 0);
		wait_for_completion_timeout(&radio->searched,
					    msecs_to_jiffies(FM_CMD_TIMEOUT_MS));
	}

	/* Wherever the search stopped, report where the receiver is */
	ret = qca_radio_get_station(radio, &st);
	if (!ret)
		radio->freq = st.freq;
	dev_dbg(radio->dev, "seek from %u kHz: at %u kHz (%ld, %d)\n",
		start, st.freq, left, ret);

	if (left < 0)
		return left;
	if (!left)
		return -EAGAIN;
	if (ret)
		return ret;

	/* Finding nothing, the receiver wraps around back to the start */
	if (st.freq == start)
		return -ENODATA;

	if (!seek->wrap_around &&
	    (seek->seek_upward ? st.freq < start : st.freq > start)) {
		qca_radio_tune(radio, start);
		return -ENODATA;
	}

	return 0;
}

static int qca_radio_enum_freq_bands(struct file *file, void *priv,
				     struct v4l2_frequency_band *band)
{
	if (band->tuner != 0 || band->index >= ARRAY_SIZE(qca_radio_bands))
		return -EINVAL;

	*band = qca_radio_bands[band->index];
	return 0;
}

static int qca_radio_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct qca_radio *radio = container_of(ctrl->handler,
					       struct qca_radio, hdl);

	/* Applied at power-up when the receiver is off */
	if (!radio->on || !radio->fm)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		return qca_radio_set_mute(radio, ctrl->val);
	case V4L2_CID_TUNE_DEEMPHASIS:
		return qca_radio_set_conf(radio, radio->band_low,
					  radio->band_high, radio->spacing);
	case V4L2_CID_RDS_RECEPTION:
		return qca_radio_set_rds(radio, ctrl->val);
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops qca_radio_ctrl_ops = {
	.s_ctrl = qca_radio_s_ctrl,
};

static bool qca_radio_rds_ready(struct qca_radio *radio)
{
	return !kfifo_is_empty(&radio->rds_fifo) ||
	       !video_is_registered(&radio->vdev);
}

static ssize_t qca_radio_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct qca_radio *radio = video_drvdata(file);
	unsigned int copied;
	int ret;

	count = rounddown(count, sizeof(struct v4l2_rds_data));
	if (!count)
		return -EINVAL;

	/* RDS arrives only while the receiver runs */
	mutex_lock(&radio->lock);
	ret = qca_radio_ensure_on(radio);
	mutex_unlock(&radio->lock);
	if (ret)
		return ret;

	if (mutex_lock_interruptible(&radio->rds_lock))
		return -ERESTARTSYS;

	while (kfifo_is_empty(&radio->rds_fifo)) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EWOULDBLOCK;
			goto unlock;
		}
		ret = wait_event_interruptible(radio->rds_wait,
					       qca_radio_rds_ready(radio));
		if (ret)
			goto unlock;
		if (!video_is_registered(&radio->vdev)) {
			ret = -ENODEV;
			goto unlock;
		}
	}

	ret = kfifo_to_user(&radio->rds_fifo, buf, count, &copied);
unlock:
	mutex_unlock(&radio->rds_lock);
	return ret ? ret : copied;
}

static __poll_t qca_radio_poll(struct file *file, poll_table *wait)
{
	struct qca_radio *radio = video_drvdata(file);
	__poll_t req = poll_requested_events(wait);
	__poll_t res = v4l2_ctrl_poll(file, wait);

	if (req & (EPOLLIN | EPOLLRDNORM)) {
		poll_wait(file, &radio->rds_wait, wait);
		if (!kfifo_is_empty(&radio->rds_fifo))
			res |= EPOLLIN | EPOLLRDNORM;
	}

	return res;
}

/* The receiver powers up on the first request that needs it */
static int qca_radio_release(struct file *file)
{
	struct qca_radio *radio = video_drvdata(file);

	mutex_lock(&radio->lock);
	if (v4l2_fh_is_singular_file(file) && radio->fm)
		qca_radio_power_down(radio);
	mutex_unlock(&radio->lock);

	return v4l2_fh_release(file);
}

static const struct v4l2_file_operations qca_radio_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = qca_radio_release,
	.read = qca_radio_read,
	.poll = qca_radio_poll,
	.unlocked_ioctl = video_ioctl2,
};

static const struct v4l2_ioctl_ops qca_radio_ioctl_ops = {
	.vidioc_querycap = qca_radio_querycap,
	.vidioc_g_tuner = qca_radio_g_tuner,
	.vidioc_s_tuner = qca_radio_s_tuner,
	.vidioc_g_frequency = qca_radio_g_frequency,
	.vidioc_s_frequency = qca_radio_s_frequency,
	.vidioc_s_hw_freq_seek = qca_radio_s_hw_freq_seek,
	.vidioc_enum_freq_bands = qca_radio_enum_freq_bands,
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* Runs once the last file handle is gone, possibly after unbind */
static void qca_radio_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct qca_radio *radio = container_of(v4l2_dev, struct qca_radio,
					       v4l2_dev);

	v4l2_ctrl_handler_free(&radio->hdl);
	v4l2_device_unregister(&radio->v4l2_dev);
	mutex_destroy(&radio->rds_lock);
	mutex_destroy(&radio->lock);
	kfree(radio);
}

static int qca_radio_probe(struct auxiliary_device *adev,
			   const struct auxiliary_device_id *id)
{
	struct qca_fm_device *fm = to_qca_fm(adev);
	struct qca_radio *radio;
	int ret;

	radio = kzalloc_obj(*radio);
	if (!radio)
		return -ENOMEM;

	mutex_init(&radio->lock);
	mutex_init(&radio->rds_lock);
	spin_lock_init(&radio->evlock);
	INIT_KFIFO(radio->rds_fifo);
	init_waitqueue_head(&radio->rds_wait);
	init_completion(&radio->cmd_done);
	init_completion(&radio->tuned);
	init_completion(&radio->searched);
	radio->fm = fm;
	radio->dev = &adev->dev;
	radio->freq = 87500;
	radio->spacing = FM_SPACING_100KHZ;

	ret = v4l2_device_register(&adev->dev, &radio->v4l2_dev);
	if (ret) {
		mutex_destroy(&radio->rds_lock);
		mutex_destroy(&radio->lock);
		kfree(radio);
		return ret;
	}
	/* From here on, dropping the last reference frees everything */
	radio->v4l2_dev.release = qca_radio_v4l2_release;

	/*
	 * The handler keeps its own lock: the core already holds radio->lock
	 * around every ioctl, including the control ones, when s_ctrl runs.
	 */
	v4l2_ctrl_handler_init(&radio->hdl, 3);
	radio->mute = v4l2_ctrl_new_std(&radio->hdl, &qca_radio_ctrl_ops,
					V4L2_CID_AUDIO_MUTE, 0, 1, 1, 0);
	radio->deemphasis =
		v4l2_ctrl_new_std_menu(&radio->hdl, &qca_radio_ctrl_ops,
				       V4L2_CID_TUNE_DEEMPHASIS,
				       V4L2_DEEMPHASIS_75_uS,
				       BIT(V4L2_DEEMPHASIS_DISABLED),
				       V4L2_DEEMPHASIS_75_uS);
	radio->rds = v4l2_ctrl_new_std(&radio->hdl, &qca_radio_ctrl_ops,
				       V4L2_CID_RDS_RECEPTION, 0, 1, 1, 1);
	if (radio->hdl.error) {
		ret = radio->hdl.error;
		goto err_hdl;
	}
	radio->v4l2_dev.ctrl_handler = &radio->hdl;

	strscpy(radio->vdev.name, "Qualcomm FM receiver",
		sizeof(radio->vdev.name));
	radio->vdev.v4l2_dev = &radio->v4l2_dev;
	radio->vdev.fops = &qca_radio_fops;
	radio->vdev.ioctl_ops = &qca_radio_ioctl_ops;
	radio->vdev.release = video_device_release_empty;
	radio->vdev.lock = &radio->lock;
	radio->vdev.device_caps = V4L2_CAP_TUNER | V4L2_CAP_RADIO |
				  V4L2_CAP_HW_FREQ_SEEK | V4L2_CAP_RDS_CAPTURE |
				  V4L2_CAP_READWRITE;
	video_set_drvdata(&radio->vdev, radio);

	auxiliary_set_drvdata(adev, radio);
	qca_fm_set_event_handler(fm, qca_radio_event, radio);

	ret = video_register_device(&radio->vdev, VFL_TYPE_RADIO, -1);
	if (ret)
		goto err_handler;

	dev_info(&adev->dev, "FM receiver registered as %s\n",
		 video_device_node_name(&radio->vdev));
	return 0;

err_handler:
	qca_fm_set_event_handler(fm, NULL, NULL);
err_hdl:
	v4l2_device_put(&radio->v4l2_dev);
	return ret;
}

static void qca_radio_remove(struct auxiliary_device *adev)
{
	struct qca_radio *radio = auxiliary_get_drvdata(adev);

	mutex_lock(&radio->lock);
	qca_radio_power_down(radio);
	qca_fm_set_event_handler(radio->fm, NULL, NULL);
	radio->fm = NULL;
	video_unregister_device(&radio->vdev);
	v4l2_device_disconnect(&radio->v4l2_dev);
	mutex_unlock(&radio->lock);
	wake_up_interruptible(&radio->rds_wait);

	v4l2_device_put(&radio->v4l2_dev);
}

static const struct auxiliary_device_id qca_radio_id_table[] = {
	{ .name = "hci_uart.fm" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, qca_radio_id_table);

static struct auxiliary_driver qca_radio_driver = {
	.probe = qca_radio_probe,
	.remove = qca_radio_remove,
	.id_table = qca_radio_id_table,
};
module_auxiliary_driver(qca_radio_driver);

MODULE_DESCRIPTION("Qualcomm WCN399x FM receiver");
MODULE_LICENSE("GPL");
