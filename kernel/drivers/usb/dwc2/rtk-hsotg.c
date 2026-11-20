// SPDX-License-Identifier: GPL-2.0
/*
 * Qua external USB2 UDC glue.
 *
 * The stock DT exposes Realtek's DWC2-derived device controller at
 * 0x981e0000 and its PHY control window at 0x98013800.  The vendor UDC
 * forces peripheral mode and programs the four PHY registers below.  This
 * glue retains exactly that board-specific handoff, then creates one
 * upstream DWC2 peripheral child.  It never enables host mode or VBUS.
 *
 * EHCI/OHCI use the same PHY2 and IRQ; their DT nodes must be disabled before
 * this module is loaded.  The computer supplies VBUS to this device port.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include "rtk-hsotg.h"

struct qua_hsotg {
	void __iomem *core;
	void __iomem *phy;
	struct platform_device *dwc2;
	struct device_link *owner_link;
};

static const struct property_entry qua_dwc2_peripheral_properties[] = {
	PROPERTY_ENTRY_STRING("dr_mode", "peripheral"),
	PROPERTY_ENTRY_BOOL("kddi,quastation-pio"),
	{ }
};
static const struct property_entry qua_dwc2_role_properties[] = {
	/* The generic DWC2 core provides the mutually exclusive host and
	 * peripheral protocols. The EHCI wrapper remains only the shared owner. */
	PROPERTY_ENTRY_STRING("dr_mode", "otg"),
	PROPERTY_ENTRY_BOOL("usb-role-switch"),
	PROPERTY_ENTRY_STRING("role-switch-default-mode", "peripheral"),
	PROPERTY_ENTRY_STRING("kddi,role-switch-name", "qua-usb2"),
	PROPERTY_ENTRY_BOOL("kddi,quastation-pio"),
	{ }
};

static void qua_hsotg_prepare_phy(struct qua_hsotg *qua)
{
	u32 value;

	/* Exact PIO vendor UDC PHY sequencing.  Do not add the host power path. */
	value = readl(qua->phy + QUA_PHY_CTRL0 - QUA_PHY_CTRL0);
	writel(value | BIT(0), qua->phy + QUA_PHY_CTRL0 - QUA_PHY_CTRL0);
	msleep(100);
	value = readl(qua->phy + QUA_PHY_CTRL1 - QUA_PHY_CTRL0);
	writel(value | BIT(0), qua->phy + QUA_PHY_CTRL1 - QUA_PHY_CTRL0);
	msleep(100);
	value = readl(qua->phy + QUA_PHY_CTRL2 - QUA_PHY_CTRL0);
	writel(value | BIT(0), qua->phy + QUA_PHY_CTRL2 - QUA_PHY_CTRL0);
	value = readl(qua->phy + QUA_PHY_CTRL3 - QUA_PHY_CTRL0);
	writel(value | 0x180, qua->phy + QUA_PHY_CTRL3 - QUA_PHY_CTRL0);
	msleep(100);

	value = readl(qua->core + QUA_DWC2_GUSBCFG);
	value &= ~QUA_DWC2_GUSBCFG_FORCEHOSTMODE;
	value |= QUA_DWC2_GUSBCFG_PHYIF16 | QUA_DWC2_GUSBCFG_FORCEDEVMODE;
	writel(value, qua->core + QUA_DWC2_GUSBCFG);
	readl(qua->core + QUA_DWC2_GUSBCFG);
}

static int qua_hsotg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qua_hsotg *qua;
	struct resource child_res[2];
	struct resource *core_res;
	struct resource *phy_res;
	struct device_node *owner_np;
	struct platform_device *owner;
	struct platform_device_info info = { };
	int irq;

	if (!of_machine_is_compatible("kddi,quastation"))
		return -ENODEV;

	/* EHCI owns the shared reset/clock/PHY lifecycle.  Device mode must
	 * only consume the already-bound owner; never unbind or reset it here. */
	owner_np = of_parse_phandle(dev->of_node, "kddi,shared-usb-owner", 0);
	if (!owner_np)
		return -EPROBE_DEFER;
	if (!of_property_read_bool(owner_np, "kddi,usb2-device-mode")) {
		of_node_put(owner_np);
		return -ENODEV;
	}
	owner = of_find_device_by_node(owner_np);
	of_node_put(owner_np);
	if (!owner)
		return -EPROBE_DEFER;
	if (!device_is_bound(&owner->dev) || !platform_get_drvdata(owner)) {
		put_device(&owner->dev);
		return -EPROBE_DEFER;
	}

	core_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	phy_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	irq = platform_get_irq(pdev, 0);
	if (!core_res || !phy_res || irq < 0)
		return -EINVAL;

	qua = devm_kzalloc(dev, sizeof(*qua), GFP_KERNEL);
	if (!qua)
		return -ENOMEM;

	/* Do not reserve the windows here: the DWC2 child owns resource 0. */
	qua->core = devm_ioremap(dev, core_res->start, resource_size(core_res));
	qua->phy = devm_ioremap(dev, phy_res->start + QUA_PHY_CTRL0,
				  QUA_PHY_CTRL3 + sizeof(u32) - QUA_PHY_CTRL0);
	if (!qua->core || !qua->phy)
		return -ENOMEM;
	qua->owner_link = device_link_add(dev, &owner->dev,
					 DL_FLAG_AUTOREMOVE_CONSUMER);
	put_device(&owner->dev);
	if (!qua->owner_link)
		return -ENOMEM;

	qua_hsotg_prepare_phy(qua);

	child_res[0] = (struct resource)DEFINE_RES_MEM(core_res->start,
							resource_size(core_res));
	child_res[1] = (struct resource)DEFINE_RES_IRQ(irq);
	info.parent = dev;
	info.name = "dwc2";
	info.id = PLATFORM_DEVID_AUTO;
	info.res = child_res;
	info.num_res = ARRAY_SIZE(child_res);
	info.properties = of_property_read_bool(dev->of_node, "kddi,usb2-role-switch") ?
		qua_dwc2_role_properties : qua_dwc2_peripheral_properties;
	qua->dwc2 = platform_device_register_full(&info);
	if (IS_ERR(qua->dwc2))
		return PTR_ERR(qua->dwc2);

	platform_set_drvdata(pdev, qua);
	dev_info(dev, "external USB2 prepared for DWC2 peripheral mode, irq %d\n",
		 irq);
	return 0;
}

static void qua_hsotg_remove(struct platform_device *pdev)
{
	struct qua_hsotg *qua = platform_get_drvdata(pdev);

	if (qua && !IS_ERR_OR_NULL(qua->dwc2))
		platform_device_unregister(qua->dwc2);
}

static const struct of_device_id qua_hsotg_of_match[] = {
	{ .compatible = "Realtek,rtd129x-usb2-udc" },
	{ }
};
MODULE_DEVICE_TABLE(of, qua_hsotg_of_match);

static struct platform_driver qua_hsotg_driver = {
	.probe = qua_hsotg_probe,
	.remove = qua_hsotg_remove,
	.driver = {
		.name = "qua-rtk-hsotg",
		.of_match_table = qua_hsotg_of_match,
	},
};
module_platform_driver(qua_hsotg_driver);

MODULE_DESCRIPTION("Qua Station external USB2 PIO gadget glue");
MODULE_LICENSE("GPL");
