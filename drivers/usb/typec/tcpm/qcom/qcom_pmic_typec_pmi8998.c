// SPDX-License-Identifier: GPL-2.0
/*
 * Type-C port of the Qualcomm PMI8998 SMB2 charger
 *
 * The PM8150B and later PMICs give Type-C its own peripheral with a set of
 * dedicated interrupts.  On PMI8998, a generation older, the same port state
 * machine is part of the USBIN peripheral of the charger: its status and
 * configuration live in USBIN registers, the power-role and VCONN controls
 * share one register, and the whole port raises a single interrupt,
 * type-c-change, whose sources are selected by TYPE_C_INTRPT_ENB.  So the
 * logic here follows qcom_pmic_typec_port.c while the registers do not.
 *
 * Register layout from Qualcomm's smb-reg.h, as shipped in LG's msm8998
 * kernel; the port configuration mirrors that kernel's smb2_configure_typec()
 * and VCONN handling.  The approach, and the discovery that the PM8150B
 * TCPM glue carries over, are Caleb Connolly's PMI8998 TCPM work.
 *
 * Copyright (c) 2016-2017 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Ltd. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/usb/tcpm.h>
#include <linux/workqueue.h>

#include "qcom_pmic_typec.h"
#include "qcom_pmic_typec_port.h"

/* Offsets from the USBIN peripheral base */
#define TYPE_C_STATUS_1_REG			0x0b
#define UFP_TYPEC_RDSTD				BIT(7)
#define UFP_TYPEC_RD1P5				BIT(6)
#define UFP_TYPEC_RD3P0				BIT(5)
#define UFP_TYPEC_MASK				GENMASK(7, 5)

#define TYPE_C_STATUS_2_REG			0x0c
#define DFP_RD_OPEN				BIT(3)
#define DFP_RD_RA_VCONN				BIT(2)
#define DFP_RD_RD				BIT(1)
#define DFP_RA_RA				BIT(0)
#define DFP_TYPEC_MASK				GENMASK(3, 0)

#define TYPE_C_STATUS_4_REG			0x0e
#define UFP_DFP_MODE_STATUS			BIT(7)	/* set: we are the source */
#define TYPEC_VBUS_STATUS			BIT(6)
#define TYPEC_VCONN_OVERCURR_STATUS		BIT(2)
#define CC_ORIENTATION				BIT(1)
#define CC_ATTACHED				BIT(0)

#define TYPE_C_CFG_REG				0x58
#define FACTORY_MODE_DETECTION_EN		BIT(5)
#define VCONN_OC_CFG				BIT(1)

#define TYPE_C_CFG_2_REG			0x59
/* bit 6 per LG's smb-reg.h; Qualcomm's names it DFP_CC_1P4V_OR_1P6V */
#define VCONN_ILIM500MA_CFG			BIT(6)
#define VCONN_SOFTSTART_CFG_MASK		GENMASK(5, 4)
#define EN_80UA_180UA_CUR_SOURCE		BIT(0)

#define TYPE_C_CFG_3_REG			0x5a
#define TYPEC_LEGACY_CABLE_INT_EN		BIT(6)
#define TYPEC_NONCOMPLIANT_LEGACY_CABLE_INT_EN	BIT(5)
#define EN_TRYSINK_MODE				BIT(2)

#define TAPER_TIMER_SEL_CFG_REG			0x64
#define TYPEC_DRP_DFP_TIME_CFG			BIT(5)

#define TYPE_C_INTRPT_ENB_REG			0x67
#define TYPEC_CCSTATE_CHANGE_INT_EN		BIT(2)
#define TYPEC_VBUS_DEASSERT_INT_EN		BIT(1)
#define TYPEC_VBUS_ASSERT_INT_EN		BIT(0)

#define TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG	0x68
#define VCONN_EN_ORIENTATION			BIT(6)
#define VCONN_EN_SRC				BIT(4)
#define VCONN_EN_VALUE				BIT(3)
#define TYPEC_POWER_ROLE_CMD_MASK		GENMASK(2, 0)
#define UFP_EN_CMD				BIT(2)	/* sink only */
#define DFP_EN_CMD				BIT(1)	/* source only */
#define TYPEC_DISABLE_CMD			BIT(0)

struct pmic_typec_pmi8998 {
	struct device			*dev;
	struct tcpm_port		*tcpm_port;
	struct regmap			*regmap;
	u32				base;
	int				irq;

	struct regulator		*vdd_vbus;
	bool				vbus_enabled;
	struct mutex			vbus_lock;	/* VBUS state serialization */

	bool				debouncing_cc;
	struct delayed_work		cc_debounce_dwork;

	spinlock_t			lock;		/* Register atomicity */
};

