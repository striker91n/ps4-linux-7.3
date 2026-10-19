// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/printk.h>

#include "ps4-dmi.h"

static const char * const ps4_strings[DMI_STRING_MAX] = {
	[DMI_BIOS_VENDOR]           = "Sony Interactive Entertainment Inc.",
	[DMI_BIOS_VERSION]          = "1.00",
	[DMI_BIOS_DATE]             = "11/15/2013",
	[DMI_BIOS_RELEASE]          = "1.0",
	[DMI_EC_FIRMWARE_RELEASE]   = "1.0",

	[DMI_SYS_VENDOR]            = "Sony Interactive Entertainment Inc.",
	[DMI_PRODUCT_NAME]          = "PlayStation 4",
	[DMI_PRODUCT_VERSION]       = "CUH-1000A",
	[DMI_PRODUCT_SERIAL]        = "STRWB3RRYWA5H3R3",
	[DMI_PRODUCT_UUID]          = "DEADBEEF-CAFE-4B33-B00B-F00DBABE1234",
	[DMI_PRODUCT_SKU]           = "CUH-1000AB01",
	[DMI_PRODUCT_FAMILY]        = "PlayStation",

	[DMI_BOARD_VENDOR]          = "Sony Interactive Entertainment Inc.",
	[DMI_BOARD_NAME]            = "Belize",
	[DMI_BOARD_VERSION]         = "rev1",
	[DMI_BOARD_SERIAL]          = "0000000000000000",

	[DMI_CHASSIS_VENDOR]        = "Sony Interactive Entertainment Inc.",
	[DMI_CHASSIS_TYPE]          = "3",
	[DMI_CHASSIS_VERSION]       = "rev1",
	[DMI_CHASSIS_SERIAL]        = "0000000000000000",
};

void __init ps4_dmi_populate(const char *ident[DMI_STRING_MAX])
{
	int i;

	pr_info("ps4_dmi: filling missing DMI fields with PS4 defaults\n");

	for (i = 0; i < DMI_STRING_MAX; i++) {
		if (ps4_strings[i] && !ident[i])
			ident[i] = ps4_strings[i];
	}
}
