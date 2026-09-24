// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dongwoon DW7800 haptic driver
 *
 * The DW7800 plays signed 8-bit samples written into a FIFO at 8 kHz and
 * drops to standby by itself once the FIFO runs dry.  Rumble effects are
 * played as a sine at the actuator's resonance, streamed in 5 ms packets.
 *
 * Register use and settings follow LG's V30 kernel, which drives the chip
 * through Immersion's TouchSense driver (drivers/tspdrv/ImmVibeSPI.c):
 * 10 ms FIFO timeout, 8 kHz, 2.8 V LDO, and the packet-size readback to
 * resend what the FIFO did not take.  The ~145 Hz resonance is read off
 * LG's own drive waveform in the same file.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#define DW7800_INFO			0x00
#define DW7800_DATA			0x04
#define DW7800_SWRESET			0x05
#define DW7800_SWRESET_CMD		0x01
#define DW7800_TIMING			0x06
#define DW7800_TIMING_10MS_8KHZ		0x20
#define DW7800_LDO			0x07
#define DW7800_LDO_2V8			0x08
#define DW7800_HWRESET			0x08
#define DW7800_HWRESET_ENABLE		0x00
#define DW7800_PKTSIZE			0x09

#define DW7800_SAMPLE_RATE		8000
#define DW7800_PACKET			40	/* 5 ms at 8 kHz */
#define DW7800_RESONANCE_HZ		145
#define DW7800_RESENDS			3

struct dw7800 {
	struct i2c_client *client;
	struct input_dev *input;
	struct work_struct work;
	unsigned int magnitude;		/* 0..0xffff, 0 = stop */
	unsigned int phase;
};

/* Quarter-wave sine, 0..127, 16 steps */
static const u8 dw7800_quarter_sine[] = {
	0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 127,
};

static s8 dw7800_sine(unsigned int phase)
{
	/* phase in 1/64ths of a period */
	unsigned int q = (phase / 16) & 3, i = phase & 15;
	int v;

	switch (q) {
	case 0:
		v = dw7800_quarter_sine[i];
		break;
	case 1:
		v = dw7800_quarter_sine[15 - i];
		break;
	case 2:
		v = -dw7800_quarter_sine[i];
		break;
	default:
		v = -dw7800_quarter_sine[15 - i];
		break;
	}

	return v;
}

static int dw7800_write(struct dw7800 *dw, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(dw->client, reg, val);
}

/* Push one packet, resending whatever the FIFO did not accept */
static int dw7800_send_packet(struct dw7800 *dw, const s8 *samples, int len)
{
	u8 buf[DW7800_PACKET + 1];
	int done = 0, tries = DW7800_RESENDS, ret;

	while (done < len && tries--) {
		buf[0] = DW7800_DATA;
		memcpy(buf + 1, samples + done, len - done);
		ret = i2c_master_send(dw->client, buf, len - done + 1);
		if (ret < 0)
			return ret;

		ret = i2c_smbus_read_byte_data(dw->client, DW7800_PKTSIZE);
		if (ret < 0)
			return ret;

		done += ret;
		if (done < len)
			usleep_range(50 * (done + 1), 50 * (done + 1) + 100);
	}

	return done < len ? -EIO : 0;
}

static void dw7800_work(struct work_struct *work)
{
	struct dw7800 *dw = container_of(work, struct dw7800, work);
	/* phase advance per sample, in 1/64ths of a period, times 1000 */
	const unsigned int step = 64 * 1000 * DW7800_RESONANCE_HZ /
				  DW7800_SAMPLE_RATE;
	s8 samples[DW7800_PACKET];
	unsigned int mag, i;

	while ((mag = READ_ONCE(dw->magnitude))) {
		for (i = 0; i < DW7800_PACKET; i++) {
			samples[i] = dw7800_sine(dw->phase / 1000) *
				     (int)mag / 0xffff;
			dw->phase = (dw->phase + step) % (64 * 1000);
		}

		if (dw7800_send_packet(dw, samples, DW7800_PACKET)) {
			dev_err_ratelimited(&dw->client->dev,
					    "failed to feed the FIFO\n");
			break;
		}

		usleep_range(4500, 5000);
	}
}

