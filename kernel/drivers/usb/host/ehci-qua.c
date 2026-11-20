// SPDX-License-Identifier: GPL-2.0-or-later
/* Qua Station RTD1295 port2 EHCI glue for the EHCI-only boot variant. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <soc/realtek/rtd129x-rbus-guard.h>
#include "ehci.h"

static struct hc_driver qua_ehci_hc_driver;

static void qua_usb2_mode_update(void __iomem *reg, u32 clear, u32 set)
{
	unsigned long flags;

	rtd129x_rbus_guard_lock(&flags);
	writel((readl(reg) & ~clear) | set, reg);
	rtd129x_rbus_guard_unlock(flags);
}

static int qua_usb2_host_mode(struct platform_device *pdev)
{
	void __iomem *wrap = devm_platform_ioremap_resource(pdev, 1);

	if (IS_ERR(wrap))
		return PTR_ERR(wrap);
	/* Stock rtk-hsotg.c switch_to_host_mode(), PHY wrapper 0x98013800. */
	qua_usb2_mode_update(wrap + 0x0c, 0, BIT(0));
	msleep(100);
	qua_usb2_mode_update(wrap + 0x2c, 0, BIT(0));
	msleep(100);
	qua_usb2_mode_update(wrap + 0x40, BIT(0), 0);
	qua_usb2_mode_update(wrap + 0x44, 0x180, 0);
	msleep(100);
	dev_info(&pdev->dev, "USB2 wrapper switched to stock host mode\n");
	return 0;
}

struct qua_ehci {
	struct clk *usb_clk;
	struct reset_control *usb_reset;
	struct reset_control *phy2_reset;
	struct usb_hcd *hcd;
	struct phy *phy;
	struct reset_control *phy0_reset;
	struct reset_control *phy1_reset;
	struct reset_control *u3phy0_reset;
	struct reset_control *u3mdio0_reset;
	void __iomem *crt;
};

static void qua_ehci_power_off(void *data)
{
	struct qua_ehci *qua = data;

	clk_disable_unprepare(qua->usb_clk);
	reset_control_assert(qua->usb_reset);
	reset_control_assert(qua->phy2_reset);
	reset_control_assert(qua->phy0_reset);
	reset_control_assert(qua->phy1_reset);
	reset_control_assert(qua->u3phy0_reset);
	reset_control_assert(qua->u3mdio0_reset);
}

static int qua_ehci_power_on(struct device *dev, struct qua_ehci *qua)
{
	int ret;

	qua->usb_clk = devm_clk_get(dev, "usb");
	if (IS_ERR(qua->usb_clk))
		return PTR_ERR(qua->usb_clk);
	qua->usb_reset = devm_reset_control_get_exclusive(dev, "usb");
	if (IS_ERR(qua->usb_reset))
		return PTR_ERR(qua->usb_reset);
	qua->phy2_reset = devm_reset_control_get_exclusive(dev, "phy2");
	if (IS_ERR(qua->phy2_reset))
		return PTR_ERR(qua->phy2_reset);
	qua->phy0_reset = devm_reset_control_get_exclusive(dev, "phy0");
	if (IS_ERR(qua->phy0_reset))
		return PTR_ERR(qua->phy0_reset);
	qua->phy1_reset = devm_reset_control_get_exclusive(dev, "phy1");
	if (IS_ERR(qua->phy1_reset))
		return PTR_ERR(qua->phy1_reset);
	qua->u3phy0_reset = devm_reset_control_get_exclusive(dev, "u3phy0");
	if (IS_ERR(qua->u3phy0_reset))
		return PTR_ERR(qua->u3phy0_reset);
	qua->u3mdio0_reset = devm_reset_control_get_optional_exclusive(dev, "u3mdio0");
	if (IS_ERR(qua->u3mdio0_reset))
		return PTR_ERR(qua->u3mdio0_reset);
	/* Shared CRT is already owned by reset/MMC providers; map without reserve. */
	qua->crt = devm_ioremap(dev, 0x98000000, 8);
	if (!qua->crt)
		return -ENOMEM;

	/* BSP USB_RST apply sequence, scoped to an EHCI-only boot variant. */
	ret = reset_control_assert(qua->phy2_reset);
	if (ret)
		return ret;
	ret = reset_control_assert(qua->usb_reset);
	if (ret)
		return ret;
	/* BSP RESET_USB APPLY commits PHY0/1/2 in two adjacent CRT writes.
	 * This EHCI-only profile owns all USB PHY resets, so no host is disrupted.
	 */
	{
		unsigned long flags;
		u32 mask = BIT(8) | BIT(9) | BIT(2);

		rtd129x_rbus_guard_lock(&flags);
		writel(readl(qua->crt) & ~mask, qua->crt);
		writel(readl(qua->crt + 4) & ~BIT(3), qua->crt + 4);
		writel(readl(qua->crt) | mask, qua->crt);
		writel(readl(qua->crt + 4) | BIT(3), qua->crt + 4);
		rtd129x_rbus_guard_unlock(flags);
	}
	ret = reset_control_deassert(qua->u3mdio0_reset);
	if (ret)
		return ret;
	usleep_range(2000, 2500);
	ret = clk_prepare_enable(qua->usb_clk);
	if (ret)
		return ret;
	clk_disable_unprepare(qua->usb_clk);
	usleep_range(1000, 1500);
	ret = reset_control_deassert(qua->usb_reset);
	if (ret)
		return ret;
	usleep_range(1000, 1500);
	ret = clk_prepare_enable(qua->usb_clk);
	if (ret)
		return ret;
	usleep_range(2000, 2500);
	if (of_property_read_bool(dev->of_node, "kddi,usb3-fixed-cc1")) {
		void __iomem *typec = devm_ioremap(dev, 0x9801334c, 4);
		unsigned long flags;
		u32 value;

		if (!typec) {
			clk_disable_unprepare(qua->usb_clk);
			return -ENOMEM;
		}
		rtd129x_rbus_guard_lock(&flags);
		value = readl(typec);
		value &= ~(BIT(29) | BIT(28) | BIT(27));
		writel(value | BIT(29), typec);
		rtd129x_rbus_guard_unlock(flags);
	}
	return devm_add_action_or_reset(dev, qua_ehci_power_off, qua);
}

