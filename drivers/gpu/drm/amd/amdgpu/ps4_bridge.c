/*
 * Panasonic MN86471A / MN864729 DP->HDMI bridge driver (via PS4 Aeolia ICC interface)
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


// TODO (ps4patches): Make functions atomic,
//  https://lore.kernel.org/linux-arm-kernel/20211020181901.2114645-5-sam@ravnborg.org/

/*
 * Forward-ported patches from https://github.com/rancido,
 * 		            in https://github.com/Ps3itaTeam/ps4-linux/
 * Maybe the problem is no one is putting a "Copyright"
 * in their files ... We don't know who introduced what change or where,
 * since there is often no centralized repository.. Patches thrown around
 * everywhere.
 * But that goes against the whole nature of tinkery/RE work..?
 * But does it really? No.
 *
 * 
 * Copyright: .....
 *
*/
#include <asm/ps4.h>

#include <drm/drm_crtc.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>

#include <drm/drm_bridge.h>
#include <drm/drm_encoder.h>

#include <linux/err.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/firmware.h>


#include "amdgpu.h"
#include "amdgpu_i2c.h"
#include "amdgpu_mode.h"
#include "atombios_dp.h"
#include "atombios_encoders.h"
#include "ObjectID.h"
#include "ps4_bridge_quirks.h"

#define CMD_READ	1, 1
#define CMD_WRITE	2, 2
#define CMD_MASK	2, 3
#define CMD_DELAY	3, 1
#define CMD_WAIT_SET	3, 2
#define CMD_WAIT_CLEAR	3, 3

#define TSYSCTRL 0x7005
#define TSYSCTRL_HDMI BIT(7)

#define TSRST 0x7006
#define TSRST_AVCSRST BIT(0)
#define TSRST_ENCSRST BIT(1)
#define TSRST_FIFOSRST BIT(2)
#define TSRST_CCSRST BIT(3)
#define TSRST_HDCPSRST BIT(4)
#define TSRST_AUDSRST BIT(6)
#define TSRST_VIFSRST BIT(7)

#define TDPCMODE 0x7009
#define PS4_DDC_SEGMENT_ADDR 0x30


#define UPDCTRL 0x7011
#define UPDCTRL_ALLUPD BIT(7)
#define UPDCTRL_AVIIUPD BIT(6)
#define UPDCTRL_AUDIUPD BIT(5)
#define UPDCTRL_CLKUPD BIT(4)
#define UPDCTRL_HVSIUPD BIT(3)
#define UPDCTRL_VIFUPD BIT(2)
#define UPDCTRL_AUDUPD BIT(1)
#define UPDCTRL_CSCUPD BIT(0)


#define VINCNT 0x7040
#define VINCNT_VIF_FILEN BIT(6)

#define VMUTECNT 0x705f
#define VMUTECNT_CCVMUTE BIT(7)
#define VMUTECNT_DUMON BIT(6)
#define VMUTECNT_LINEWIDTH_80 (0<<4)
#define VMUTECNT_LINEWIDTH_90 (1<<4)
#define VMUTECNT_LINEWIDTH_180 (2<<4)
#define VMUTECNT_LINEWIDTH_360 (3<<4)
#define VMUTECNT_VMUTE_MUTE_ASYNC 1
#define VMUTECNT_VMUTE_MUTE_NORMAL 2
#define VMUTECNT_VMUTE_MUTE_RAMPA 4
#define VMUTECNT_VMUTE_MUTE_RAMPB 8
#define VMUTECNT_VMUTE_MUTE_COLORBAR_RGB 10
#define VMUTECNT_VMUTE_MUTE_TOGGLE 12
#define VMUTECNT_VMUTE_MUTE_COLORBAR_YCBCR 14

#define CSCMOD 0x70c0
#define C420SET 0x70c2
#define OUTWSET 0x70c3

#define PKTENA 0x7202

#define INFENA 0x7203
#define INFENA_AVIEN BIT(6)

#define AKESTA 0x7a84
#define AKESTA_BUSY BIT(0)

#define AKESRST 0x7a88

#define HDCPEN 0x7a8b
#define HDCPEN_NONE 0x00
#define HDCPEN_ENC_EN 0x03
#define HDCPEN_ENC_DIS 0x05

#define EDIDCTRL0 0x7a80
#define EDIDCTRL1 0x7a81
#define EDIDCTRL2 0x7a82
#define EDIDCTRL3 0x7a83
#define EDIDCTRL4 0x7a9c

#define PCI_DEVICE_ID_CUH_11XX 0x9920
#define PCI_DEVICE_ID_CUH_12XX 0x9922
#define PCI_DEVICE_ID_CUH_2XXX 0x9923
#define PCI_DEVICE_ID_CUH_7XXX 0x9924

#define PS4_BRIDGE_BELIZE_ENABLE_ATTEMPTS 5
#define PS4_BRIDGE_BELIZE_RETRY_DELAY_MS 200
#define PS4_BRIDGE_DPCD_READY_TIMEOUT_MS 3000

struct edid *drm_get_edid(struct drm_connector *connector,
 				 struct i2c_adapter *adapter);

struct i2c_cmd_hdr {
	u8 major;
	u8 length;
	u8 minor;
	u8 count;
} __packed;

#define ICC_REPLY_HEADER_SIZE 0x14

struct i2c_cmdqueue {
	struct {
		u8 code;
		u16 length;
		u8 count;
		u8 cmdbuf[0x7ec];
	} __packed req;
	struct {
		u8 pad0[0xc];
		u8 res1, res2;
		u8 pad1[6];
		u8 databuf[0x7f0 - ICC_REPLY_HEADER_SIZE];
	} __packed reply;

	u8 *p;
	struct i2c_cmd_hdr *cmd;
};

struct ps4_bridge {
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_bridge bridge;
	struct i2c_cmdqueue cq;
	struct mutex mutex;

	int mode;
	bool enabled;
	bool enabling;

	void *cached_edid;
	size_t cached_edid_size;
};

/* this should really be taken care of by the connector, but that is currently
 * contained/owned by radeon_connector so just use a global for now */
static struct ps4_bridge *g_bridge;
static bool ps4_bridge_modes_logged;

/* Function prototype declarations, to fix compilation warnings */
void ps4_bridge_mode_set(struct drm_bridge *bridge,
			 const struct drm_display_mode *mode,
			 const struct drm_display_mode *adjusted_mode);

int ps4_bridge_get_modes(struct drm_connector *connector);

enum drm_connector_status ps4_bridge_detect(struct drm_connector *connector,
		bool force);

enum drm_mode_status ps4_bridge_mode_valid(struct drm_connector *connector,
				  const struct drm_display_mode *mode);

int ps4_bridge_register(struct drm_connector *connector,
			     struct drm_encoder *encoder);

static int ps4_bridge_read_edid_block(void *data, u8 *buf,
				       unsigned int block, size_t len);
static int ps4_bridge_read_edid_block_smbus(void *data, u8 *buf,
					     unsigned int block, size_t len);
static int ps4_bridge_read_edid_block_icc(void *data, u8 *buf,
					   unsigned int block, size_t len);


static void cq_init(struct i2c_cmdqueue *q, u8 code)
{
	q->req.code = code;
	q->req.count = 0;
	q->p = q->req.cmdbuf;
	q->cmd = NULL;
}