static struct pmic_typec_pmi8998 *tcpc_to_pmi8998(struct tcpc_dev *tcpc)
{
	return tcpc_to_tcpm(tcpc)->pmic_typec_pmi8998;
}

static int pmi8998_read(struct pmic_typec_pmi8998 *port, u32 reg,
			unsigned int *val)
{
	return regmap_read(port->regmap, port->base + reg, val);
}

static int pmi8998_update(struct pmic_typec_pmi8998 *port, u32 reg,
			  unsigned int mask, unsigned int val)
{
	return regmap_update_bits(port->regmap, port->base + reg, mask, val);
}

static void pmi8998_cc_debounce(struct work_struct *work)
{
	struct pmic_typec_pmi8998 *port =
		container_of(work, struct pmic_typec_pmi8998, cc_debounce_dwork.work);
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	port->debouncing_cc = false;
	spin_unlock_irqrestore(&port->lock, flags);
}

/* Ignore CC changes for a moment after reprogramming the port. */
static void pmi8998_set_cc_debounce(struct pmic_typec_pmi8998 *port)
{
	port->debouncing_cc = true;
	schedule_delayed_work(&port->cc_debounce_dwork, msecs_to_jiffies(2));
}

/*
 * The port has one interrupt for all of its events, and nothing latches
 * which of them fired.  Tell the TCPM that both CC and VBUS may have changed;
 * it reads the state back through get_cc() and get_vbus().
 */
static irqreturn_t pmi8998_typec_isr(int irq, void *dev_id)
{
	struct pmic_typec_pmi8998 *port = dev_id;
	bool cc_change;
	unsigned long flags;

	spin_lock_irqsave(&port->lock, flags);
	cc_change = !port->debouncing_cc;
	spin_unlock_irqrestore(&port->lock, flags);

	tcpm_vbus_change(port->tcpm_port);
	if (cc_change)
		tcpm_cc_change(port->tcpm_port);

	return IRQ_HANDLED;
}

static bool pmi8998_vbus_detect(struct pmic_typec_pmi8998 *port)
{
	unsigned int stat;

	if (pmi8998_read(port, TYPE_C_STATUS_4_REG, &stat))
		return false;

	return stat & TYPEC_VBUS_STATUS;
}

static int pmi8998_get_vbus(struct tcpc_dev *tcpc)
{
	struct pmic_typec_pmi8998 *port = tcpc_to_pmi8998(tcpc);
	int ret;

	mutex_lock(&port->vbus_lock);
	ret = port->vbus_enabled || pmi8998_vbus_detect(port);
	mutex_unlock(&port->vbus_lock);

	return ret;
}

static int pmi8998_set_vbus(struct tcpc_dev *tcpc, bool on, bool sink)
{
	struct pmic_typec_pmi8998 *port = tcpc_to_pmi8998(tcpc);
	unsigned int stat;
	int ret = 0;

	mutex_lock(&port->vbus_lock);
	if (port->vbus_enabled == on)
		goto done;

	if (on)
		ret = regulator_enable(port->vdd_vbus);
	else
		ret = regulator_disable(port->vdd_vbus);
	if (ret)
		goto done;

	/*
	 * Unlike PM8150B there is no vSafe5V/vSafe0V status to wait for, so
	 * wait for the port's own view of VBUS to follow instead.
	 */
	if (regmap_read_poll_timeout(port->regmap,
				     port->base + TYPE_C_STATUS_4_REG, stat,
				     !!(stat & TYPEC_VBUS_STATUS) == on,
				     100, 250000))
		dev_dbg(port->dev, "VBUS did not turn %s\n", on ? "on" : "off");

	port->vbus_enabled = on;
	tcpm_vbus_change(port->tcpm_port);

done:
	mutex_unlock(&port->vbus_lock);

	return ret;
}

static int pmi8998_get_cc(struct tcpc_dev *tcpc, enum typec_cc_status *cc1,
			  enum typec_cc_status *cc2)
{
	struct pmic_typec_pmi8998 *port = tcpc_to_pmi8998(tcpc);
	enum typec_cc_status cc;
	unsigned int stat, val;
	int ret;

	*cc1 = TYPEC_CC_OPEN;
	*cc2 = TYPEC_CC_OPEN;

	if (port->debouncing_cc)
		return -EBUSY;

	ret = pmi8998_read(port, TYPE_C_STATUS_4_REG, &stat);
	if (ret)
		return ret;

	if (!(stat & CC_ATTACHED))
		return 0;

