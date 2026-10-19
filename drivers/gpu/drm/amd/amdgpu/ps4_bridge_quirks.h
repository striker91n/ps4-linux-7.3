#ifndef __PS4_BRIDGE_QUIRKS_H__
#define __PS4_BRIDGE_QUIRKS_H__

#include <linux/bits.h>
#include <drm/drm_edid.h>

#define PS4_BRIDGE_QUIRK_MODE_1080P60	BIT(0)

unsigned int ps4_bridge_quirk_forced_modes(const struct edid *edid);

#endif
