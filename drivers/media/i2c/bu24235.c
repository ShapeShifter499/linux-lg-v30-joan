// SPDX-License-Identifier: GPL-2.0
/*
 * ROHM BU24235 AF/OIS controller with RAM firmware download, as fitted to
 * the LG Innotek rear camera module of the LG V30 (joan).
 *
 * The part shares the camera module's supplies with the IMX351 sensor and
 * has no regulators of its own, so no I2C access happens at probe; the
 * controller is brought up on the first focus request, which userspace
 * only makes while the sensor is on.  The module is dual-sourced: the
 * module EEPROM (0x54 on the same bus) tells which lens controller is
 * fitted, and this driver declines anything that is not the ROHM one.
 *
 * The download protocol, firmware file names, revision selection by
 * EEPROM calibration version and the checksums are transcribed from LG's
 * [ android_kernel_lge_msm8998 drivers/media/platform/msm/camera_v2/
 *   sensor/ois/lgit_imx351_rohm_ois.c ] and its I2C helpers.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define BU24235_NAME		"bu24235"

/*
 * The focus DAC is a signed 16-bit position that LG clamps to +/-512
 * (0x1ff..0x200 wraps).  Expose the standard 0..1023 absolute range
 * centred on DAC 0; the EEPROM calibration limits that would narrow it
 * are not decoded yet.
 */
#define BU24235_MAX_FOCUS_POS	1023

/* controller registers, 16-bit addresses */
#define BU24235_OIS_START_DL	0xf010
#define BU24235_OIS_COMPLETE_DL	0xf006
#define BU24235_CHECKSUM	0xf008
#define BU24235_STATUS		0x6024
#define BU24235_OIS_CTRL	0x6020
#define BU24235_VCM_STATUS	0x6023
#define BU24235_VCM_INIT	0x60f1
#define BU24235_VCM_DAC		0x612e

/* module EEPROM (24xx-style, 16-bit addresses) on the same bus */
#define BU24235_E2P_ADDR	0x54
#define BU24235_E2P_CAL_VER	0x0ac2
#define BU24235_E2P_MAP_VER	0x0be2
#define BU24235_E2P_CAL_BASE	0x0a90
#define BU24235_E2P_CAL_SPLIT	26
#define BU24235_E2P_CAL_ADDR2	0x0aac
#define BU24235_E2P_CAL_SIZE	48
/*
 * The qcom CCI caps a single read message (12 bytes on msm8998's cci_v2),
 * enforced by i2c_check_for_quirks() as -EOPNOTSUPP. The EEPROM has no
 * such limit, so long reads are split into adapter-sized chunks.
 */
#define BU24235_E2P_MAX_READ	12

/* the calibration lands in controller RAM at this address */
#define BU24235_RAM_CAL_DEST	0x1dc0

#define BU24235_POLL_RETRIES	20
#define BU24235_I2C_CHUNK	6

/* firmware revisions, selected by the EEPROM calibration version */
static const struct bu24235_fw {
	u16 cal_ver;
	u32 checksum;
	const char *files[2];
} bu24235_fw_revs[] = {
	{
		.cal_ver = 0xa9e9,
		.checksum = 0x00014555,
		.files = {
			"bu24235_dl_program_Joan_LGITAct_ICG1020S_rev0_S_data1.bin",
			"bu24235_dl_program_Joan_LGITAct_ICG1020S_rev0_S_data2.bin",
		},
	},
	{
		.cal_ver = 0x0101,
		.checksum = 0x00014ab9,
		.files = {
			"bu24235_dl_program_Joan_LGITAct_ICG1020S_rev3_S_data1.bin",
			"bu24235_dl_program_Joan_LGITAct_ICG1020S_rev3_S_data2.bin",
		},
	},
	{
		.cal_ver = 0x0301,
		.checksum = 0x00014523,
		.files = {
			"bu24235_dl_program_Joan_LGITAct_ICG1020S_rev5_S_data1.bin",
			"bu24235_dl_program_Joan_LGITAct_ICG1020S_rev5_S_data2.bin",
		},
	},
};

struct bu24235_device {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct i2c_client *client;
	struct mutex mutex;
	bool init_done;
};

static inline struct bu24235_device *sd_to_bu24235(struct v4l2_subdev *sd)
{
	return container_of(sd, struct bu24235_device, sd);
}

