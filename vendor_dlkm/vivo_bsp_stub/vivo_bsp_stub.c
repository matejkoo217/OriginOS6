// SPDX-License-Identifier: GPL-2.0
/*
 * vivo_bsp_stub
 *
 * Needed by the donor driver:
 *   vivo-tshell.ko  -> power_off_charging_mode()
 * (get_hall_status comes from the donor hall_sensor.ko.)
 *
 */
#include <linux/kernel.h>
#include <linux/module.h>

int power_off_charging_mode(void)
{
	return 0;
}
EXPORT_SYMBOL(power_off_charging_mode);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BSP symbols imported by vivo's donor modules");
MODULE_AUTHOR("~jkoo");
