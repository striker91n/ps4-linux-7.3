// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/err.h>
#include "aeolia.h"
#include "ps4-fan.h"

#define PS4_FAN_CACHE_JIFFIES HZ

struct ps4_fan_priv {
	struct mutex lock;
	long temp_mc;
	long thresh_mc;
	unsigned long temp_updated;
	unsigned long thresh_updated;
	bool temp_valid;
	bool thresh_valid;
};

static bool ps4_fan_cache_valid(unsigned long updated, bool valid)
{
	return valid &&
	       time_is_after_jiffies(updated + PS4_FAN_CACHE_JIFFIES);
}

static int icc_read_apu_temp(long *temp_mc)
{
	u8 reply[PS4_FAN_TEMP_REPLY_LEN];
	int ret;

	memset(reply, 0, sizeof(reply));
	ret = apcie_icc_cmd(PS4_FAN_TEMP_ICC_MAJOR, PS4_FAN_TEMP_ICC_MINOR,
			    NULL, 0, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (reply[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;

	*temp_mc = (long)reply[PS4_FAN_TEMP_BYTE] * 1000L;
	return 0;
}

static int icc_read_fan_threshold(long *thresh_mc)
{
	u8 reply[PS4_FAN_CONFIG_REPLY_LEN];
	int ret;

	memset(reply, 0, sizeof(reply));
	ret = apcie_icc_cmd(PS4_FAN_ICC_MAJOR, PS4_FAN_ICC_MINOR_GET,
			    NULL, 0, reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (reply[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;
	if (reply[PS4_FAN_THRESH_BYTE] < PS4_FAN_THRESH_MIN_C ||
	    reply[PS4_FAN_THRESH_BYTE] > PS4_FAN_THRESH_MAX_C)
		return -ERANGE;

	*thresh_mc = (long)reply[PS4_FAN_THRESH_BYTE] * 1000L;
	return 0;
}

static int icc_write_fan_threshold(long thresh_mc)
{
	u8 config[PS4_FAN_CONFIG_REPLY_LEN];
	u8 reply[0x20];
	u8 thresh_c;
	int ret;

	if (thresh_mc < PS4_FAN_THRESH_MIN_MC ||
	    thresh_mc > PS4_FAN_THRESH_MAX_MC)
		return -EINVAL;

	thresh_c = (u8)(thresh_mc / 1000L);

	memset(config, 0, sizeof(config));
	ret = apcie_icc_cmd(PS4_FAN_ICC_MAJOR, PS4_FAN_ICC_MINOR_GET,
			    NULL, 0, config, sizeof(config));
	if (ret < 0)
		return ret;
	if (config[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;

	config[PS4_FAN_THRESH_BYTE] = thresh_c;

	memset(reply, 0, sizeof(reply));
	ret = apcie_icc_cmd(PS4_FAN_ICC_MAJOR, PS4_FAN_ICC_MINOR_SET,
			    config, PS4_FAN_CONFIG_LEN,
			    reply, sizeof(reply));
	if (ret < 0)
		return ret;
	if (reply[PS4_ICC_STATUS_BYTE] != 0x00)
		return -EIO;

	return 0;
}

static umode_t ps4_fan_is_visible(const void *drvdata,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	if (type != hwmon_temp || channel != 0)
		return 0;

	switch (attr) {
	case hwmon_temp_input: return 0444;
	case hwmon_temp_crit:  return 0644;
	default:               return 0;
	}
}

static int ps4_fan_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct ps4_fan_priv *priv = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_temp || channel != 0)
		return -EOPNOTSUPP;

	mutex_lock(&priv->lock);

	switch (attr) {
	case hwmon_temp_input:
		if (ps4_fan_cache_valid(priv->temp_updated, priv->temp_valid)) {
			*val = priv->temp_mc;
			ret = 0;
			break;
		}
		ret = icc_read_apu_temp(val);
		if (!ret) {
			priv->temp_mc = *val;
			priv->temp_updated = jiffies;
			priv->temp_valid = true;
		}
		break;
	case hwmon_temp_crit:
		if (ps4_fan_cache_valid(priv->thresh_updated, priv->thresh_valid)) {
			*val = priv->thresh_mc;
			ret = 0;
			break;
		}
		ret = icc_read_fan_threshold(val);
		if (!ret) {
			priv->thresh_mc = *val;
			priv->thresh_updated = jiffies;
			priv->thresh_valid = true;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	mutex_unlock(&priv->lock);
	return ret;
}

static int ps4_fan_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct ps4_fan_priv *priv = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_temp || channel != 0 || attr != hwmon_temp_crit)
		return -EOPNOTSUPP;
	if (val % 1000)
		return -EINVAL;

	mutex_lock(&priv->lock);
	if (ps4_fan_cache_valid(priv->thresh_updated, priv->thresh_valid) &&
	    priv->thresh_mc == val) {
		mutex_unlock(&priv->lock);
		return 0;
	}

	ret = icc_write_fan_threshold(val);
	if (!ret) {
		priv->thresh_mc = val;
		priv->thresh_updated = jiffies;
		priv->thresh_valid = true;
	}
	mutex_unlock(&priv->lock);

	return ret;
}

static const struct hwmon_channel_info * const ps4_fan_channel_info[] = {
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_CRIT),
	NULL,
};

static const struct hwmon_ops ps4_fan_hwmon_ops = {
	.is_visible = ps4_fan_is_visible,
	.read       = ps4_fan_read,
	.write      = ps4_fan_write,
};

static const struct hwmon_chip_info ps4_fan_chip_info = {
	.ops  = &ps4_fan_hwmon_ops,
	.info = ps4_fan_channel_info,
};

static int ps4_fan_probe(struct platform_device *pdev)
{
	struct ps4_fan_priv *priv;
	struct device *hwmon_dev;
	long temp_mc = 0, thresh_mc = 0;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	hwmon_dev = devm_hwmon_device_register_with_info(
			&pdev->dev, "ps4_fan", priv,
			&ps4_fan_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	mutex_lock(&priv->lock);

	ret = icc_write_fan_threshold(PS4_FAN_THRESH_DEFAULT_MC);
	if (ret)
		dev_warn(&pdev->dev, "failed to set fan threshold: %d\n", ret);

	icc_read_apu_temp(&temp_mc);
	icc_read_fan_threshold(&thresh_mc);

	mutex_unlock(&priv->lock);

	dev_info(&pdev->dev,
		 "PS4 fan hwmon ready: temp=%ldC threshold=%ldC\n",
		 temp_mc / 1000, thresh_mc / 1000);
	return 0;
}

static void ps4_fan_remove(struct platform_device *pdev) {}

static struct platform_driver ps4_fan_driver = {
	.probe  = ps4_fan_probe,
	.remove = ps4_fan_remove,
	.driver = { .name = "ps4-fan" },
};

static struct platform_device *ps4_fan_pdev;

static int __init ps4_fan_init(void)
{
	int ret;

	ret = platform_driver_register(&ps4_fan_driver);
	if (ret) {
		pr_err("ps4-fan: failed to register driver: %d\n", ret);
		return ret;
	}

	ps4_fan_pdev = platform_device_register_simple("ps4-fan", -1, NULL, 0);
	if (IS_ERR(ps4_fan_pdev)) {
		ret = PTR_ERR(ps4_fan_pdev);
		pr_err("ps4-fan: failed to register device: %d\n", ret);
		platform_driver_unregister(&ps4_fan_driver);
		ps4_fan_pdev = NULL;
		return ret;
	}

	return 0;
}

static void __exit ps4_fan_exit(void)
{
	if (ps4_fan_pdev)
		platform_device_unregister(ps4_fan_pdev);
	platform_driver_unregister(&ps4_fan_driver);
}

module_init(ps4_fan_init);
module_exit(ps4_fan_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PS4 Aeolia/Belize fan threshold hwmon driver");
MODULE_ALIAS("platform:ps4-fan");