/* one transaction: 16-bit big-endian register address, then len data bytes */
static int bu24235_read_addr(struct i2c_client *client, u16 slave_addr,
			     u16 reg, void *val, size_t len)
{
	struct i2c_msg msgs[2];
	u8 reg_buf[2] = { reg >> 8, reg & 0xff };
	int ret;

	msgs[0].addr = slave_addr;
	msgs[0].flags = 0;
	msgs[0].len = sizeof(reg_buf);
	msgs[0].buf = reg_buf;
	msgs[1].addr = slave_addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = val;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		return ret;
	if (ret != 2)
		return -EIO;

	return 0;
}

static int bu24235_read(struct i2c_client *client, u16 reg, void *val,
			size_t len)
{
	return bu24235_read_addr(client, client->addr, reg, val, len);
}

static int bu24235_eeprom_read(struct i2c_client *client, u16 reg, void *val,
			       size_t len)
{
	return bu24235_read_addr(client, BU24235_E2P_ADDR, reg, val, len);
}

/*
 * Read len bytes from the EEPROM, splitting the transfer into chunks no
 * larger than the adapter allows for a single read message. Each chunk
 * re-issues the register address, so the EEPROM's own limits do not
 * apply and the byte stream is contiguous.
 */
static int bu24235_eeprom_read_chunked(struct i2c_client *client, u16 reg,
				       void *val, size_t len)
{
	const struct i2c_adapter_quirks *quirks = client->adapter->quirks;
	size_t max_len = BU24235_E2P_MAX_READ;
	u8 *buf = val;

	if (quirks && quirks->max_read_len && quirks->max_read_len < max_len)
		max_len = quirks->max_read_len;

	while (len) {
		size_t chunk = min(len, max_len);
		int ret = bu24235_eeprom_read(client, reg, buf, chunk);

		if (ret)
			return ret;

		reg += chunk;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int bu24235_write(struct i2c_client *client, u16 reg, const void *data,
			 size_t len)
{
	u8 buf[2 + BU24235_I2C_CHUNK];
	struct i2c_msg msg;
	size_t chunk;

	msg.addr = client->addr;
	msg.flags = 0;
	msg.buf = buf;

	while (len) {
		chunk = min(len, (size_t)BU24235_I2C_CHUNK);

		buf[0] = reg >> 8;
		buf[1] = reg & 0xff;
		memcpy(buf + 2, data, chunk);

		msg.len = 2 + chunk;
		if (i2c_transfer(client->adapter, &msg, 1) != 1)
			return -EIO;

		reg += chunk;
		data += chunk;
		len -= chunk;
	}

	return 0;
}

static int bu24235_write_u8(struct i2c_client *client, u16 reg, u8 val)
{
	return bu24235_write(client, reg, &val, 1);
}

static int bu24235_write_s16(struct i2c_client *client, u16 reg, s16 val)
{
	__be16 be = cpu_to_be16(val);

	return bu24235_write(client, reg, &be, sizeof(be));
}

/* LG reads the 32-bit checksum big-endian (RamRead32A) */
static int bu24235_read_u32_be(struct i2c_client *client, u16 reg, u32 *val)
{
	__be32 be;
	int ret;

	ret = bu24235_read(client, reg, &be, sizeof(be));
	if (ret)
		return ret;

	*val = be32_to_cpu(be);
	return 0;
}

/* the part asserts 0x01 in 0x6024 when it is ready; LG polls 15 x 5 ms */
static int bu24235_poll_ready(struct i2c_client *client)
{
	u8 status = 0;
	int i;

	usleep_range(1000, 1100);

	for (i = 0; i < BU24235_POLL_RETRIES; i++) {
		int ret = bu24235_read(client, BU24235_STATUS, &status, 1);

		if (ret)
			return ret;
		if (status == 0x01)
			return 0;

		usleep_range(5000, 5100);
	}

	dev_dbg(&client->dev, "status poll timed out (0x%02x)\n", status);
	return -ETIMEDOUT;
}

/* 48 bytes of module EEPROM calibration, copied into controller RAM */
static int bu24235_read_cal(struct i2c_client *client, u8 *cal)
{
	int ret;

	ret = bu24235_eeprom_read_chunked(client, BU24235_E2P_CAL_BASE, cal,
					  BU24235_E2P_CAL_SPLIT);
	if (ret)
		return ret;

	return bu24235_eeprom_read_chunked(client, BU24235_E2P_CAL_ADDR2,
					   cal + BU24235_E2P_CAL_SPLIT,
					   BU24235_E2P_CAL_SIZE - BU24235_E2P_CAL_SPLIT);
}

static int bu24235_download(struct bu24235_device *bu24235,
			    const struct bu24235_fw *fw)
{
	struct i2c_client *client = bu24235->client;
	const struct firmware *data1, *data2;
	u8 cal[BU24235_E2P_CAL_SIZE];
	u32 checksum;
	int ret;

	ret = request_firmware(&data1, fw->files[0], &client->dev);
	if (ret)
		return ret;
	ret = request_firmware(&data2, fw->files[1], &client->dev);
	if (ret) {
		release_firmware(data1);
		return ret;
	}

	ret = bu24235_poll_ready(client);
	if (ret)
		goto out;

	ret = bu24235_write_u8(client, BU24235_OIS_START_DL, 0);
	if (ret)
		goto out;

	ret = bu24235_write(client, 0x0000, data1->data, data1->size);
	if (ret)
		goto out;
	ret = bu24235_write(client, 0x1c00, data2->data, data2->size);
	if (ret)
		goto out;

	ret = bu24235_read_u32_be(client, BU24235_CHECKSUM, &checksum);
	if (ret)
		goto out;
	if (checksum != fw->checksum) {
		dev_err(&client->dev,
			"firmware checksum mismatch: expected 0x%08x, read 0x%08x\n",
			fw->checksum, checksum);
		ret = -EIO;
		goto out;
	}

	/* copy the per-unit calibration next to the firmware in RAM */
	ret = bu24235_read_cal(client, cal);
	if (ret)
		goto out;
	ret = bu24235_write(client, BU24235_RAM_CAL_DEST, cal, sizeof(cal));
	if (ret)
		goto out;

	ret = bu24235_write_u8(client, BU24235_OIS_COMPLETE_DL, 0);
	if (ret)
		goto out;

	ret = bu24235_poll_ready(client);

out:
	release_firmware(data2);
	release_firmware(data1);
	return ret;
}

/* LG's lgit_imx351_rohm_ois_init_cmd(): bring the OIS/VCM up after download */
static int bu24235_init_cmd(struct bu24235_device *bu24235, u16 cal_ver)
{
	struct i2c_client *client = bu24235->client;
	u8 status = 0;
	int ret;

	ret = bu24235_read(client, BU24235_OIS_CTRL, &status, 1);
	if (ret)
		return ret;
	if (status != 0x01) {
		ret = bu24235_write_u8(client, BU24235_OIS_CTRL, 0x01);
		if (ret)
			return ret;
	}

	ret = bu24235_poll_ready(client);
	if (ret)
		return ret;

	/* VCM init: this also brings up the AF path */
	ret = bu24235_write_u8(client, BU24235_VCM_INIT, 0x02);
	if (ret)
		return ret;

	ret = bu24235_poll_ready(client);
	if (ret)
		return ret;

	if (cal_ver == 0x0101 || cal_ver == 0x0301) {
		ret = bu24235_write_u8(client, BU24235_VCM_STATUS, 0x00);
		if (ret)
			return ret;
		ret = bu24235_poll_ready(client);
	}

	return ret;
}

/*
 * Bring the controller up on first use.  Everything here needs the camera
 * module powered, which only happens while the IMX351 streams; a failure
 * leaves init_done clear so the next focus request retries.
 */
static int bu24235_init(struct bu24235_device *bu24235)
{
	struct i2c_client *client = bu24235->client;
	const struct bu24235_fw *fw = NULL;
	u8 map_ver = 0, cal_ver[2] = { 0 };
	unsigned int i;
	int ret;

	/* which lens controller did this unit ship with? */
	ret = bu24235_eeprom_read(client, BU24235_E2P_MAP_VER, &map_ver, 1);
	if (ret)
		return ret;

	switch (map_ver) {
	case 0xff:
	case 0x01:
	case 0x02:
		dev_dbg(&client->dev,
			"Renesas lens controller fitted, not us (map 0x%02x)\n",
			map_ver);
		return -ENODEV;
	case 0x03:
		break;
	default:
		dev_err(&client->dev, "unknown module map version 0x%02x\n",
			map_ver);
		return -EINVAL;
	}

	ret = bu24235_eeprom_read(client, BU24235_E2P_CAL_VER, cal_ver, 2);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(bu24235_fw_revs); i++) {
		if ((cal_ver[0] << 8 | cal_ver[1]) == bu24235_fw_revs[i].cal_ver) {
			fw = &bu24235_fw_revs[i];
			break;
		}
	}
	if (!fw) {
		dev_err(&client->dev, "unknown calibration version 0x%02x%02x\n",
			cal_ver[0], cal_ver[1]);
		return -EINVAL;
	}

	ret = bu24235_download(bu24235, fw);
	if (ret)
		return ret;

	ret = bu24235_init_cmd(bu24235, fw->cal_ver);
	if (ret)
		return ret;

	bu24235->init_done = true;
	dev_info(&client->dev, "ready, calibration 0x%02x%02x\n",
		 cal_ver[0], cal_ver[1]);

	return 0;
}

static int bu24235_set_focus(struct bu24235_device *bu24235, s32 val)
{
	s16 dac;
	int ret;

	if (!bu24235->init_done) {
		ret = bu24235_init(bu24235);
		if (ret)
			return ret;
	}

	dac = (s16)(val - BU24235_MAX_FOCUS_POS / 2 - 1);

	return bu24235_write_s16(bu24235->client, BU24235_VCM_DAC, dac);
}

static int bu24235_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bu24235_device *bu24235 =
		container_of(ctrl->handler, struct bu24235_device, ctrls);
	int ret;

	mutex_lock(&bu24235->mutex);
	ret = bu24235_set_focus(bu24235, ctrl->val);
	mutex_unlock(&bu24235->mutex);

	return ret;
}

