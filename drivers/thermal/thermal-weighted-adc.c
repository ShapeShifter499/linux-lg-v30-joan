// SPDX-License-Identifier: GPL-2.0-only
/*
 * Weighted-sum ADC thermal sensor.
 *
 * Reports T = sum(weight_i * T_i) / 1000 + offset, where each T_i is an IIO
 * temperature channel read in millicelsius. Phones estimate skin temperature
 * this way from board thermistors: the LG V30's stock kernel defines
 * "vts" = 0.37 * xo_therm + 0.48 * bd_therm_2 + 3.83 C, and its thermal
 * policy throttles on that value. The thermal core takes one sensor per
 * zone, so the combination has to be a sensor of its own.
 */

#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/thermal.h>

#include "thermal_hwmon.h"

#define WADC_MAX_CHANNELS	8

struct wadc_thermal {
	struct iio_channel *chans;
	s32 weights[WADC_MAX_CHANNELS];
	int nchans;
	s32 offset;
};

static int wadc_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct wadc_thermal *wt = thermal_zone_device_priv(tz);
	s64 sum = 0;
	int i, val, ret;

	for (i = 0; i < wt->nchans; i++) {
		ret = iio_read_channel_processed(&wt->chans[i], &val);
		if (ret < 0)
			return ret;
		sum += (s64)wt->weights[i] * val;
	}

	*temp = div_s64(sum, 1000) + wt->offset;

	return 0;
}

static const struct thermal_zone_device_ops wadc_thermal_ops = {
	.get_temp = wadc_thermal_get_temp,
};

static int wadc_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tz;
	struct wadc_thermal *wt;
	enum iio_chan_type type;
	int i, ret;

	wt = devm_kzalloc(dev, sizeof(*wt), GFP_KERNEL);
	if (!wt)
		return -ENOMEM;

	wt->chans = devm_iio_channel_get_all(dev);
	if (IS_ERR(wt->chans))
		return dev_err_probe(dev, PTR_ERR(wt->chans),
				     "cannot get IIO channels\n");

	while (wt->chans[wt->nchans].indio_dev)
		wt->nchans++;

	if (!wt->nchans || wt->nchans > WADC_MAX_CHANNELS)
		return dev_err_probe(dev, -EINVAL, "need 1..%d channels, have %d\n",
				     WADC_MAX_CHANNELS, wt->nchans);

	ret = device_property_count_u32(dev, "weights-milli");
	if (ret != wt->nchans)
		return dev_err_probe(dev, -EINVAL,
				     "weights-milli needs one entry per channel\n");

	ret = device_property_read_u32_array(dev, "weights-milli",
					     (u32 *)wt->weights, wt->nchans);
	if (ret)
		return ret;

	device_property_read_u32(dev, "offset-millicelsius", (u32 *)&wt->offset);

	for (i = 0; i < wt->nchans; i++) {
		ret = iio_get_channel_type(&wt->chans[i], &type);
		if (ret)
			return ret;
		if (type != IIO_TEMP)
			return dev_err_probe(dev, -EINVAL,
					     "channel %d is not a temperature\n", i);
	}

	tz = devm_thermal_of_zone_register(dev, 0, wt, &wadc_thermal_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz),
				     "cannot register thermal sensor\n");

	devm_thermal_add_hwmon_sysfs(dev, tz);

	return 0;
}

static const struct of_device_id wadc_thermal_match[] = {
	{ .compatible = "weighted-adc-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, wadc_thermal_match);

static struct platform_driver wadc_thermal_driver = {
	.driver = {
		.name = "weighted-adc-thermal",
		.of_match_table = wadc_thermal_match,
	},
	.probe = wadc_thermal_probe,
};
module_platform_driver(wadc_thermal_driver);

MODULE_DESCRIPTION("Weighted-sum ADC thermal sensor");
MODULE_LICENSE("GPL");
