// SPDX-License-Identifier: GPL-2.0-or-later
/* Qua Station RTD1295 legacy EHCI (port2) USB2 PHY. */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <soc/realtek/rtd129x-rbus-guard.h>

#define QUA_PHY_BUSY		BIT(17)
#define QUA_PHY_PORT		BIT(13)
#define QUA_PHY_LOAD		BIT(12)
#define QUA_PHY_CTRL_SHIFT	8

struct qua_rle0599 {
	void __iomem *vstatus;
	void __iomem *insn;
	struct phy *phy;
	struct mutex lock;
};

/* Local working BPi 4.9 Qua port2 table, rtd-129x-usb.dtsi. */
static const u8 qua_page0_addr[] = { 0xe0, 0xe1, 0xe2, 0xe3, 0xe4,
	0xe5, 0xe6, 0xe7, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7 };
static const u8 qua_page0_data[] = { 0x90, 0x30, 0x3a, 0x8d, 0xa9,
	0x65, 0x91, 0x81, 0xfc, 0x8c, 0x00, 0x11, 0x9b, 0x81, 0x00, 0x02 };
static const u8 qua_page1_addr[] = { 0xe0, 0xe1, 0xe2, 0xe3,
					     0xe4, 0xe5, 0xe6, 0xe7 };
static const u8 qua_page1_data[] = { 0x25, 0xef, 0x60, 0x00,
					     0x00, 0x0f, 0x18, 0xe3 };

static int qua_rle_wait(struct qua_rle0599 *rle)
{
	u32 value;

	return readl_poll_timeout(rle->insn, value, !(value & QUA_PHY_BUSY),
				 100, 50000);
}

static int qua_rle_write(struct qua_rle0599 *rle, u8 addr, u8 data)
{
	u32 value;
	int ret;

	/* The legacy wrapper consumes data from VStatusOut2, then two nibbles. */
	/* VStatusOut2 is in the RTD1295 RBUS 0x800-0x9ff erratum window.
	 * Do not hold this lock across the PHY busy polling below. */
	{
		unsigned long flags;

		rtd129x_rbus_guard_lock(&flags);
		writel(data, rle->vstatus);
		rtd129x_rbus_guard_unlock(flags);
	}
	ret = qua_rle_wait(rle);
	if (ret)
		return ret;
	value = QUA_PHY_PORT | QUA_PHY_LOAD | ((addr & 0x0f) << QUA_PHY_CTRL_SHIFT);
	writel(value, rle->insn);
	if (qua_rle_wait(rle))
		return -ETIMEDOUT;
	writel(value & ~QUA_PHY_LOAD, rle->insn);
	if (qua_rle_wait(rle))
		return -ETIMEDOUT;
	value = QUA_PHY_PORT | QUA_PHY_LOAD | ((addr & 0xf0) << 4);
	writel(value, rle->insn);
	if (qua_rle_wait(rle))
		return -ETIMEDOUT;
	writel(value & ~QUA_PHY_LOAD, rle->insn);
	return qua_rle_wait(rle);
}

static int qua_rle_init(struct phy *phy)
{
	struct qua_rle0599 *rle = phy_get_drvdata(phy);
	int i, ret;

	mutex_lock(&rle->lock);
	ret = qua_rle_write(rle, 0xf4, 0x9b);
	for (i = 0; !ret && i < ARRAY_SIZE(qua_page0_addr); i++)
		ret = qua_rle_write(rle, qua_page0_addr[i], qua_page0_data[i]);
	ret = ret ?: qua_rle_write(rle, 0xf4, 0xbb);
	for (i = 0; !ret && i < ARRAY_SIZE(qua_page1_addr); i++)
		ret = qua_rle_write(rle, qua_page1_addr[i], qua_page1_data[i]);
	/* BPi 4.9 init calls rtk_rle0599_phy_toggle(phy, false). */
	ret = ret ?: qua_rle_write(rle, 0xf4, 0x9b);
	ret = ret ?: qua_rle_write(rle, 0xe7, 0x81 | BIT(4) | BIT(5) | BIT(6));
	ret = ret ?: qua_rle_write(rle, 0xf4, 0xbb);
	ret = ret ?: qua_rle_write(rle, 0xe0, 0x25 & ~BIT(2));
	if (!ret) {
		usleep_range(1000, 1500);
		ret = qua_rle_write(rle, 0xe0, 0x25);
	}
	mutex_unlock(&rle->lock);
	return ret;
}

static const struct phy_ops qua_rle_ops = {
	.init = qua_rle_init,
	.owner = THIS_MODULE,
};

static int qua_rle_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qua_rle0599 *rle;
	struct resource *res;
	struct phy_provider *provider;

	rle = devm_kzalloc(dev, sizeof(*rle), GFP_KERNEL);
	if (!rle)
		return -ENOMEM;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	rle->vstatus = devm_ioremap(dev, res->start, resource_size(res));
	if (!rle->vstatus)
		return -ENOMEM;
	/* 0x980130a4 sits inside the EHCI resource.  The EHCI driver is its
	 * sole resource owner, so this PHY uses an unreserved sub-window map. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	rle->insn = devm_ioremap(dev, res->start, resource_size(res));
	if (!rle->insn)
		return -ENOMEM;
	/* resource 2 is the vendor efuse window; stock Qua settings do not use it. */
	mutex_init(&rle->lock);
	rle->phy = devm_phy_create(dev, dev->of_node, &qua_rle_ops);
	if (IS_ERR(rle->phy))
		return PTR_ERR(rle->phy);
	phy_set_drvdata(rle->phy, rle);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id qua_rle_match[] = {
	{ .compatible = "kddi,quastation-rle0599-usb2phy" },
	{}
};
MODULE_DEVICE_TABLE(of, qua_rle_match);

static struct platform_driver qua_rle_driver = {
	.probe = qua_rle_probe,
	.driver = { .name = "qua-rle0599-usb2phy", .of_match_table = qua_rle_match },
};
module_platform_driver(qua_rle_driver);
MODULE_DESCRIPTION("Qua Station RTD1295 legacy EHCI USB2 PHY");
MODULE_LICENSE("GPL");