static const struct v4l2_ctrl_ops bu24235_ctrl_ops = {
	.s_ctrl = bu24235_s_ctrl,
};

static const struct v4l2_subdev_ops bu24235_subdev_ops = { };

static int bu24235_probe(struct i2c_client *client)
{
	struct bu24235_device *bu24235;
	int ret;

	bu24235 = devm_kzalloc(&client->dev, sizeof(*bu24235), GFP_KERNEL);
	if (!bu24235)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&bu24235->sd, client, &bu24235_subdev_ops);
	bu24235->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	bu24235->client = client;
	mutex_init(&bu24235->mutex);

	v4l2_ctrl_handler_init(&bu24235->ctrls, 1);
	v4l2_ctrl_new_std(&bu24235->ctrls, &bu24235_ctrl_ops,
			  V4L2_CID_FOCUS_ABSOLUTE, 0, BU24235_MAX_FOCUS_POS,
			  1, BU24235_MAX_FOCUS_POS / 2);
	bu24235->sd.ctrl_handler = &bu24235->ctrls;
	if (bu24235->ctrls.error) {
		ret = bu24235->ctrls.error;
		goto err_handler;
	}

	ret = media_entity_pads_init(&bu24235->sd.entity, 0, NULL);
	if (ret)
		goto err_handler;

	bu24235->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = v4l2_async_register_subdev(&bu24235->sd);
	if (ret)
		goto err_entity;

	return 0;

err_entity:
	media_entity_cleanup(&bu24235->sd.entity);
err_handler:
	v4l2_ctrl_handler_free(&bu24235->ctrls);
	return ret;
}

static void bu24235_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
}

static const struct i2c_device_id bu24235_id_table[] = {
	{ BU24235_NAME },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bu24235_id_table);

static const struct of_device_id bu24235_of_table[] = {
	{ .compatible = "rohm,bu24235" },
	{ }
};
MODULE_DEVICE_TABLE(of, bu24235_of_table);

static struct i2c_driver bu24235_i2c_driver = {
	.driver = {
		.name = BU24235_NAME,
		.of_match_table = bu24235_of_table,
	},
	.probe = bu24235_probe,
	.remove = bu24235_remove,
	.id_table = bu24235_id_table,
};

module_i2c_driver(bu24235_i2c_driver);

MODULE_DESCRIPTION("ROHM BU24235 AF controller driver for the LG V30");
MODULE_LICENSE("GPL");