static int ps4_bridge_read_edid_block(void *data, u8 *buf,
				       unsigned int block, size_t len)
{
	struct i2c_adapter *adapter = data;
	unsigned char start = block * EDID_LENGTH;
	unsigned char segment = block >> 1;
	unsigned char xfers = segment ? 3 : 2;
	int ret, retries = 5;

	do {
		struct i2c_msg msgs[] = {
			{
				.addr = PS4_DDC_SEGMENT_ADDR,
				.flags = 0,
				.len = 1,
				.buf = &segment,
			}, {
				.addr = DDC_ADDR,
				.flags = 0,
				.len = 1,
				.buf = &start,
			}, {
				.addr = DDC_ADDR,
				.flags = I2C_M_RD,
				.len = len,
				.buf = buf,
			}
		};

		ret = i2c_transfer(adapter, &msgs[3 - xfers], xfers);
		if (ret == -ENXIO)
			break;
	} while (ret != xfers && --retries);

	return ret == xfers ? 0 : -1;
}

static int ps4_bridge_read_edid_block_smbus(void *data, u8 *buf,
					     unsigned int block, size_t len)
{
	struct i2c_adapter *adapter = data;
	union i2c_smbus_data smbus;
	unsigned int start = block * EDID_LENGTH;
	size_t i;
	int ret;

	if (block > 1)
		return -1;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -1;

	for (i = 0; i < len; i++) {
		ret = i2c_smbus_xfer(adapter, DDC_ADDR, 0, I2C_SMBUS_READ,
				     (u8)(start + i), I2C_SMBUS_BYTE_DATA, &smbus);
		if (ret < 0)
			return -1;
		buf[i] = smbus.byte;
	}

	return 0;
}

static void cq_cmd(struct i2c_cmdqueue *q, u8 major, u8 minor)
{
	if (!q->cmd || q->cmd->major != major || q->cmd->minor != minor) {
		if (q->cmd)
			q->cmd->length = q->p - (u8 *)q->cmd;
		q->cmd = (struct i2c_cmd_hdr *)q->p;
		q->cmd->major = major;
		q->cmd->minor = minor;
		q->cmd->length = 0;
		q->cmd->count = 1;
		q->req.count += 1;
		q->p += sizeof(*q->cmd);
	} else {
		q->cmd->count += 1;
	}
}

static int cq_exec(struct i2c_cmdqueue *q)
{
	int res;

	if (!q->cmd)
		return 0;

	q->cmd->length = q->p - (u8 *)q->cmd;
	q->req.length = q->p - (u8 *)&q->req;

	res = apcie_icc_cmd(0x10, 0, &q->req, q->req.length,
		      &q->reply, sizeof(q->reply));

	if (res < ICC_REPLY_HEADER_SIZE) {
		DRM_ERROR("icc i2c commandqueue failed: %d\n", res);
		return -EIO;
	}
	if (q->reply.res1 != 0 || q->reply.res2) {
		DRM_ERROR("icc i2c commandqueue failed: %d, %d\n",
			  q->reply.res1, q->reply.res2);
		return -EIO;
	}

	return res;
}

static void cq_read(struct i2c_cmdqueue *q, u16 addr, u8 count)
{
	cq_cmd(q, CMD_READ);
	*q->p++ = count;
	*q->p++ = addr >> 8;
	*q->p++ = addr & 0xff;
	*q->p++ = 0;
}

static void cq_writereg(struct i2c_cmdqueue *q, u16 addr, u8 data)
{
	cq_cmd(q, CMD_WRITE);
	*q->p++ = 1;
	*q->p++ = addr >> 8;
	*q->p++ = addr & 0xff;
	*q->p++ = data;
}

#if 0
static void cq_write(struct i2c_cmdqueue *q, u16 addr, u8 *data, u8 count)
{
	cq_cmd(q, CMD_WRITE);
	*q->p++ = count;
	*q->p++ = addr >> 8;
	*q->p++ = addr & 0xff;
	while (count--)
		*q->p++ = *data++;
}
#endif

static void cq_mask(struct i2c_cmdqueue *q, u16 addr, u8 value, u8 mask)
{
	cq_cmd(q, CMD_MASK);
	*q->p++ = 1;
	*q->p++ = addr >> 8;
	*q->p++ = addr & 0xff;
	*q->p++ = value;
	*q->p++ = mask;
}

#if 0
static void cq_delay(struct i2c_cmdqueue *q, u16 time)
{
	cq_cmd(q, CMD_DELAY);
	*q->p++ = 0;
	*q->p++ = time & 0xff;
	*q->p++ = time>>8;
	*q->p++ = 0;
}
#endif

static void cq_wait_set(struct i2c_cmdqueue *q, u16 addr, u8 mask)
{
	cq_cmd(q, CMD_WAIT_SET);
	*q->p++ = 0;
	*q->p++ = addr >> 8;
	*q->p++ = addr & 0xff;
	*q->p++ = mask;
}

static void cq_wait_clear(struct i2c_cmdqueue *q, u16 addr, u8 mask)
{
	cq_cmd(q, CMD_WAIT_CLEAR);
	*q->p++ = 0;
	*q->p++ = addr >> 8;
	*q->p++ = addr & 0xff;
	*q->p++ = mask;
}

#define EDID_SEQ_CHUNK_SIZE (2 * EDID_LENGTH)
#define EDID_SEQ_READ_ITERS 32
#define EDID_SEQ_READ_COUNT 8
#define EDID_FETCH_ADDR0 0x7c00
#define EDID_FETCH_ADDR1 0x7c80

static int ps4_bridge_read_edid_block_icc(void *data, u8 *buf,
					   unsigned int block, size_t len)
{
	struct ps4_bridge *mn_bridge = data;
	struct i2c_cmdqueue *q = &mn_bridge->cq;
	unsigned int seq = block / 2;
	unsigned int half = block % 2;
	u8 seqbuf[EDID_SEQ_CHUNK_SIZE];
	int ret;
	unsigned int i;

	DRM_INFO("ps4_bridge: icc-edid: requesting block=%u len=%zu (seq=%u half=%u)\n",
		 block, len, seq, half);

	if (len > EDID_LENGTH) {
		DRM_INFO("ps4_bridge: icc-edid: len %zu too large\n", len);
		return -1;
	}

	mutex_lock(&mn_bridge->mutex);

	cq_init(q, 4);
	cq_writereg(q, AKESRST, 0xff);
	cq_writereg(q, EDIDCTRL3, 0x88);
	cq_wait_set(q, AKESTA, 0xa0);
	cq_wait_clear(q, AKESTA, AKESTA_BUSY);
	cq_writereg(q, EDIDCTRL4, 0x0a);
	cq_writereg(q, EDIDCTRL0, seq);
	cq_writereg(q, EDIDCTRL1, 0x00);
	cq_writereg(q, EDIDCTRL2, 0xff);
	cq_writereg(q, EDIDCTRL3, 0x82);
	cq_wait_set(q, AKESTA, 0xa0);
	cq_wait_clear(q, AKESTA, AKESTA_BUSY);
	ret = cq_exec(q);
	DRM_INFO("ps4_bridge: icc-edid: kick seq=%u ret=%d\n", seq, ret);
	if (ret < 0) {
		mutex_unlock(&mn_bridge->mutex);
		return -1;
	}

	for (i = 0; i < EDID_SEQ_READ_ITERS; i++) {
		u16 addr = i == 0 ? EDID_FETCH_ADDR0 : EDID_FETCH_ADDR1;

		cq_init(q, 4);
		cq_read(q, addr, EDID_SEQ_READ_COUNT);
		ret = cq_exec(q);
		if (ret < 0) {
			DRM_INFO("ps4_bridge: icc-edid: read iter=%u addr=0x%04x failed ret=%d res1=%u res2=%u\n",
				 i, addr, ret, q->reply.res1, q->reply.res2);
			mutex_unlock(&mn_bridge->mutex);
			return -1;
		}
		memcpy(seqbuf + i * EDID_SEQ_READ_COUNT, q->reply.databuf,
		       EDID_SEQ_READ_COUNT);
	}

	mutex_unlock(&mn_bridge->mutex);

	memcpy(buf, seqbuf + half * EDID_LENGTH, len);
	print_hex_dump(KERN_INFO, "ps4_bridge: icc-edid: ", DUMP_PREFIX_OFFSET,
		       16, 1, buf, len, false);

	return 0;
}

