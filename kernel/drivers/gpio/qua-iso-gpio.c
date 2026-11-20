// SPDX-License-Identifier: GPL-2.0
/* Publish the existing disabled ISO GPIO node without its shared IRQ window. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>

static struct platform_device *iso;
static const struct property_entry props[] = {
	PROPERTY_ENTRY_BOOL("realtek,gpio-only"),
	{}
};
static const struct resource resources[] = {
	DEFINE_RES_MEM(0x98007100, 0x100),
};

static int __init qua_iso_init(void)
{
	struct device_node *np;
	struct platform_device *existing;
	int ret;

	if (!of_machine_is_compatible("kddi,quastation"))
		return -ENODEV;
	np = of_find_compatible_node(NULL, NULL, "realtek,rtd1295-iso-gpio");
	if (!np)
		return -ENODEV;
	existing = of_find_device_by_node(np);
	if (existing) {
		put_device(&existing->dev);
		of_node_put(np);
		return -EBUSY;
	}
	iso = platform_device_alloc("qua-iso-gpio", PLATFORM_DEVID_NONE);
	if (!iso) {
		of_node_put(np);
		return -ENOMEM;
	}
	device_set_node(&iso->dev, of_fwnode_handle(np));
	ret = platform_device_add_resources(iso, resources, ARRAY_SIZE(resources));
	if (ret)
		goto fail;
	ret = device_create_managed_software_node(&iso->dev, props, NULL);
	if (ret)
		goto fail;
	ret = platform_device_add(iso);
	if (ret)
		goto fail;
	if (!iso->dev.driver) {
		platform_device_unregister(iso);
		iso = NULL;
		return -ENODEV;
	}
	pr_info("qua-iso-gpio: GPIO-only controller bound, IRQ window untouched\n");
	return 0;
fail:
	platform_device_put(iso);
	iso = NULL;
	return ret;
}

static void __exit qua_iso_exit(void)
{
	platform_device_unregister(iso);
}
module_init(qua_iso_init);
module_exit(qua_iso_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qua Station ISO GPIO-only platform registration");
