/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Interface the LG SW43402 panel driver uses to power the ST FTS touch
 * controller together with the display (see stmfts_set_power()).
 */
#ifndef _LINUX_INPUT_STMFTS_H
#define _LINUX_INPUT_STMFTS_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct i2c_client;

#if IS_REACHABLE(CONFIG_TOUCHSCREEN_STMFTS)
void stmfts_set_power(struct i2c_client *client, bool on);
#else
static inline void stmfts_set_power(struct i2c_client *client, bool on) { }
#endif

#endif /* _LINUX_INPUT_STMFTS_H */