static inline struct ps4_bridge *
		bridge_to_ps4_bridge(struct drm_bridge *bridge)
{
	return container_of(bridge, struct ps4_bridge, bridge);
}

static void ps4_bridge_clear_global(void *data)
{
	if (g_bridge == data)
		g_bridge = NULL;
}

static void ps4_bridge_retrain_dp(struct ps4_bridge *mn_bridge)
{
	struct drm_connector *connector = mn_bridge->connector;
	struct drm_encoder *encoder = mn_bridge->encoder;
	struct amdgpu_connector *amdgpu_connector;
	struct amdgpu_connector_atom_dig *dig_connector;
	int ret;

	if (!connector || !encoder)
		return;

	amdgpu_connector = to_amdgpu_connector(connector);
	if (!amdgpu_connector->ddc_bus ||
	    !amdgpu_connector->ddc_bus->has_aux)
		return;

	dig_connector = amdgpu_connector->con_priv;
	if (dig_connector)
		dig_connector->dp_sink_type = CONNECTOR_OBJECT_ID_DISPLAYPORT;

	ret = amdgpu_atombios_dp_get_dpcd(amdgpu_connector);
	DRM_DEBUG_KMS("ps4_bridge: post-enable DPCD ret=%d lanes=%u clock=%u sink_type=%d\n",
		      ret,
		      dig_connector ? dig_connector->dp_lane_count : 0,
		      dig_connector ? dig_connector->dp_clock : 0,
		      dig_connector ? dig_connector->dp_sink_type : -1);

	if (ret)
		return;

	DRM_DEBUG_KMS("ps4_bridge: retraining DP link after bridge enable\n");
	amdgpu_atombios_dp_link_train(encoder, connector);
}

void ps4_bridge_mode_set(struct drm_bridge *bridge,
			 const struct drm_display_mode *mode,
			 const struct drm_display_mode *adjusted_mode)
{
	struct ps4_bridge *mn_bridge = bridge_to_ps4_bridge(bridge);

	/* This gets called before pre_enable/enable, so we just stash
	 * the vic ID for later */
	mn_bridge->mode = drm_match_cea_mode(adjusted_mode);
	DRM_DEBUG_KMS("vic mode: %d\n", mn_bridge->mode);
	if (!mn_bridge->mode) {
		DRM_ERROR("attempted to set non-CEA mode\n");
	}
}

static void ps4_bridge_pre_enable(struct drm_bridge *bridge)
{
	struct ps4_bridge *mn_bridge = bridge_to_ps4_bridge(bridge);
	DRM_DEBUG_KMS("ps4_bridge_pre_enable\n");
	DRM_DEBUG("Enable ps4_bridge_pre_enable\n");
	mutex_lock(&mn_bridge->mutex);
	if (mn_bridge->enabled || mn_bridge->enabling) {
		DRM_DEBUG_KMS("ps4_bridge_pre_enable (already enabled or enabling, skip)\n");
		mutex_unlock(&mn_bridge->mutex);
		return;
	}

	cq_init(&mn_bridge->cq, 4);

#if 0
	/* No idea. DP stuff probably. This borks for some reason. Meh. */
	cq_writereg(&mn_bridge->cq, 0x7657,0xff);
	cq_writereg(&mn_bridge->cq, 0x76a5,0x80);
	cq_writereg(&mn_bridge->cq, 0x76a6,0x04);
	cq_writereg(&mn_bridge->cq, 0x7601,0x0a);
	cq_writereg(&mn_bridge->cq, 0x7602,0x84);
	cq_writereg(&mn_bridge->cq, 0x7603,0x00);
	cq_writereg(&mn_bridge->cq, 0x76a8,0x09);
	cq_writereg(&mn_bridge->cq, 0x76ae,0xd1);
	cq_writereg(&mn_bridge->cq, 0x76af,0x50);
	cq_writereg(&mn_bridge->cq, 0x76b0,0x70);
	cq_writereg(&mn_bridge->cq, 0x76b1,0xb0);
	cq_writereg(&mn_bridge->cq, 0x76b2,0xf0);
	cq_writereg(&mn_bridge->cq, 0x76db,0x00);
	cq_writereg(&mn_bridge->cq, 0x76dc,0x64);
	cq_writereg(&mn_bridge->cq, 0x76dd,0x22);
	cq_writereg(&mn_bridge->cq, 0x76e4,0x00);
	cq_writereg(&mn_bridge->cq, 0x76e6,0x1e); /* 0 for (DP?) scramble off */
	cq_writereg(&mn_bridge->cq, 0x7670,0xff);
	cq_writereg(&mn_bridge->cq, 0x7671,0xff);
	cq_writereg(&mn_bridge->cq, 0x7672,0xff);
	cq_writereg(&mn_bridge->cq, 0x7673,0xff);
	cq_writereg(&mn_bridge->cq, 0x7668,0xff);
	cq_writereg(&mn_bridge->cq, 0x7669,0xff);
	cq_writereg(&mn_bridge->cq, 0x766a,0xff);
	cq_writereg(&mn_bridge->cq, 0x766b,0xff);
	cq_writereg(&mn_bridge->cq, 0x7655,0x04);
	cq_writereg(&mn_bridge->cq, 0x7007,0xff);
	cq_writereg(&mn_bridge->cq, 0x7098,0xff);
	cq_writereg(&mn_bridge->cq, 0x7099,0x00);
	cq_writereg(&mn_bridge->cq, 0x709a,0x0f);
	cq_writereg(&mn_bridge->cq, 0x709b,0x00);
	cq_writereg(&mn_bridge->cq, 0x709c,0x50);
	cq_writereg(&mn_bridge->cq, 0x709d,0x00);
	cq_writereg(&mn_bridge->cq, 0x709e,0x00);
	cq_writereg(&mn_bridge->cq, 0x709f,0xd0);
	cq_writereg(&mn_bridge->cq, 0x7a9c,0x2e);
	cq_writereg(&mn_bridge->cq, 0x7021,0x04);
	cq_writereg(&mn_bridge->cq, 0x7028,0x00);
	cq_writereg(&mn_bridge->cq, 0x7030,0xa3);
	cq_writereg(&mn_bridge->cq, 0x7016,0x04);
#endif

	/* Disable InfoFrames */
	cq_writereg(&mn_bridge->cq, INFENA, 0x00);
	/* Reset HDCP */
	cq_writereg(&mn_bridge->cq, TSRST, TSRST_ENCSRST | TSRST_HDCPSRST);
	/* Disable HDCP flag */
	cq_writereg(&mn_bridge->cq, HDCPEN, HDCPEN_ENC_DIS);
	/* HDCP AKE reset */
	cq_writereg(&mn_bridge->cq, AKESRST, 0xff);
	/* Wait AKE busy */
	cq_wait_clear(&mn_bridge->cq, AKESTA, AKESTA_BUSY);

	if (cq_exec(&mn_bridge->cq) < 0) {
		DRM_ERROR("failed to run pre-enable sequence");
	}
	mutex_unlock(&mn_bridge->mutex);
}

