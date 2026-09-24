// SPDX-License-Identifier: GPL-2.0-only
//
// Qualcomm PMIC VBUS output regulator driver
//
// Copyright (c) 2020, The Linux Foundation. All rights reserved.

#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regmap.h>

#define OTG_STATUS			0x09
#define OTG_STATE_MASK			GENMASK(2, 0)
#define OTG_STATE_ENABLED		0x2
#define CMD_OTG				0x40
#define OTG_EN				BIT(0)
#define OTG_CURRENT_LIMIT_CFG		0x52
#define OTG_CURRENT_LIMIT_MASK		GENMASK(2, 0)
#define OTG_CFG				0x53
#define OTG_EN_SRC_CFG			BIT(1)
#define OTG_ENG_OTG_CFG			0xc0
#define ENG_BUCKBOOST_HALT1_8_MODE	BIT(0)
#define SEC_ACCESS			0xd0
#define SEC_ACCESS_UNLOCK		0xa5

struct qcom_usb_vbus_data {
	const struct regulator_ops *ops;
	const unsigned int *curr_table;
	unsigned int n_current_limits;
};

static const unsigned int pm8150b_curr_table[] = {
	500000, 1000000, 1500000, 2000000, 2500000, 3000000,
};

static const unsigned int pmi8998_curr_table[] = {
	250000, 500000, 750000, 1000000, 1250000, 1500000, 1750000, 2000000,
};

static const struct regulator_ops qcom_usb_vbus_reg_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_current_limit = regulator_get_current_limit_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
};

/*
 * Qualcomm's charger driver sets ENG_BUCKBOOST_HALT1_8_MODE for as long as
 * the PMI8998 boost runs and clears it again once the boost is off.  The
 * register is in the peripheral's secure range, so unlock it first.
 */
static int pmi8998_vbus_halt1_8(struct regulator_dev *rdev, bool halt)
{
	unsigned int base = rdev->desc->enable_reg - CMD_OTG;
	int ret;

	ret = regmap_write(rdev->regmap, base + SEC_ACCESS, SEC_ACCESS_UNLOCK);
	if (ret)
		return ret;

	return regmap_write_bits(rdev->regmap, base + OTG_ENG_OTG_CFG,
				 ENG_BUCKBOOST_HALT1_8_MODE,
				 halt ? ENG_BUCKBOOST_HALT1_8_MODE : 0);
}

/*
 * On PMI8998 OTG_EN only requests the boost; OTG_STATUS says when it is
 * actually running.  Wait for that before returning, so the Type-C port
 * does not carry on while VBUS is still rising, and take the request back
 * if the boost never starts.
 */
static int pmi8998_vbus_enable(struct regulator_dev *rdev)
{
	unsigned int base = rdev->desc->enable_reg - CMD_OTG;
	unsigned int val;
	int ret;

	ret = pmi8998_vbus_halt1_8(rdev, true);
	if (ret)
		return ret;

	ret = regulator_enable_regmap(rdev);
	if (ret)
		goto err_halt;

	ret = regmap_read_poll_timeout(rdev->regmap, base + OTG_STATUS, val,
				       (val & OTG_STATE_MASK) == OTG_STATE_ENABLED,
				       1000, 250000);
	if (ret) {
		dev_err(rdev->dev.parent, "OTG boost did not start: %d\n", ret);
		regulator_disable_regmap(rdev);
		goto err_halt;
	}

	return 0;

err_halt:
	pmi8998_vbus_halt1_8(rdev, false);
	return ret;
}

static int pmi8998_vbus_disable(struct regulator_dev *rdev)
{
	int ret;

	ret = regulator_disable_regmap(rdev);
	if (ret)
		return ret;

	return pmi8998_vbus_halt1_8(rdev, false);
}

static const struct regulator_ops pmi8998_vbus_ops = {
	.enable = pmi8998_vbus_enable,
	.disable = pmi8998_vbus_disable,
	.is_enabled = regulator_is_enabled_regmap,
	.get_current_limit = regulator_get_current_limit_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
};

static const struct qcom_usb_vbus_data pm8150b_vbus_data = {
	.ops = &qcom_usb_vbus_reg_ops,
	.curr_table = pm8150b_curr_table,
	.n_current_limits = ARRAY_SIZE(pm8150b_curr_table),
};

static const struct qcom_usb_vbus_data pmi8998_vbus_data = {
	.ops = &pmi8998_vbus_ops,
	.curr_table = pmi8998_curr_table,
	.n_current_limits = ARRAY_SIZE(pmi8998_curr_table),
};

static int qcom_usb_vbus_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct qcom_usb_vbus_data *data;
	struct regulator_desc *rdesc;
	struct regulator_dev *rdev;
	struct regmap *regmap;
	struct regulator_config config = { };
	struct regulator_init_data *init_data;
	int ret;
	u32 base;

	data = of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	ret = of_property_read_u32(dev->of_node, "reg", &base);
	if (ret < 0) {
		dev_err(dev, "no base address found\n");
		return ret;
	}

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap) {
		dev_err(dev, "Failed to get regmap\n");
		return -ENOENT;
	}

	rdesc = devm_kzalloc(dev, sizeof(*rdesc), GFP_KERNEL);
	if (!rdesc)
		return -ENOMEM;

	rdesc->name = "usb_vbus";
	rdesc->ops = data->ops;
	rdesc->owner = THIS_MODULE;
	rdesc->type = REGULATOR_VOLTAGE;
	rdesc->curr_table = data->curr_table;
	rdesc->n_current_limits = data->n_current_limits;
	rdesc->enable_reg = base + CMD_OTG;
	rdesc->enable_mask = OTG_EN;
	rdesc->csel_reg = base + OTG_CURRENT_LIMIT_CFG;
	rdesc->csel_mask = OTG_CURRENT_LIMIT_MASK;

	init_data = of_get_regulator_init_data(dev, dev->of_node, rdesc);
	if (!init_data)
		return -ENOMEM;
	config.dev = dev;
	config.init_data = init_data;
	config.of_node = dev->of_node;
	config.regmap = regmap;

	rdev = devm_regulator_register(dev, rdesc, &config);
	if (IS_ERR(rdev)) {
		ret = PTR_ERR(rdev);
		dev_err(dev, "not able to register vbus reg %d\n", ret);
		return ret;
	}

	/* Disable HW logic for VBUS enable */
	regmap_update_bits(regmap, base + OTG_CFG, OTG_EN_SRC_CFG, 0);

	return 0;
}

static const struct of_device_id qcom_usb_vbus_regulator_match[] = {
	{ .compatible = "qcom,pm8150b-vbus-reg", .data = &pm8150b_vbus_data },
	{ .compatible = "qcom,pmi8998-vbus-reg", .data = &pmi8998_vbus_data },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_usb_vbus_regulator_match);

static struct platform_driver qcom_usb_vbus_regulator_driver = {
	.driver		= {
		.name	= "qcom-usb-vbus-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = qcom_usb_vbus_regulator_match,
	},
	.probe		= qcom_usb_vbus_regulator_probe,
};
module_platform_driver(qcom_usb_vbus_regulator_driver);

MODULE_DESCRIPTION("Qualcomm USB vbus regulator driver");
MODULE_LICENSE("GPL v2");