static int qua_ehci_reset(struct usb_hcd *hcd)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	ehci->caps = hcd->regs;
	return ehci_setup(hcd);
}

static const struct ehci_driver_overrides qua_ehci_overrides __initconst = {
	.reset = qua_ehci_reset,
};

static int qua_ehci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct qua_ehci *qua;
	int irq, ret;

	if (usb_disabled())
		return -ENODEV;
	qua = devm_kzalloc(dev, sizeof(*qua), GFP_KERNEL);
	if (!qua)
		return -ENOMEM;
	ret = qua_ehci_power_on(dev, qua);
	if (ret)
		return dev_err_probe(dev, ret, "EHCI-only USB reset/clock setup failed\n");
	ret = of_property_read_bool(dev->of_node, "kddi,usb2-device-mode") ?
		0 : qua_usb2_host_mode(pdev);
	if (ret)
		return dev_err_probe(dev, ret, "USB2 host mode setup failed\n");
	qua->phy = devm_phy_get(dev, "usb2-phy");
	if (IS_ERR(qua->phy))
		return dev_err_probe(dev, PTR_ERR(qua->phy), "missing port2 USB2 PHY\n");
	ret = phy_init(qua->phy);
	if (ret)
		return dev_err_probe(dev, ret, "port2 USB2 PHY init failed\n");
	/* The shared reset/clock owner must remain bound for both DWC3 hosts.
	 * In USB2 device mode, leave the PHY initialized for the UDC without
	 * registering an EHCI HCD or claiming its wrapper registers. */
	if (of_property_read_bool(dev->of_node, "kddi,usb2-device-mode")) {
		platform_set_drvdata(pdev, qua);
		dev_info(dev, "USB2 device mode: shared power retained, EHCI inactive\n");
		return 0;
	}
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	qua->hcd = usb_create_hcd(&qua_ehci_hc_driver, dev, dev_name(dev));
	if (!qua->hcd) {
		ret = -ENOMEM;
		goto err_phy;
	}
	qua->hcd->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(qua->hcd->regs)) {
		ret = PTR_ERR(qua->hcd->regs);
		goto err_hcd;
	}
	qua->hcd->rsrc_start = res->start;
	qua->hcd->rsrc_len = resource_size(res);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_hcd;
	}
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(30));
	if (ret)
		goto err_hcd;
	platform_set_drvdata(pdev, qua);
	ret = usb_add_hcd(qua->hcd, irq, IRQF_SHARED);
	if (!ret)
		return 0;
	platform_set_drvdata(pdev, NULL);
err_hcd:
	usb_put_hcd(qua->hcd);
err_phy:
	phy_exit(qua->phy);
	return ret;
}

static void qua_ehci_remove(struct platform_device *pdev)
{
	struct qua_ehci *qua = platform_get_drvdata(pdev);

	if (qua->hcd) {
		usb_remove_hcd(qua->hcd);
		usb_put_hcd(qua->hcd);
	}
	phy_exit(qua->phy);
}

static const struct of_device_id qua_ehci_match[] = {
	{ .compatible = "kddi,quastation-ehci" },
	{}
};
MODULE_DEVICE_TABLE(of, qua_ehci_match);

static struct platform_driver qua_ehci_driver = {
	.probe = qua_ehci_probe,
	.remove = qua_ehci_remove,
	.driver = { .name = "qua-ehci", .of_match_table = qua_ehci_match },
};

static int __init qua_ehci_init(void)
{
	ehci_init_driver(&qua_ehci_hc_driver, &qua_ehci_overrides);
	return platform_driver_register(&qua_ehci_driver);
}
static void __exit qua_ehci_exit(void)
{
	platform_driver_unregister(&qua_ehci_driver);
}
module_init(qua_ehci_init);
module_exit(qua_ehci_exit);
MODULE_DESCRIPTION("Qua Station RTD1295 port2 EHCI host");
MODULE_LICENSE("GPL");