static int ps4_bridge_wait_dpcd_ready(struct ps4_bridge *mn_bridge,
				      unsigned long timeout_ms)
{
	struct drm_connector *connector = mn_bridge->connector;
	struct amdgpu_connector *amdgpu_connector;
	struct amdgpu_connector_atom_dig *dig_connector;
	unsigned long deadline;
	int ret;

	if (!connector)
		return -ENODEV;

	amdgpu_connector = to_amdgpu_connector(connector);
	if (!amdgpu_connector->ddc_bus ||
	    !amdgpu_connector->ddc_bus->has_aux)
		return 0;

	dig_connector = amdgpu_connector->con_priv;
	if (dig_connector)
		dig_connector->dp_sink_type = CONNECTOR_OBJECT_ID_DISPLAYPORT;

	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	do {
		ret = amdgpu_atombios_dp_get_dpcd(amdgpu_connector);
		if (ret == 0)
			return 0;
		msleep(20);
	} while (time_before(jiffies, deadline));

	DRM_WARN("ps4_bridge: DPCD not ready after %lu ms\n", timeout_ms);
	return -ETIMEDOUT;
}

static void ps4_bridge_reset_mn864729(struct ps4_bridge *mn_bridge)
{
	mutex_lock(&mn_bridge->mutex);
	cq_init(&mn_bridge->cq, 4);
	cq_writereg(&mn_bridge->cq, TSRST,
		   TSRST_AVCSRST | TSRST_ENCSRST | TSRST_FIFOSRST |
		   TSRST_CCSRST | TSRST_HDCPSRST | TSRST_AUDSRST |
		   TSRST_VIFSRST);
	if (cq_exec(&mn_bridge->cq) < 0)
		DRM_ERROR("Failed to reset bridge between Belize attempts\n");
	mutex_unlock(&mn_bridge->mutex);
}

static int ps4_bridge_enable_mn864729_video(struct ps4_bridge *mn_bridge,
					    struct pci_dev *pdev)
{
	int ret;

	mutex_lock(&mn_bridge->mutex);
	cq_init(&mn_bridge->cq, 4);
	cq_mask(&mn_bridge->cq, 0x6005, 0x01, 0x01);
	cq_writereg(&mn_bridge->cq, 0x6a03, 0x47);

	/* Wait for DP lane status */
	cq_wait_set(&mn_bridge->cq, 0x60f8, 0xff);
	cq_wait_set(&mn_bridge->cq, 0x60f9, 0x01);
	cq_writereg(&mn_bridge->cq, 0x6a01, 0x4d);
	cq_wait_set(&mn_bridge->cq, 0x60f9, 0x1a);

	cq_mask(&mn_bridge->cq, 0x1e00, 0x00, 0x21);
	cq_mask(&mn_bridge->cq, 0x1e02, 0x00, 0x70);
	// 03 08 01 01 00  2c 01 00
	//rancido has no delay here vvv
	//cq_delay(&mn_bridge->cq, 0x012c);
	cq_writereg(&mn_bridge->cq, 0x6020, 0x00);

	//rancido has no delay here vvv
	//cq_delay(&mn_bridge->cq, 0x0032);
	cq_writereg(&mn_bridge->cq, 0x7402, 0x1c);
	cq_writereg(&mn_bridge->cq, 0x6020, 0x04);
	cq_writereg(&mn_bridge->cq, TSYSCTRL, TSYSCTRL_HDMI);
	cq_writereg(&mn_bridge->cq, 0x10c7, 0x38);
	cq_writereg(&mn_bridge->cq, 0x1e02, 0x88);
	cq_writereg(&mn_bridge->cq, 0x1e00, 0x66);
	cq_writereg(&mn_bridge->cq, 0x100c, 0x01);
	cq_writereg(&mn_bridge->cq, TSYSCTRL, TSYSCTRL_HDMI);

	cq_writereg(&mn_bridge->cq, 0x7009, 0x00);
	cq_writereg(&mn_bridge->cq, 0x7040, 0x42);
	cq_writereg(&mn_bridge->cq, 0x7225, 0x28);
	cq_writereg(&mn_bridge->cq, 0x7227, mn_bridge->mode);
	cq_writereg(&mn_bridge->cq, 0x7228, 0x00);
	cq_writereg(&mn_bridge->cq, 0x7070, mn_bridge->mode);
	cq_writereg(&mn_bridge->cq, 0x7071, mn_bridge->mode | 0x80);
	cq_writereg(&mn_bridge->cq, 0x7072, 0x00);
	cq_writereg(&mn_bridge->cq, 0x7073, 0x00);
	cq_writereg(&mn_bridge->cq, 0x7074, 0x00);
	cq_writereg(&mn_bridge->cq, 0x7075, 0x00);
	cq_writereg(&mn_bridge->cq, 0x70c4, 0x0a);
	cq_writereg(&mn_bridge->cq, 0x70c5, 0x0a);
	cq_writereg(&mn_bridge->cq, 0x70c2, 0x00);
	cq_writereg(&mn_bridge->cq, 0x70fe, 0x12);
	cq_writereg(&mn_bridge->cq, 0x70c3, 0x10);

	if (pdev->device == PCI_DEVICE_ID_CUH_12XX) {
		/* newer ps4 phats need here 0x03 idk why. */
		cq_writereg(&mn_bridge->cq, 0x10c5, 0x03);
	} else {
		cq_writereg(&mn_bridge->cq, 0x10c5, 0x00);
	}

	cq_writereg(&mn_bridge->cq, 0x10f6, 0xff);
	cq_writereg(&mn_bridge->cq, 0x7202, 0x20);
	cq_writereg(&mn_bridge->cq, 0x7203, 0x60);
	cq_writereg(&mn_bridge->cq, 0x7011, 0xd5);
	//cq_writereg(&mn_bridge->cq, 0x7a00, 0x0e);

	cq_wait_set(&mn_bridge->cq, 0x10f6, 0x80);
	cq_mask(&mn_bridge->cq, 0x7226, 0x00, 0x80);
	cq_mask(&mn_bridge->cq, 0x7228, 0x00, 0xFF);
	// rancido has no delay here vvv
	//cq_delay(&mn_bridge->cq, 0x012c);
	cq_writereg(&mn_bridge->cq, 0x7204, 0x40);
	cq_wait_clear(&mn_bridge->cq, 0x7204, 0x40);
	cq_writereg(&mn_bridge->cq, 0x7a8b, 0x05);
	cq_mask(&mn_bridge->cq, 0x1e02, 0x70, 0x70);
	cq_mask(&mn_bridge->cq, 0x1034, 0x02, 0x02);
	cq_mask(&mn_bridge->cq, 0x1e00, 0x01, 0x01);
	cq_writereg(&mn_bridge->cq, VMUTECNT, VMUTECNT_LINEWIDTH_90);
	cq_writereg(&mn_bridge->cq, HDCPEN, 0x00);
	ret = cq_exec(&mn_bridge->cq);
	if (ret < 0)
		DRM_ERROR("Failed to configure ps4-bridge (MN864729) mode\n");

