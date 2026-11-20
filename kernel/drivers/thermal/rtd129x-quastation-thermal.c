// SPDX-License-Identifier: GPL-2.0-or-later
/* Native RTD1295 CPU thermal sensor, adapted from the Realtek 4.9 BSP. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>

#define RTD129X_TM_CTRL2	0x08
#define RTD129X_TM_STATUS1	0x18
#define RTD129X_TM_STATUS2	0x1c

#define RTD129X_TM_POWER_DOWN	0x01904001
#define RTD129X_TM_POWER_UP	0x01924001

struct rtd129x_thermal {
	void __iomem *base;
	struct thermal_zone_device *zone;
};

static void rtd129x_thermal_reset(struct rtd129x_thermal *sensor)
{
	writel(RTD129X_TM_POWER_DOWN, sensor->base + RTD129X_TM_CTRL2);
	writel(RTD129X_TM_POWER_UP, sensor->base + RTD129X_TM_CTRL2);
	usleep_range(25000, 26000);
}

static int rtd129x_raw_temperature(struct rtd129x_thermal *sensor)
{
	u32 raw = readl(sensor->base + RTD129X_TM_STATUS1);

	return sign_extend32(raw, 18) * 1000 / 1024;
}

static bool rtd129x_thermal_invalid(struct rtd129x_thermal *sensor, int temp)
{
	u32 status = readl(sensor->base + RTD129X_TM_STATUS2);

	return status == 0 || status == 0x3fffff ||
	       temp < -30000 || temp > 150000;
}

static int rtd129x_thermal_get_temp(struct thermal_zone_device *zone,
				    int *temperature)
{
	struct rtd129x_thermal *sensor = thermal_zone_device_priv(zone);
	int temp;

	writel(RTD129X_TM_POWER_UP, sensor->base + RTD129X_TM_CTRL2);
	usleep_range(5000, 6000);
	temp = rtd129x_raw_temperature(sensor);
	if (rtd129x_thermal_invalid(sensor, temp)) {
		rtd129x_thermal_reset(sensor);
		temp = rtd129x_raw_temperature(sensor);
		if (rtd129x_thermal_invalid(sensor, temp))
			return -EIO;
	}

	*temperature = temp;
	return 0;
}

static const struct thermal_zone_device_ops rtd129x_thermal_ops = {
	.get_temp = rtd129x_thermal_get_temp,
};

static int rtd129x_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd129x_thermal *sensor;
	int temperature;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sensor->base))
		return PTR_ERR(sensor->base);

	rtd129x_thermal_reset(sensor);
	sensor->zone = devm_thermal_of_zone_register(dev, 0, sensor,
						     &rtd129x_thermal_ops);
	if (IS_ERR(sensor->zone))
		return dev_err_probe(dev, PTR_ERR(sensor->zone),
				     "failed to register thermal sensor\n");

	ret = rtd129x_thermal_get_temp(sensor->zone, &temperature);
	if (ret)
		return dev_err_probe(dev, ret, "invalid initial temperature\n");
	dev_info(dev, "CPU temperature %d milli-Celsius\n", temperature);
	return 0;
}

static const struct of_device_id rtd129x_thermal_of_match[] = {
	{ .compatible = "kddi,quastation-rtd1295-thermal" },
	{}
};
MODULE_DEVICE_TABLE(of, rtd129x_thermal_of_match);

static struct platform_driver rtd129x_thermal_driver = {
	.probe = rtd129x_thermal_probe,
	.driver = {
		.name = "rtd129x-quastation-thermal",
		.of_match_table = rtd129x_thermal_of_match,
	},
};
module_platform_driver(rtd129x_thermal_driver);

MODULE_DESCRIPTION("Qua Station RTD1295 CPU thermal sensor");
MODULE_LICENSE("GPL");
