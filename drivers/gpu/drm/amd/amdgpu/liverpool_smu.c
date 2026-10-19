// SPDX-License-Identifier: GPL-2.0-only
/*
 * PlayStation 4 SMU mailbox driver
 *
 * Based on the SMU7 mailbox protocol (smu_7_0_1_d.h).
 */

#include <linux/delay.h>

#include "amdgpu.h"
#include "liverpool_smu.h"

#define LIVERPOOL_SMU_MSG_REG		0xc2100000
#define LIVERPOOL_SMU_RESP_REG		0xc2100004
#define LIVERPOOL_SMU_ARG_REG		0xc210003c

#define LIVERPOOL_SMU_RESP_READY	BIT(0)
#define LIVERPOOL_SMU_RESP_DONE		BIT(1)

#define LIVERPOOL_SMU_MSG_MASK		0xfffe0001
#define LIVERPOOL_SMU_MSG_VALID		BIT(0)

#define LIVERPOOL_SMU_ARG_TIMEOUT	(500 << 16)

#define LIVERPOOL_SMU_POLL_US		10
#define LIVERPOOL_SMU_POLL_ITER		100000

static const u8 liverpool_smu_mempstate_msg[] = {
	[0] = 0x58,
	[1] = 0x57,
	[2] = 0x59,
};

int liverpool_smu_read(struct amdgpu_device *adev, u32 reg, u32 *val)
{
	if (!adev->reg.smc.rreg)
		return -EIO;

	*val = RREG32_SMC(reg);
	return 0;
}

int liverpool_smu_write(struct amdgpu_device *adev, u32 reg, u32 val)
{
	if (!adev->reg.smc.wreg)
		return -EIO;

	WREG32_SMC(reg, val);
	return 0;
}

#define LIVERPOOL_SAMU_CMD		0x22070
#define LIVERPOOL_SAMU_ADDR		0x22074
#define LIVERPOOL_SAMU_DATA		0x22078
#define LIVERPOOL_SAMU_CLEAR		0x2207c
#define LIVERPOOL_SAMU_TRIGGER		0x32
#define LIVERPOOL_SAMU_STATUS		0x4a
#define LIVERPOOL_SAMU_OP_WRITE		0xa505

int liverpool_smu_samu_write(struct amdgpu_device *adev, u32 reg, u32 val)
{
	u32 i;

	if (!adev->rmmio)
		return -EIO;

	writel(LIVERPOOL_SAMU_OP_WRITE, adev->rmmio + LIVERPOOL_SAMU_CMD);
	writel(reg, adev->rmmio + LIVERPOOL_SAMU_ADDR);
	writel(val, adev->rmmio + LIVERPOOL_SAMU_DATA);

	WREG32_SMC(LIVERPOOL_SAMU_TRIGGER, 1);

	for (i = 0; i < 100000; i++) {
		if (!(RREG32_SMC(LIVERPOOL_SAMU_STATUS) & 1))
			break;
		udelay(1);
	}
	if (i == 100000)
		return -ETIMEDOUT;

	writel(0, adev->rmmio + LIVERPOOL_SAMU_CLEAR);
	return 0;
}