	cq_init(&mn_bridge->cq, 1);
	cq_read(&mn_bridge->cq, 0x60f8, 2);
	if (cq_exec(&mn_bridge->cq) >= ICC_REPLY_HEADER_SIZE + 2)
		DRM_DEBUG_KMS("mn864729: 0x60f8=0x%02x 0x60f9=0x%02x\n",
			      mn_bridge->cq.reply.databuf[0],
			      mn_bridge->cq.reply.databuf[1]);
	else
		DRM_DEBUG_KMS("mn864729: 0x60f8/0x60f9 readback failed\n");

	mutex_unlock(&mn_bridge->mutex);

	return ret < 0 ? ret : 0;
}

static void ps4_bridge_enable_mn864729_audio(struct ps4_bridge *mn_bridge)
{
	mutex_lock(&mn_bridge->mutex);

	// AUDIO preinit
	cq_init(&mn_bridge->cq, 4);
	cq_writereg(&mn_bridge->cq,0x70aa, 0x00);
	cq_writereg(&mn_bridge->cq,0x70af, 0x07);
	cq_writereg(&mn_bridge->cq,0x70a9, 0x5a);

	cq_mask(&mn_bridge->cq,0x70af, 0x06, 0x06);
	cq_mask(&mn_bridge->cq,0x70af, 0x02, 0x0f);
	cq_mask(&mn_bridge->cq,0x70b3, 0x02, 0x0f);
	cq_mask(&mn_bridge->cq,0x70ae, 0x80, 0xe0);
	cq_mask(&mn_bridge->cq,0x70ae, 0x01, 0x07);
	cq_mask(&mn_bridge->cq,0x70ac, 0x01, 0x21);
	cq_mask(&mn_bridge->cq,0x70ab, 0x80, 0x88);
	cq_mask(&mn_bridge->cq,0x70a9, 0x01, 0x01);
	if (cq_exec(&mn_bridge->cq) < 0)
		DRM_ERROR("failed to run enable hdmi audio seq. 0");

	cq_init(&mn_bridge->cq, 4);
	cq_writereg(&mn_bridge->cq,0x70b0, 0x01);
	cq_mask(&mn_bridge->cq,0x70b0, 0x00, 0xff);
	cq_mask(&mn_bridge->cq,0x70b1, 0x79, 0xff);
	cq_mask(&mn_bridge->cq,0x70b2, 0x00, 0xff);
	cq_mask(&mn_bridge->cq,0x70b3, 0x02, 0xff);
	cq_mask(&mn_bridge->cq,0x70b4, 0x0b, 0x0f);
	cq_mask(&mn_bridge->cq,0x70b5, 0x00, 0xff);
	cq_mask(&mn_bridge->cq,0x70b6, 0x00, 0xff);
	cq_writereg(&mn_bridge->cq,0x10f6, 0xff);
	cq_writereg(&mn_bridge->cq,0x7011, 0xa2);
	cq_wait_set(&mn_bridge->cq,0x10f6, 0xa2);
	cq_mask(&mn_bridge->cq,0x7267, 0x00, 0xff);
	cq_writereg(&mn_bridge->cq,0x7204, 0x10);
	cq_wait_clear(&mn_bridge->cq,0x7204, 0x10);
	cq_writereg(&mn_bridge->cq,0x10f6, 0xff);
	cq_mask(&mn_bridge->cq,0x7203, 0x10, 0x10);
	cq_writereg(&mn_bridge->cq,0x70a8, 0xc0);
	if (cq_exec(&mn_bridge->cq) < 0)
		DRM_ERROR("failed to run enable hdmi audio seq. 1");

	mutex_unlock(&mn_bridge->mutex);
}

