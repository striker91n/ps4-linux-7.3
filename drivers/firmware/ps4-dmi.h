/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DRIVERS_FIRMWARE_PS4_DMI_H
#define _DRIVERS_FIRMWARE_PS4_DMI_H

#include <linux/dmi.h>
#include <linux/init.h>

#ifdef CONFIG_PS4_DMI_SPOOF
void __init ps4_dmi_populate(const char *ident[DMI_STRING_MAX]);
#else
static inline void ps4_dmi_populate(const char *ident[DMI_STRING_MAX])
{
}
#endif

#endif /* _DRIVERS_FIRMWARE_PS4_DMI_H */
