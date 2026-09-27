/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FM receiver channel of Qualcomm WCN399x Bluetooth controllers.
 *
 * The FM block shares the Bluetooth UART: FM commands travel as H4 packet
 * type 0x11 and FM events come back as type 0x14. hci_qca exposes the channel
 * as an auxiliary device ("hci_uart.fm") for the radio driver to bind to.
 */
#ifndef __QCA_FM_H
#define __QCA_FM_H

#include <linux/auxiliary_bus.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

#define QCA_FM_CMD_PKT		0x11
#define QCA_FM_EVENT_PKT	0x14

struct hci_uart;

struct qca_fm_device {
	struct auxiliary_device adev;
	struct hci_uart *hu;
	spinlock_t lock;		/* protects event and priv */
	void (*event)(void *priv, struct sk_buff *skb);
	void *priv;
	/*
	 * Bumped each time the controller has been set up again, for example
	 * after Bluetooth was switched off and on: the FM receiver state is
	 * lost then and has to be restored.
	 */
	atomic_t generation;
};

static inline struct qca_fm_device *to_qca_fm(struct auxiliary_device *adev)
{
	return container_of(adev, struct qca_fm_device, adev);
}

/*
 * Send one FM command frame: opcode (le16), parameter length, parameters.
 * Fails with -ENETDOWN while the Bluetooth controller is not running.
 */
int qca_fm_send(struct qca_fm_device *fm, const void *data, size_t len);

/*
 * Receive FM event frames (event code, length, parameters). The handler runs
 * in the UART receive path with a spinlock held: it must not sleep, and it
 * owns the skb. Pass NULL to stop receiving.
 */
void qca_fm_set_event_handler(struct qca_fm_device *fm,
			      void (*event)(void *priv, struct sk_buff *skb),
			      void *priv);

#endif /* __QCA_FM_H */