static void ps4_bridge_enable(struct drm_bridge *bridge)
{
	struct ps4_bridge *mn_bridge = bridge_to_ps4_bridge(bridge);
	struct drm_connector *connector = mn_bridge->connector;
	struct drm_device *dev = connector->dev;
	struct pci_dev *pdev = to_pci_dev(dev->dev);
	u8 dp[3];
	bool is_mn864729 = pdev->device != PCI_DEVICE_ID_CUH_11XX;
	bool success = false;
	int ret;
	unsigned int attempt;

	DRM_DEBUG("Enable PS4_BRIDGE_ENABLE\n");
	mutex_lock(&mn_bridge->mutex);
	if (mn_bridge->enabled && !is_mn864729) {
		DRM_DEBUG_KMS("ps4_bridge_enable (already enabled, skip)\n");
		mutex_unlock(&mn_bridge->mutex);
		return;
	}
	mn_bridge->enabling = true;
	mutex_unlock(&mn_bridge->mutex);

	if (!mn_bridge->mode) {
		DRM_ERROR("mode not available\n");
		goto out;
	}

	DRM_DEBUG_KMS("ps4_bridge_enable (mode: %d)\n", mn_bridge->mode);

	/* Here come the dragons */

	if (!is_mn864729)
	{
		/* Panasonic MN86471A */
		mutex_lock(&mn_bridge->mutex);
		cq_init(&mn_bridge->cq, 4);

		/* Read DisplayPort status (?) */
		cq_read(&mn_bridge->cq, 0x76e1, 3);
		if (cq_exec(&mn_bridge->cq) < ICC_REPLY_HEADER_SIZE + 3) {
			mutex_unlock(&mn_bridge->mutex);
			DRM_ERROR("could not read DP status");
			goto out;
		}
		memcpy(dp, &mn_bridge->cq.reply.databuf[0], 3);

		cq_init(&mn_bridge->cq, 4);

		/* Wait for DP lane status */
		cq_wait_set(&mn_bridge->cq, 0x761e, 0x77);
		cq_wait_set(&mn_bridge->cq, 0x761f, 0x77);
		/* Wait for ?? */
		cq_wait_set(&mn_bridge->cq, 0x7669, 0x01);
		cq_writereg(&mn_bridge->cq, 0x76d9, (dp[0] & 0x1f) | (dp[0] << 5));
		cq_writereg(&mn_bridge->cq, 0x76da, (dp[1] & 0x7c) | ((dp[0] >> 3) & 3) | ((dp[1] << 5) & 0x80));
		cq_writereg(&mn_bridge->cq, 0x76db, 0x80 | ((dp[1] >> 3) & 0xf));
		cq_writereg(&mn_bridge->cq, 0x76e4, 0x01);
		cq_writereg(&mn_bridge->cq, TSYSCTRL, TSYSCTRL_HDMI);
		cq_writereg(&mn_bridge->cq, VINCNT, VINCNT_VIF_FILEN);
		cq_writereg(&mn_bridge->cq, 0x7071, 0);
		cq_writereg(&mn_bridge->cq, 0x7062, mn_bridge->mode);
		cq_writereg(&mn_bridge->cq, 0x765a, 0);
		cq_writereg(&mn_bridge->cq, 0x7062, mn_bridge->mode | 0x80);
		cq_writereg(&mn_bridge->cq, 0x7215, 0x28); /* aspect */
		cq_writereg(&mn_bridge->cq, 0x7217, mn_bridge->mode);
		cq_writereg(&mn_bridge->cq, 0x7218, 0);
		cq_writereg(&mn_bridge->cq, CSCMOD, 0xdc);
		cq_writereg(&mn_bridge->cq, C420SET, 0xaa);
		cq_writereg(&mn_bridge->cq, TDPCMODE, 0x4a);
		cq_writereg(&mn_bridge->cq, OUTWSET, 0x00);
		cq_writereg(&mn_bridge->cq, 0x70c4, 0x08);
		cq_writereg(&mn_bridge->cq, 0x70c5, 0x08);
		cq_writereg(&mn_bridge->cq, 0x7096, 0xff);
		cq_writereg(&mn_bridge->cq, 0x7027, 0x00);
		cq_writereg(&mn_bridge->cq, 0x7020, 0x20);
		cq_writereg(&mn_bridge->cq, 0x700b, 0x01);
		cq_writereg(&mn_bridge->cq, PKTENA, 0x20);
		cq_writereg(&mn_bridge->cq, 0x7096, 0xff);
		cq_writereg(&mn_bridge->cq, INFENA, INFENA_AVIEN);
		cq_writereg(&mn_bridge->cq, UPDCTRL, UPDCTRL_ALLUPD | UPDCTRL_AVIIUPD |
						     UPDCTRL_CLKUPD | UPDCTRL_VIFUPD |
						     UPDCTRL_CSCUPD);
		cq_wait_set(&mn_bridge->cq, 0x7096, 0x80);

		cq_mask(&mn_bridge->cq, 0x7216, 0x00, 0x80);
		cq_writereg(&mn_bridge->cq, 0x7218, 0x00);

		cq_writereg(&mn_bridge->cq, 0x7096, 0xff);
		cq_writereg(&mn_bridge->cq, VMUTECNT, VMUTECNT_LINEWIDTH_90 | VMUTECNT_VMUTE_MUTE_NORMAL);
		cq_writereg(&mn_bridge->cq, 0x7016, 0x04);
		cq_writereg(&mn_bridge->cq, 0x7a88, 0xff);
		cq_writereg(&mn_bridge->cq, 0x7a83, 0x88);
		cq_writereg(&mn_bridge->cq, 0x7204, 0x40);

		cq_wait_set(&mn_bridge->cq, 0x7096, 0x80);

		cq_writereg(&mn_bridge->cq, 0x7006, 0x02);
		cq_writereg(&mn_bridge->cq, 0x7020, 0x21);
		cq_writereg(&mn_bridge->cq, 0x7a8b, 0x00);
		cq_writereg(&mn_bridge->cq, 0x7020, 0x21);

		cq_writereg(&mn_bridge->cq, VMUTECNT, VMUTECNT_LINEWIDTH_90);
		ret = cq_exec(&mn_bridge->cq);
		if (ret < 0) {
			DRM_ERROR("Failed to configure ps4-bridge (MN86471A) mode\n");
		} else {
			success = true;
		}
		#if 1
		// preinit
		cq_init(&mn_bridge->cq, 4);
		cq_writereg(&mn_bridge->cq,0x70b3, 0x00);
		cq_writereg(&mn_bridge->cq,0x70b7, 0x0b);
		cq_writereg(&mn_bridge->cq,0x70a8, 0x24);

		cq_mask(&mn_bridge->cq,0x70b9, 0x06, 0x06);
		cq_mask(&mn_bridge->cq,0x70b6, 0x02, 0x0f);
		cq_mask(&mn_bridge->cq,0x70ba, 0x40, 0x70);
		cq_mask(&mn_bridge->cq,0x70b2, 0x20, 0xe0);
		cq_mask(&mn_bridge->cq,0x7257, 0x00, 0xff);
		cq_mask(&mn_bridge->cq,0x70b0, 0x01, 0x21);
		cq_mask(&mn_bridge->cq,0x70ba, 0x00, 0x88);
		cq_mask(&mn_bridge->cq,0x70b9, 0x01, 0x01);
		if (cq_exec(&mn_bridge->cq) < 0) {
			DRM_ERROR("failed to run enable MN86471A hdmi audio seq. 0");
		}

		cq_init(&mn_bridge->cq, 4);
		cq_writereg(&mn_bridge->cq,0x7ed8, 0x01);

		cq_mask(&mn_bridge->cq,0x70b4, 0x00, 0x3e);
		cq_mask(&mn_bridge->cq,0x70b5, 0x79, 0xff);
		cq_mask(&mn_bridge->cq,0x70ab, 0x00, 0xff);
		cq_mask(&mn_bridge->cq,0x70b6, 0x02, 0x3f);
		cq_mask(&mn_bridge->cq,0x70b7, 0x0b, 0x0f);
		cq_mask(&mn_bridge->cq,0x70ac, 0x00, 0xff);
		cq_mask(&mn_bridge->cq,0x70bd, 0x00, 0xff);

		cq_writereg(&mn_bridge->cq, 0x7204, 0x10);
		cq_writereg(&mn_bridge->cq,0x7011, 0xa2);

		cq_wait_set(&mn_bridge->cq,0x7096, 0x80);
		cq_writereg(&mn_bridge->cq,0x7096, 0xff);

		cq_mask(&mn_bridge->cq,0x7203, 0x10, 0x10);
		cq_writereg(&mn_bridge->cq,0x70b1, 0xc0);
		if (cq_exec(&mn_bridge->cq) < 0) {
			DRM_ERROR("failed to run enable hdmi MN86471A audio seq. 1");
		}
		#endif
		mutex_unlock(&mn_bridge->mutex);
	}
	else
	{
		ps4_bridge_wait_dpcd_ready(mn_bridge,
					   PS4_BRIDGE_DPCD_READY_TIMEOUT_MS);

		for (attempt = 1; attempt <= PS4_BRIDGE_BELIZE_ENABLE_ATTEMPTS;
		     attempt++) {
			DRM_DEBUG_KMS("ps4_bridge_enable: Belize attempt %u/%u\n",
				      attempt,
				      PS4_BRIDGE_BELIZE_ENABLE_ATTEMPTS);

			ret = ps4_bridge_enable_mn864729_video(mn_bridge, pdev);
			if (!ret) {
				success = true;
				ps4_bridge_retrain_dp(mn_bridge);
				break;
			}

			if (attempt < PS4_BRIDGE_BELIZE_ENABLE_ATTEMPTS) {
				ps4_bridge_reset_mn864729(mn_bridge);
				msleep(PS4_BRIDGE_BELIZE_RETRY_DELAY_MS);
			}
		}

		if (success)
			ps4_bridge_enable_mn864729_audio(mn_bridge);
	}

out:
	mutex_lock(&mn_bridge->mutex);
	mn_bridge->enabled = success;
	mn_bridge->enabling = false;
	mutex_unlock(&mn_bridge->mutex);

}

