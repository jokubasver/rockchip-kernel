/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * rk817_battery.c and rk817_battery_arkos4clone.c drive the same
 * "rk817-battery" device. With both built in, rk817_gauge=arkos4clone on the
 * kernel command line registers the ArkOS4Clone gauge instead of the
 * standard one.
 */

#ifndef _RK817_BATTERY_H
#define _RK817_BATTERY_H

#include <linux/types.h>

#ifdef CONFIG_BATTERY_RK817_ARKOS4CLONE
bool rk817_battery_arkos4clone_selected(void);
#else
static inline bool rk817_battery_arkos4clone_selected(void)
{
	return false;
}
#endif

#endif