	if (stat & UFP_DFP_MODE_STATUS) {
		/* We are the source: report what the partner terminates with */
		ret = pmi8998_read(port, TYPE_C_STATUS_2_REG, &val);
		if (ret)
			return ret;

		switch (val & DFP_TYPEC_MASK) {
		case DFP_RA_RA:
			*cc1 = TYPEC_CC_RA;
			*cc2 = TYPEC_CC_RA;
			return 0;
		case DFP_RD_RD:
			*cc1 = TYPEC_CC_RD;
			*cc2 = TYPEC_CC_RD;
			return 0;
		case DFP_RD_RA_VCONN:
			*cc1 = TYPEC_CC_RA;
			*cc2 = TYPEC_CC_RA;
			cc = TYPEC_CC_RD;
			break;
		case DFP_RD_OPEN:
			cc = TYPEC_CC_RD;
			break;
		default:
			dev_dbg(port->dev, "unexpected DFP status %#x\n", val);
			cc = TYPEC_CC_RD;
			break;
		}
	} else {
		/* We are the sink: report the source's advertisement */
		ret = pmi8998_read(port, TYPE_C_STATUS_1_REG, &val);
		if (ret)
			return ret;

		switch (val & UFP_TYPEC_MASK) {
		case UFP_TYPEC_RD3P0:
			cc = TYPEC_CC_RP_3_0;
			break;
		case UFP_TYPEC_RD1P5:
			cc = TYPEC_CC_RP_1_5;
			break;
		default:
			cc = TYPEC_CC_RP_DEF;
			break;
		}
	}

	if (stat & CC_ORIENTATION)
		*cc2 = cc;
	else
		*cc1 = cc;

	return 0;
}

static int pmi8998_set_cc(struct tcpc_dev *tcpc, enum typec_cc_status cc)
{
	struct pmic_typec_pmi8998 *port = tcpc_to_pmi8998(tcpc);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&port->lock, flags);

	/*
	 * The port's own state machine owns the terminations.  All that is
	 * ours to choose is how much current Rp advertises when sourcing, and
	 * the part tops out at 1.5 A.
	 */
	switch (cc) {
	case TYPEC_CC_RP_1_5:
	case TYPEC_CC_RP_3_0:
		ret = pmi8998_update(port, TYPE_C_CFG_2_REG,
				     EN_80UA_180UA_CUR_SOURCE,
				     EN_80UA_180UA_CUR_SOURCE);
		break;
	case TYPEC_CC_RP_DEF:
		ret = pmi8998_update(port, TYPE_C_CFG_2_REG,
				     EN_80UA_180UA_CUR_SOURCE, 0);
		break;
	default:
		break;
	}

	if (!ret)
		pmi8998_set_cc_debounce(port);

	spin_unlock_irqrestore(&port->lock, flags);

	return ret;
}

static int pmi8998_set_polarity(struct tcpc_dev *tcpc,
				enum typec_cc_polarity pol)
{
	/* Polarity is applied by the orientation switch, not by the port */
	return 0;
}

static int pmi8998_set_vconn(struct tcpc_dev *tcpc, bool on)
{
	struct pmic_typec_pmi8998 *port = tcpc_to_pmi8998(tcpc);
	unsigned int stat, orientation;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&port->lock, flags);

	if (!on) {
		ret = pmi8998_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				     VCONN_EN_VALUE, 0);
		goto done;
	}

	ret = pmi8998_read(port, TYPE_C_STATUS_4_REG, &stat);
	if (ret)
		goto done;

	/*
	 * LG's kernel slows the VCONN soft-start and limits VCONN to 500 mA
	 * before turning it on.
	 */
	ret = pmi8998_update(port, TYPE_C_CFG_2_REG,
			     VCONN_SOFTSTART_CFG_MASK | VCONN_ILIM500MA_CFG,
			     VCONN_SOFTSTART_CFG_MASK | VCONN_ILIM500MA_CFG);
	if (ret)
		goto done;

	/* VCONN goes on the CC pin that is not carrying the connection */
	orientation = (stat & CC_ORIENTATION) ? 0 : VCONN_EN_ORIENTATION;
	ret = pmi8998_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			     VCONN_EN_ORIENTATION | VCONN_EN_VALUE,
			     orientation | VCONN_EN_VALUE);

done:
	spin_unlock_irqrestore(&port->lock, flags);

	return ret;
}

static int pmi8998_start_toggling(struct tcpc_dev *tcpc,
				  enum typec_port_type port_type,
				  enum typec_cc_status cc)
{
	struct pmic_typec_pmi8998 *port = tcpc_to_pmi8998(tcpc);
	unsigned int role;
	unsigned long flags;
	int ret;

	switch (port_type) {
	case TYPEC_PORT_SRC:
		role = DFP_EN_CMD;
		break;
	case TYPEC_PORT_SNK:
		role = UFP_EN_CMD;
		break;
	default:
		/* Dual role; Try.SNK is enabled in TYPE_C_CFG_3 */
		role = 0;
		break;
	}

	spin_lock_irqsave(&port->lock, flags);

	pmi8998_set_cc_debounce(port);