static void ps4_bridge_disable(struct drm_bridge *bridge)
{
	struct ps4_bridge *mn_bridge = bridge_to_ps4_bridge(bridge);

	mutex_lock(&mn_bridge->mutex);
	if (!mn_bridge->enabled) {
		mn_bridge->enabled = false;
		mn_bridge->enabling = false;
		DRM_DEBUG_KMS("ps4_bridge_disable (already disabled, skip)\n");
		mutex_unlock(&mn_bridge->mutex);
		return;
	}

	mn_bridge->enabled = false;
	mn_bridge->enabling = false;
	DRM_DEBUG_KMS("ps4_bridge_disable\n");

	cq_init(&mn_bridge->cq, 4);
	cq_writereg(&mn_bridge->cq, VMUTECNT, VMUTECNT_LINEWIDTH_90 | VMUTECNT_VMUTE_MUTE_NORMAL);
	cq_writereg(&mn_bridge->cq, INFENA, 0x00);
	if (cq_exec(&mn_bridge->cq) < 0) {
		DRM_ERROR("Failed to disable bridge\n");
	}
	mutex_unlock(&mn_bridge->mutex);
}

static void ps4_bridge_post_disable(struct drm_bridge *bridge)
{
	struct ps4_bridge *mn_bridge = bridge_to_ps4_bridge(bridge);

	DRM_DEBUG_KMS("ps4_bridge_post_disable\n");

	mutex_lock(&mn_bridge->mutex);
	mn_bridge->enabled = false;
	mn_bridge->enabling = false;
	if (!mn_bridge->mode) {
		mutex_unlock(&mn_bridge->mutex);
		return;
	}

	cq_init(&mn_bridge->cq, 4);
	cq_writereg(&mn_bridge->cq, TSRST,
		   TSRST_AVCSRST | TSRST_ENCSRST | TSRST_FIFOSRST |
		   TSRST_CCSRST | TSRST_HDCPSRST | TSRST_AUDSRST |
		   TSRST_VIFSRST);
	if (cq_exec(&mn_bridge->cq) < 0)
		DRM_ERROR("Failed to reset bridge in post_disable\n");

	mn_bridge->mode = 0;
	mutex_unlock(&mn_bridge->mutex);
}

/* 4 - 1280x720@60Hz */
static const struct drm_display_mode mode_720p = {
	DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
		 1430, 1650, 0, 720, 725, 730, 750, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	.picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9
};
/* 16 - 1920x1080@60Hz */
static const struct drm_display_mode mode_1080p = {
	DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED, 148500, 1920, 2008,
		 2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	.picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9
};

int ps4_bridge_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct amdgpu_connector *amdgpu_connector = to_amdgpu_connector(connector);
	struct amdgpu_connector_atom_dig *dig_connector = amdgpu_connector->con_priv;
	struct drm_encoder *encoder;
	const struct drm_edid *drm_edid = NULL;
	const struct edid *raw_edid;
	struct i2c_adapter *ddc;
	struct drm_display_mode *newmode;
	int dpcd_ret = -ENODEV;
	int count = 0;
	const char *edid_source = NULL;

	DRM_DEBUG_KMS("ps4_bridge_get_modes\n");

	drm_edid_free(amdgpu_connector->edid);
	amdgpu_connector->edid = NULL;
	drm_connector_update_edid_property(connector, NULL);

	{
		const struct firmware *fw = NULL;

		if (request_firmware(&fw, "edid/my_edid.bin",
				      connector->dev->dev) == 0 && fw) {
			drm_info(dev, "ps4_bridge: EDID loaded from firmware "
				 "(edid/my_edid.bin), %zu bytes\n", fw->size);
			drm_edid = drm_edid_alloc(fw->data, fw->size);
			if (drm_edid)
				edid_source = "firmware";
			release_firmware(fw);
		} else {
			DRM_DEBUG_KMS("ps4_bridge_get_modes: no firmware EDID "
				      "(edid/my_edid.bin), falling back to DDC\n");
		}
	}

	if (drm_edid)
		goto edid_ready;

	if (g_bridge) {
		drm_info(dev, "ps4_bridge: trying ICC EDID fetch\n");
		drm_edid = drm_edid_read_custom(connector,
						ps4_bridge_read_edid_block_icc,
						g_bridge);
		if (drm_edid)
			edid_source = "ICC";
		else
			drm_info(dev, "ps4_bridge: ICC EDID fetch failed\n");
	}

	if (drm_edid)
		goto edid_ready;

	if (!amdgpu_connector->ddc_bus) {
		DRM_DEBUG_KMS("ps4_bridge_get_modes: no DDC bus, using fallback modes\n");
		goto fallback_modes;
	}

	if (amdgpu_connector->router.ddc_valid) {
		DRM_DEBUG_KMS("ps4_bridge_get_modes: selecting router DDC port\n");
		amdgpu_i2c_router_select_ddc_port(amdgpu_connector);
	}

	if (dig_connector)
		dig_connector->dp_sink_type = CONNECTOR_OBJECT_ID_DISPLAYPORT;

	if (amdgpu_connector->ddc_bus && amdgpu_connector->ddc_bus->has_aux) {
		dpcd_ret = amdgpu_atombios_dp_get_dpcd(amdgpu_connector);
		DRM_DEBUG_KMS("ps4_bridge_get_modes: DPCD ret=%d sink_type=%d lanes=%u clock=%u\n",
			      dpcd_ret,
			      dig_connector ? dig_connector->dp_sink_type : -1,
			      dig_connector ? dig_connector->dp_lane_count : 0,
			      dig_connector ? dig_connector->dp_clock : 0);
	}

	drm_connector_for_each_possible_encoder(connector, encoder) {
		amdgpu_atombios_encoder_setup_ext_encoder_ddc(encoder);
		break;
	}

	if (amdgpu_connector->ddc_bus->has_aux) {
		ddc = &amdgpu_connector->ddc_bus->aux.ddc;
		DRM_DEBUG_KMS("ps4_bridge_get_modes: trying AUX DDC i2c_id=%d adapter_nr=%d adapter=%s\n",
			      amdgpu_connector->ddc_bus->rec.i2c_id, ddc->nr, ddc->name);
		drm_edid = drm_edid_read_custom(connector,
						ps4_bridge_read_edid_block,
						ddc);
		if (!drm_edid) {
			DRM_DEBUG_KMS("ps4_bridge_get_modes: trying AUX SMBUS DDC adapter_nr=%d adapter=%s\n",
				      ddc->nr, ddc->name);
			drm_edid = drm_edid_read_custom(connector,
							ps4_bridge_read_edid_block_smbus,
							ddc);
			if (drm_edid)
				edid_source = "AUX SMBUS DDC";
		} else {
			edid_source = "AUX DDC";
		}
	}

	if (!drm_edid) {
		ddc = &amdgpu_connector->ddc_bus->adapter;
		DRM_DEBUG_KMS("ps4_bridge_get_modes: trying native DDC i2c_id=%d adapter_nr=%d adapter=%s has_aux=%d router_ddc=%d\n",
			      amdgpu_connector->ddc_bus->rec.i2c_id, ddc->nr, ddc->name,
			      amdgpu_connector->ddc_bus->has_aux,
			      amdgpu_connector->router.ddc_valid);
		drm_edid = drm_edid_read_custom(connector,
						ps4_bridge_read_edid_block,
						ddc);
		if (!drm_edid) {
			DRM_DEBUG_KMS("ps4_bridge_get_modes: trying native SMBUS DDC adapter_nr=%d adapter=%s\n",
				      ddc->nr, ddc->name);
			drm_edid = drm_edid_read_custom(connector,
							ps4_bridge_read_edid_block_smbus,
							ddc);
			if (drm_edid)
				edid_source = "native SMBUS DDC";
		} else {
			edid_source = "native DDC";
		}
	}

	if (drm_edid && g_bridge) {
		const struct edid *raw = drm_edid_raw(drm_edid);
		if (raw) {
			size_t sz = (raw->extensions + 1) * EDID_LENGTH;
			void *copy = kmemdup(raw, sz, GFP_KERNEL);
			if (copy) {
				kfree(g_bridge->cached_edid);
				g_bridge->cached_edid = copy;
				g_bridge->cached_edid_size = sz;
			}
		}
	}

	if (!drm_edid && g_bridge && g_bridge->cached_edid) {
		DRM_DEBUG_KMS("ps4_bridge_get_modes: DDC failed, using cached EDID\n");
		drm_edid = drm_edid_alloc(g_bridge->cached_edid,
					  g_bridge->cached_edid_size);
		if (drm_edid)
			edid_source = "cached";
	}

