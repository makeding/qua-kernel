// SPDX-License-Identifier: GPL-2.0-or-later
/* Experimental RTD1295 GPU platform initialization for Qua Station. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

struct qua_gpu {
	struct clk *clock;
	struct reset_control *reset;
};

static int qua_gpu_check_supply(struct device *dev)
{
	struct i2c_adapter *adapter;
	struct i2c_client *pmic;
	int enabled, selector, attempt;

	adapter = i2c_get_adapter(0);
	if (!adapter)
		return dev_err_probe(dev, -EPROBE_DEFER, "waiting for PMIC I2C0\n");
	pmic = i2c_new_dummy_device(adapter, 0x12);
	i2c_put_adapter(adapter);
	if (IS_ERR(pmic))
		return dev_err_probe(dev, PTR_ERR(pmic), "PMIC unavailable\n");
	for (attempt = 0; attempt < 4; attempt++) {
		enabled = i2c_smbus_read_byte_data(pmic, 0x05);
		selector = enabled < 0 ? enabled : i2c_smbus_read_byte_data(pmic, 0x11);
		if (enabled >= 0 && selector >= 0)
			break;
		if ((enabled < 0 ? enabled : selector) != -EPROTO)
			break;
		msleep(50);
	}
	i2c_unregister_device(pmic);
	if (enabled < 0 || selector < 0)
		return dev_err_probe(dev, enabled < 0 ? enabled : selector,
				     "PMIC read failed after retry\n");
	selector &= 0x1f;
	dev_info(dev, "PMIC DCDC3 enabled=%u voltage=%u uV\n",
		 !!(enabled & BIT(5)), 800000 + selector * 12500);
	/* Stock DT's 300 MHz OPP specifies 900 mV. Never alter the PMIC. */
	if (!(enabled & BIT(5)) || selector < 8)
		return dev_err_probe(dev, -EINVAL, "GPU supply below 900 mV or off\n");
	return 0;
}

static int qua_gpu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qua_gpu *gpu;
	struct reset_control *reset;
	struct clk *clock;
	struct resource *res;
	void __iomem *crt, *l2;
	u32 status;
	int ret, attempt;

	ret = qua_gpu_check_supply(dev);
	if (ret)
		return ret;
	gpu = devm_kzalloc(dev, sizeof(*gpu), GFP_KERNEL);
	if (!gpu)
		return -ENOMEM;
	/* CRT is shared with reset/clock controllers; L2 is in GPU MMIO. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	crt = devm_ioremap(dev, res->start, resource_size(res));
	if (!crt)
		return -ENOMEM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res)
		return -EINVAL;
	l2 = devm_ioremap(dev, res->start, resource_size(res));
	if (!l2)
		return -ENOMEM;
	clock = devm_clk_get(dev, NULL);
	if (IS_ERR(clock))
		return dev_err_probe(dev, PTR_ERR(clock), "GPU clock\n");
	reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "GPU reset\n");

	/* Vendor resume PLL sequence, 300 MHz. No voltage change or OPP. */
	writel(0x40e3, crt + 0x5a4);
	writel(0x7, crt + 0x1c4);
	writel(0xd, crt + 0x5a0);
	usleep_range(800, 1000);
	writel(0x3, crt + 0x1c4);
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;
	ret = clk_prepare_enable(clock);
	if (ret)
		goto assert_reset;

	/* BISR may not complete on the first warm-reset initialization. */
	for (attempt = 0; attempt < 4; attempt++) {
		writel(0x20, l2 + 0x00);
		writel(0x1, l2 + 0x9c);
		writel(0x3, l2 + 0x9c);
		usleep_range(800, 1000);
		ret = readl_poll_timeout(l2 + 0x20, status, status & 0x20,
					 100, 50000);
		dev_info(dev, "L2 BISR=%08x check=%08x attempt=%d\n",
			 status, readl(l2 + 0x44), attempt + 1);
		if (!ret)
			break;
		if (attempt == 3)
			goto disable_clock;
		clk_disable_unprepare(clock);
		reset_control_assert(reset);
		usleep_range(1000, 1500);
		ret = reset_control_deassert(reset);
		if (ret)
			goto assert_reset;
		ret = clk_prepare_enable(clock);
		if (ret)
			goto assert_reset;
	}
	writel(0xf, l2 + 0x9c);
	writel(0, l2 + 0x00);

	/* Vendor pctrl_gpu on, followed by L2 SD/DS enable callback. */
	writel(0xf00, crt + 0x3a4);
	writel(0x300, crt + 0x3a4);
	udelay(10);
	status = readl(crt + 0x3ac);
	dev_info(dev, "GPU power switch status=%08x\n", status);
	writel(0x4, crt + 0x3ac);

	ret = devm_of_platform_populate(dev);
	if (ret)
		goto disable_clock;
	gpu->clock = clock;
	gpu->reset = reset;
	platform_set_drvdata(pdev, gpu);
	return 0;

disable_clock:
	clk_disable_unprepare(clock);
assert_reset:
	reset_control_assert(reset);
	return ret;
}

static void qua_gpu_remove(struct platform_device *pdev)
{
	struct qua_gpu *gpu = platform_get_drvdata(pdev);

	devm_of_platform_depopulate(&pdev->dev);
	clk_disable_unprepare(gpu->clock);
	reset_control_assert(gpu->reset);
}

static const struct of_device_id qua_gpu_of_match[] = {
	{ .compatible = "kddi,quastation-gpu-init" },
	{ }
};
MODULE_DEVICE_TABLE(of, qua_gpu_of_match);

static struct platform_driver qua_gpu_driver = {
	.probe = qua_gpu_probe,
	.remove = qua_gpu_remove,
	.driver = {
		.name = "qua-gpu-init",
		.of_match_table = qua_gpu_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(qua_gpu_driver);
MODULE_DESCRIPTION("Qua Station experimental GPU initialization");
MODULE_LICENSE("GPL");