	/* Force the state machine through Disabled so it toggles again */
	ret = pmi8998_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			     TYPEC_POWER_ROLE_CMD_MASK, TYPEC_DISABLE_CMD);
	if (!ret)
		ret = pmi8998_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
				     TYPEC_POWER_ROLE_CMD_MASK, role);

	spin_unlock_irqrestore(&port->lock, flags);

	return ret;
}

static int pmi8998_port_start(struct pmic_typec *tcpm,
			      struct tcpm_port *tcpm_port)
{
	struct pmic_typec_pmi8998 *port = tcpm->pmic_typec_pmi8998;
	int ret;

	/* Raise type-c-change on CC state changes and on VBUS edges */
	ret = regmap_write(port->regmap, port->base + TYPE_C_INTRPT_ENB_REG,
			   TYPEC_CCSTATE_CHANGE_INT_EN |
			   TYPEC_VBUS_DEASSERT_INT_EN |
			   TYPEC_VBUS_ASSERT_INT_EN);
	if (ret)
		return ret;

	/*
	 * Disable factory mode detection, and stay attached as a source when
	 * VCONN is overloaded rather than dropping the connection.
	 */
	ret = pmi8998_update(port, TYPE_C_CFG_REG,
			     FACTORY_MODE_DETECTION_EN | VCONN_OC_CFG, 0);
	if (ret)
		return ret;

	/* LG's settings: Try.SNK with no legacy cable interrupts ... */
	ret = pmi8998_update(port, TYPE_C_CFG_3_REG,
			     EN_TRYSINK_MODE | TYPEC_LEGACY_CABLE_INT_EN |
			     TYPEC_NONCOMPLIANT_LEGACY_CABLE_INT_EN,
			     EN_TRYSINK_MODE);
	if (ret)
		return ret;

	/* ... and a shorter DRP DFP time when a resistance is connected */
	ret = pmi8998_update(port, TAPER_TIMER_SEL_CFG_REG,
			     TYPEC_DRP_DFP_TIME_CFG, TYPEC_DRP_DFP_TIME_CFG);
	if (ret)
		return ret;

	/* VCONN under software control, off; dual role */
	ret = pmi8998_update(port, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG,
			     VCONN_EN_SRC | VCONN_EN_VALUE |
			     TYPEC_POWER_ROLE_CMD_MASK,
			     VCONN_EN_SRC);
	if (ret)
		return ret;

	port->tcpm_port = tcpm_port;
	enable_irq(port->irq);

	return 0;
}

static void pmi8998_port_stop(struct pmic_typec *tcpm)
{
	struct pmic_typec_pmi8998 *port = tcpm->pmic_typec_pmi8998;

	disable_irq(port->irq);
	cancel_delayed_work_sync(&port->cc_debounce_dwork);
}

int qcom_pmic_typec_pmi8998_port_probe(struct platform_device *pdev,
				       struct pmic_typec *tcpm,
				       struct regmap *regmap, u32 base)
{
	struct device *dev = &pdev->dev;
	struct pmic_typec_pmi8998 *port;
	struct fwnode_handle *connector;
	int ret;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	connector = device_get_named_child_node(dev, "connector");
	if (!connector)
		return -EINVAL;

	port->vdd_vbus = devm_of_regulator_get_optional(dev, to_of_node(connector),
							"vbus");
	fwnode_handle_put(connector);
	if (IS_ERR(port->vdd_vbus))
		return dev_err_probe(dev, PTR_ERR(port->vdd_vbus),
				     "failed to get VBUS supply\n");

	port->dev = dev;
	port->regmap = regmap;
	port->base = base;
	mutex_init(&port->vbus_lock);
	spin_lock_init(&port->lock);
	INIT_DELAYED_WORK(&port->cc_debounce_dwork, pmi8998_cc_debounce);

	port->irq = platform_get_irq_byname(pdev, "type-c-change");
	if (port->irq < 0)
		return port->irq;

	ret = devm_request_threaded_irq(dev, port->irq, NULL, pmi8998_typec_isr,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					"type-c-change", port);
	if (ret)
		return ret;

	tcpm->pmic_typec_pmi8998 = port;

	tcpm->tcpc.get_vbus = pmi8998_get_vbus;
	tcpm->tcpc.set_vbus = pmi8998_set_vbus;
	tcpm->tcpc.get_cc = pmi8998_get_cc;
	tcpm->tcpc.set_cc = pmi8998_set_cc;
	tcpm->tcpc.set_polarity = pmi8998_set_polarity;
	tcpm->tcpc.set_vconn = pmi8998_set_vconn;
	tcpm->tcpc.start_toggling = pmi8998_start_toggling;

	tcpm->port_start = pmi8998_port_start;
	tcpm->port_stop = pmi8998_port_stop;

	return 0;
}