static int dw7800_play(struct input_dev *input, void *data,
		       struct ff_effect *effect)
{
	struct dw7800 *dw = input_get_drvdata(input);
	unsigned int mag = max(effect->u.rumble.strong_magnitude,
			       effect->u.rumble.weak_magnitude);

	WRITE_ONCE(dw->magnitude, mag);
	if (mag)
		schedule_work(&dw->work);

	return 0;
}

static int dw7800_power_on(struct dw7800 *dw)
{
	int ret;

	ret = dw7800_write(dw, DW7800_SWRESET, DW7800_SWRESET_CMD);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	ret = dw7800_write(dw, DW7800_TIMING, DW7800_TIMING_10MS_8KHZ);
	if (!ret)
		ret = dw7800_write(dw, DW7800_LDO, DW7800_LDO_2V8);
	if (!ret)
		ret = dw7800_write(dw, DW7800_HWRESET, DW7800_HWRESET_ENABLE);

	return ret;
}

static void dw7800_close(struct input_dev *input)
{
	struct dw7800 *dw = input_get_drvdata(input);

	WRITE_ONCE(dw->magnitude, 0);
	cancel_work_sync(&dw->work);
}

static int dw7800_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct dw7800 *dw;
	int ret;

	dw = devm_kzalloc(dev, sizeof(*dw), GFP_KERNEL);
	if (!dw)
		return -ENOMEM;

	dw->client = client;
	INIT_WORK(&dw->work, dw7800_work);
	i2c_set_clientdata(client, dw);

	ret = i2c_smbus_read_byte_data(client, DW7800_INFO);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no DW7800 found\n");

	ret = dw7800_power_on(dw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialise\n");

	dw->input = devm_input_allocate_device(dev);
	if (!dw->input)
		return -ENOMEM;

	dw->input->name = "dw7800-haptics";
	dw->input->id.bustype = BUS_I2C;
	dw->input->close = dw7800_close;
	input_set_drvdata(dw->input, dw);
	input_set_capability(dw->input, EV_FF, FF_RUMBLE);

	ret = input_ff_create_memless(dw->input, NULL, dw7800_play);
	if (ret)
		return dev_err_probe(dev, ret, "failed to create FF device\n");

	return input_register_device(dw->input);
}

static void dw7800_remove(struct i2c_client *client)
{
	struct dw7800 *dw = i2c_get_clientdata(client);

	WRITE_ONCE(dw->magnitude, 0);
	cancel_work_sync(&dw->work);
}

static int dw7800_suspend(struct device *dev)
{
	struct dw7800 *dw = dev_get_drvdata(dev);

	WRITE_ONCE(dw->magnitude, 0);
	cancel_work_sync(&dw->work);

	return 0;
}

static int dw7800_resume(struct device *dev)
{
	return dw7800_power_on(dev_get_drvdata(dev));
}

static DEFINE_SIMPLE_DEV_PM_OPS(dw7800_pm_ops, dw7800_suspend, dw7800_resume);

static const struct of_device_id dw7800_of_match[] = {
	{ .compatible = "dongwoon,dw7800" },
	{ }
};
MODULE_DEVICE_TABLE(of, dw7800_of_match);

static struct i2c_driver dw7800_driver = {
	.driver = {
		.name = "dw7800-haptics",
		.of_match_table = dw7800_of_match,
		.pm = pm_sleep_ptr(&dw7800_pm_ops),
	},
	.probe = dw7800_probe,
	.remove = dw7800_remove,
};
module_i2c_driver(dw7800_driver);

MODULE_DESCRIPTION("Dongwoon DW7800 haptic driver");
MODULE_LICENSE("GPL");