int liverpool_smu_cmd(struct amdgpu_device *adev, u32 msg, u32 arg)
{
	u32 resp, tmp;
	int i;

	for (i = 0; i < LIVERPOOL_SMU_POLL_ITER; i++) {
		resp = RREG32_SMC(LIVERPOOL_SMU_RESP_REG);
		if (resp & LIVERPOOL_SMU_RESP_DONE)
			break;
		udelay(LIVERPOOL_SMU_POLL_US);
	}
	if (i == LIVERPOOL_SMU_POLL_ITER)
		return -ETIMEDOUT;

	WREG32_SMC(LIVERPOOL_SMU_ARG_REG, LIVERPOOL_SMU_ARG_TIMEOUT | arg);

	tmp = RREG32_SMC(LIVERPOOL_SMU_MSG_REG);
	tmp &= LIVERPOOL_SMU_MSG_MASK;
	tmp |= msg << 1;
	tmp ^= LIVERPOOL_SMU_MSG_VALID;
	WREG32_SMC(LIVERPOOL_SMU_MSG_REG, tmp);

	for (i = 0; i < LIVERPOOL_SMU_POLL_ITER; i++) {
		resp = RREG32_SMC(LIVERPOOL_SMU_RESP_REG);
		if (resp & LIVERPOOL_SMU_RESP_READY)
			break;
		udelay(LIVERPOOL_SMU_POLL_US);
	}
	if (i == LIVERPOOL_SMU_POLL_ITER)
		return -ETIMEDOUT;

	for (i = 0; i < LIVERPOOL_SMU_POLL_ITER; i++) {
		resp = RREG32_SMC(LIVERPOOL_SMU_RESP_REG);
		if (resp & LIVERPOOL_SMU_RESP_DONE)
			break;
		udelay(LIVERPOOL_SMU_POLL_US);
	}
	if (i == LIVERPOOL_SMU_POLL_ITER)
		return -ETIMEDOUT;

	return 0;
}

int liverpool_smu_set_mempstate(struct amdgpu_device *adev, u32 idx)
{
	int r;

	if (idx >= ARRAY_SIZE(liverpool_smu_mempstate_msg))
		return -EINVAL;

	if (idx != 1) {
		r = liverpool_smu_cmd(adev, liverpool_smu_mempstate_msg[1], 0);
		if (r)
			return r;
	}

	return liverpool_smu_cmd(adev, liverpool_smu_mempstate_msg[idx], 0);
}

static ssize_t liverpool_smu_mempstate_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct amdgpu_device *adev =
		drm_to_adev(pci_get_drvdata(to_pci_dev(dev)));
	u32 idx;
	int r;

	r = kstrtou32(buf, 0, &idx);
	if (r)
		return r;

	r = liverpool_smu_set_mempstate(adev, idx);
	if (r)
		return r;

	return count;
}
static DEVICE_ATTR_WO(liverpool_smu_mempstate);

static ssize_t liverpool_smu_cmd_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct amdgpu_device *adev =
		drm_to_adev(pci_get_drvdata(to_pci_dev(dev)));
	u32 msg, arg;
	int r, n;

	n = sscanf(buf, "%u %u", &msg, &arg);
	if (n != 2)
		return -EINVAL;

	r = liverpool_smu_cmd(adev, msg, arg);
	if (r)
		return r;

	return count;
}
static DEVICE_ATTR_WO(liverpool_smu_cmd);

static ssize_t liverpool_smu_reg_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct amdgpu_device *adev =
		drm_to_adev(pci_get_drvdata(to_pci_dev(dev)));
	u32 reg;
	int r;

	r = kstrtou32(buf, 0, &reg);
	if (r)
		return r;

	adev->pm.smu_reg_sel = reg;
	return count;
}

static ssize_t liverpool_smu_reg_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct amdgpu_device *adev =
		drm_to_adev(pci_get_drvdata(to_pci_dev(dev)));
	u32 val;
	int r;

	r = liverpool_smu_read(adev, adev->pm.smu_reg_sel, &val);
	if (r)
		return r;

	return sysfs_emit(buf, "0x%08x\n", val);
}
static DEVICE_ATTR_RW(liverpool_smu_reg);

static struct attribute *liverpool_smu_attrs[] = {
	&dev_attr_liverpool_smu_cmd.attr,
	&dev_attr_liverpool_smu_mempstate.attr,
	&dev_attr_liverpool_smu_reg.attr,
	NULL,
};

static const struct attribute_group liverpool_smu_attr_group = {
	.attrs = liverpool_smu_attrs,
};

int liverpool_smu_sysfs_init(struct amdgpu_device *adev)
{
	return devm_device_add_group(adev->dev, &liverpool_smu_attr_group);
}


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Armandas Kvietkus");
MODULE_DESCRIPTION("PlayStation 4 SMU mailbox driver");