edid_ready:
	if (drm_edid) {
		struct drm_display_mode *mode;

		raw_edid = drm_edid_raw(drm_edid);

		{
			unsigned int forced = ps4_bridge_quirk_forced_modes(raw_edid);

			if (forced) {
				drm_info(dev, "ps4_bridge: EDID from %s is known-bad, forcing fixed mode set\n",
					 edid_source ? edid_source : "unknown");
				drm_edid_free(drm_edid);

				if (forced & PS4_BRIDGE_QUIRK_MODE_1080P60) {
					newmode = drm_mode_duplicate(dev, &mode_1080p);
					if (newmode) {
						drm_mode_probed_add(connector, newmode);
						count++;
					}
				}
				return count;
			}
		}

		amdgpu_connector->edid = drm_edid_dup(drm_edid);
		drm_edid_connector_update(connector, drm_edid);
		count = drm_edid_connector_add_modes(connector);

		{
			struct drm_display_mode *m, *first_1080p = NULL;

			list_for_each_entry(m, &connector->probed_modes, head) {
				if (m->hdisplay == 1920 &&
				    m->vdisplay == 1080 &&
				    !(m->flags & DRM_MODE_FLAG_INTERLACE) &&
				    drm_mode_vrefresh(m) == 60 &&
				    !first_1080p)
					first_1080p = m;
			}
			if (first_1080p) {
				list_for_each_entry(m, &connector->probed_modes, head)
					m->type &= ~DRM_MODE_TYPE_PREFERRED;
				first_1080p->type |= DRM_MODE_TYPE_PREFERRED;
			}
		}

		drm_info(dev, "ps4_bridge: EDID loaded from %s, %d modes, %u extension block(s)\n",
			 edid_source ? edid_source : "unknown", count,
			 raw_edid ? raw_edid->extensions : 0);
		if (!ps4_bridge_modes_logged) {
			list_for_each_entry(mode, &connector->probed_modes, head)
				drm_info(dev, "ps4_bridge: mode %s %dx%d@%dHz\n",
					 mode->name, mode->hdisplay, mode->vdisplay,
					 drm_mode_vrefresh(mode));
			ps4_bridge_modes_logged = true;
		}
		drm_edid_free(drm_edid);
		return count;
	}

	drm_info(dev, "ps4_bridge: no EDID available, using fallback modes\n");

fallback_modes:
	newmode = drm_mode_duplicate(dev, &mode_1080p);
	if (newmode) {
		drm_mode_probed_add(connector, newmode);
		count++;
	}

	newmode = drm_mode_duplicate(dev, &mode_720p);
	if (newmode) {
		drm_mode_probed_add(connector, newmode);
		count++;
	}

	return count;
}

enum drm_connector_status ps4_bridge_detect(struct drm_connector *connector,
		bool force)
{
	struct ps4_bridge *mn_bridge = g_bridge;
	struct amdgpu_connector *amdgpu_connector = to_amdgpu_connector(connector);
	struct amdgpu_connector_atom_dig *dig_connector = amdgpu_connector->con_priv;
	int dpcd_ret = -ENODEV;

	if (!mn_bridge)
		return connector_status_disconnected;

	if (!force)
		return connector->status;

	if (dig_connector)
		dig_connector->dp_sink_type = CONNECTOR_OBJECT_ID_DISPLAYPORT;

	if (amdgpu_connector->ddc_bus && amdgpu_connector->ddc_bus->has_aux)
		dpcd_ret = amdgpu_atombios_dp_get_dpcd(amdgpu_connector);

	if (dpcd_ret != 0 && dig_connector) {
		dig_connector->dp_sink_type = CONNECTOR_OBJECT_ID_NONE;
		dig_connector->dp_lane_count = 0;
		dig_connector->dp_clock = 0;
	}

	DRM_DEBUG_KMS("ps4_bridge_detect: force=%d dpcd_ret=%d lanes=%u clock=%u\n",
		      force, dpcd_ret,
		      dig_connector ? dig_connector->dp_lane_count : 0,
		      dig_connector ? dig_connector->dp_clock : 0);

	if (!amdgpu_connector->ddc_bus || !amdgpu_connector->ddc_bus->has_aux)
		return connector_status_connected;

	return dpcd_ret == 0 ? connector_status_connected :
	       connector_status_disconnected;
}

enum drm_mode_status ps4_bridge_mode_valid(struct drm_connector *connector,
				  const struct drm_display_mode *mode)
{
	if (!drm_match_cea_mode(mode))
		return MODE_BAD;
	return MODE_OK;
}

static int ps4_bridge_attach(struct drm_bridge *bridge,
							struct drm_encoder *encoder,
			     			enum drm_bridge_attach_flags flags)
{
	/* struct ps4_bridge *mn_bridge = bridge_to_ps4_bridge(bridge); */

	return 0;
}

static struct drm_bridge_funcs ps4_bridge_funcs = {
	.pre_enable = ps4_bridge_pre_enable,
	.enable = ps4_bridge_enable,
	.disable = ps4_bridge_disable,
	.post_disable = ps4_bridge_post_disable,
	.attach = ps4_bridge_attach,
	.mode_set = ps4_bridge_mode_set,
};

int ps4_bridge_register(struct drm_connector *connector,
			     struct drm_encoder *encoder)
{
	struct device *dev = connector->dev->dev;
	int ret;
	struct ps4_bridge *mn_bridge;

	if (g_bridge) {
		if (g_bridge->connector == connector && g_bridge->encoder == encoder)
			return 0;

		DRM_ERROR("PS4 bridge already registered for a different connector\n");
		return -EBUSY;
	}

	mn_bridge = devm_drm_bridge_alloc(dev, struct ps4_bridge, bridge,
					      &ps4_bridge_funcs);
	if (IS_ERR(mn_bridge))
		return PTR_ERR(mn_bridge);

	ret = devm_add_action_or_reset(dev, ps4_bridge_clear_global, mn_bridge);
	if (ret)
		return ret;

	mutex_init(&mn_bridge->mutex);

	mn_bridge->encoder = encoder;
	mn_bridge->connector = connector;
	mn_bridge->bridge.type = DRM_MODE_CONNECTOR_HDMIA;

	ret = devm_drm_bridge_add(dev, &mn_bridge->bridge);
	if (ret)
		return ret;

	ret = drm_bridge_attach(mn_bridge->encoder, &mn_bridge->bridge, NULL, 0);
	if (ret) {
		DRM_ERROR("Failed to initialize bridge with drm\n");
		return ret;
	}

	g_bridge = mn_bridge;

	return 0;
}
