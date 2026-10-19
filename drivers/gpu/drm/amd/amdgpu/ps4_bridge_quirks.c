#include <linux/array_size.h>
#include <linux/kernel.h>
#include <asm/byteorder.h>

#include "ps4_bridge_quirks.h"

struct ps4_bridge_monitor_quirk {
	u16 manufacturer_name;
	u16 product_code;
	unsigned int forced_modes;
};

static const struct ps4_bridge_monitor_quirk ps4_bridge_monitor_quirks[] = {
	{ 0x4a8b, 0x1366, PS4_BRIDGE_QUIRK_MODE_1080P60 },
};

unsigned int ps4_bridge_quirk_forced_modes(const struct edid *edid)
{
	u16 mfg, prod;
	size_t i;

	if (!edid)
		return 0;

	mfg = be16_to_cpu(edid->product_id.manufacturer_name);
	prod = le16_to_cpu(edid->product_id.product_code);

	for (i = 0; i < ARRAY_SIZE(ps4_bridge_monitor_quirks); i++) {
		if (ps4_bridge_monitor_quirks[i].manufacturer_name == mfg &&
		    ps4_bridge_monitor_quirks[i].product_code == prod)
			return ps4_bridge_monitor_quirks[i].forced_modes;
	}

	return 0;
}
