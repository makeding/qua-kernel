// SPDX-License-Identifier: GPL-2.0-or-later
/* RTD1295 gates needed by the Qua Station maintenance kernel. */
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
static DEFINE_SPINLOCK(qua_gate_lock);
static int qua_clocks_probe(struct platform_device *pdev)
{
 static const char * const names[] = { "qua-pcie0", "qua-pcie1", "qua-emmc", "qua-emmc-ip", "qua-usb", "qua-gpu" };
 static const u8 offsets[] = { 0, 4, 0, 0, 0, 0 }, bits[] = { 1, 5, 24, 28, 4, 11 };
 struct device *dev = &pdev->dev; struct clk_hw_onecell_data *d;
 void __iomem *base; unsigned int i;
 base = devm_platform_ioremap_resource(pdev, 0);
 if (IS_ERR(base)) return PTR_ERR(base);
 d = devm_kzalloc(dev, struct_size(d, hws, ARRAY_SIZE(names)), GFP_KERNEL);
 if (!d) return -ENOMEM;
 d->num = ARRAY_SIZE(names);
 for (i = 0; i < d->num; i++) {
  d->hws[i] = devm_clk_hw_register_gate(dev, names[i], of_clk_get_parent_name(dev->of_node, i == 3 ? 1 : (i == 5 ? 2 : 0)), 0, base + offsets[i], bits[i], 0, &qua_gate_lock);
  if (IS_ERR(d->hws[i])) return PTR_ERR(d->hws[i]);
 }
 return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, d);
}
static const struct of_device_id qua_clock_match[] = { { .compatible="kddi,quastation-gates" }, {} };
static struct platform_driver qua_clock_driver = { .probe=qua_clocks_probe, .driver={ .name="qua-gates", .of_match_table=qua_clock_match } };
builtin_platform_driver(qua_clock_driver);
