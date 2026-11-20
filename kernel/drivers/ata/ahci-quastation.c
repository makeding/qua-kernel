// SPDX-License-Identifier: GPL-2.0-or-later
/* Qua Station SATA0. PHY/MAC constants: Realtek vendor ahci_rtk.c (GPLv2+).
 * Command/interrupt/error handling is provided by the current kernel's AHCI.
 */
#include <linux/ahci_platform.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/libata.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include "ahci.h"

#define QUA_RESET_BITS (BIT(5) | BIT(7) | BIT(10))
#define QUA_CLOCK_BITS (BIT(2) | BIT(7))

struct qua_sata {
	struct ahci_host_priv *hpriv;
	struct regmap *crt;
	struct gpio_desc *power;
	void __iomem *oob;
	u32 saved_reset, saved_clocks, saved_oob;
	int saved_direction, saved_power;
	bool active, mmio_live;
};

static struct platform_device *qua_device;
static struct gpiod_lookup_table qua_gpio_lookup = {
	.dev_id = "qua-sata",
	.table = {
		GPIO_LOOKUP("9801b100.gpio", 16, "power", GPIO_ACTIVE_HIGH),
		{ }
	},
};

static void qua_write(struct qua_sata *q, u32 value, unsigned int offset)
{
	writel(value, q->hpriv->mmio + offset);
	readl(q->hpriv->mmio + offset);
	msleep(1);
}

static void qua_phy_init(struct qua_sata *q)
{
	/* Vendor HPC_PLUS, SSC off, default RX, Qua board TX driving level6.
	 * Ordering and 1ms write delays are preserved.
	 */
	static const u32 words[] = {
		0x00001111, 0x00005111, 0x00009111,
		0x538e0411, 0x538e4411, 0x538e8411,
		0x336a0511, 0x336a4511, 0x336a8511,
		0xe0700111, 0xe05c4111, 0xe04a8111,
		0x00150611, 0x00154611, 0x00158611,
		0xc6000a11, 0xc6004a11, 0xc6008a11,
		0x70000211, 0x70004211, 0x70008211,
		0xc6600a11, 0xc6604a11, 0xc6608a11,
		0x20041911, 0x20045911, 0x20049911,
		0x94aa2011, 0x94aa6011, 0x94aaa011,
		0x72100911, 0x72104911, 0x72108911,
		0x27710311, 0x27684311, 0x27648311,
		0x29001011, 0x29005011, 0x29009011,
		0x94a32011, 0x94a36011, 0x94a3a011,
		0x283a2111, 0x283a6111, 0x283aa111,
		0x40000c11, 0x40004c11, 0x40008c11,
		0x00271711, 0x00275711, 0x00279711,
	};
	unsigned int i;

	qua_write(q, 0, 0xf64);
	for (i = 0; i < ARRAY_SIZE(words); i++)
		qua_write(q, words[i], 0xf60);
}

static void qua_mac_init(struct qua_sata *q)
{
	void __iomem *base = q->hpriv->mmio;

	qua_write(q, 0, 0xf64);
	qua_write(q, 0x6726ff81, 0);
	qua_write(q, 0x6737ff81, 0);
	qua_write(q, 0x80000001, 4);
	qua_write(q, 0x80000002, 4);
	qua_write(q, readl(base + 0x14) & ~BIT(0), 0x14);
	qua_write(q, readl(base + 0x0c) | 3, 0x0c);
	qua_write(q, readl(base + 0x18), 0x18);
	qua_write(q, 0xffffffff, 0x114);
	qua_write(q, 0x88, 0x170);
	qua_write(q, 0x10, 0x118);
	qua_write(q, 0x4016, 0x118);
	qua_write(q, 0xf000, 0x140);
	qua_write(q, 0x3c300, 0xf20);
	qua_write(q, 0x1700, 0xa4);
	qua_write(q, 0x0a, 0xf68);
}

static void qua_restore(void *data)
{
	struct qua_sata *q = data;
	void __iomem *base = q->hpriv->mmio;

	if (!q->active)
		return;
	/* Host detach precedes this action. Stop engines before touching gates. */
	if (q->mmio_live) {
		writel(readl(base + HOST_CTL) & ~HOST_IRQ_EN, base + HOST_CTL);
		writel(readl(base + 0x118) & ~(PORT_CMD_START | PORT_CMD_FIS_RX),
		       base + 0x118);
		readl(base + 0x118);
	}
	regmap_update_bits(q->crt, 0, QUA_RESET_BITS, 0);
	writel((readl(q->oob + 0x80) & ~0x115) | (q->saved_oob & 0x115),
	       q->oob + 0x80);
	readl(q->oob + 0x80);
	regmap_update_bits(q->crt, 0x0c, QUA_CLOCK_BITS, q->saved_clocks);
	regmap_update_bits(q->crt, 0, QUA_RESET_BITS, q->saved_reset);
	if (q->saved_direction)
		gpiod_direction_input(q->power);
	else
		gpiod_direction_output(q->power, q->saved_power);
	q->active = false;
	q->mmio_live = false;
}

static void qua_host_stop(struct ata_host *host)
{
	struct ahci_host_priv *hpriv = host->private_data;

	qua_restore(hpriv->plat_data);
}

static struct ata_port_operations qua_port_ops = {
	.inherits = &ahci_ops,
	.host_stop = qua_host_stop,
};

static const struct ata_port_info qua_port_info = {
	.flags = AHCI_FLAG_COMMON,
	.pio_mask = ATA_PIO4,
	.udma_mask = ATA_UDMA6,
	.port_ops = &qua_port_ops,
};

static const struct scsi_host_template qua_sht = {
	AHCI_SHT("qua-sata"),
};

static struct regmap *qua_crt_regmap(void)
{
	struct device_node *np;
	struct resource resource;
	struct regmap *map;

	for_each_compatible_node(np, NULL, "syscon") {
		if (of_address_to_resource(np, 0, &resource) ||
		    resource.start != 0x98000000)
			continue;
		map = syscon_node_to_regmap(np);
		of_node_put(np);
		return map;
	}
	return ERR_PTR(-ENODEV);
}

static int qua_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qua_sata *q;
	int ret;

	q = devm_kzalloc(dev, sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	q->hpriv = devm_kzalloc(dev, sizeof(*q->hpriv), GFP_KERNEL);
	if (!q->hpriv)
		return -ENOMEM;
	q->hpriv->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(q->hpriv->mmio))
		return PTR_ERR(q->hpriv->mmio);
	q->oob = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(q->oob))
		return PTR_ERR(q->oob);
	q->crt = qua_crt_regmap();
	if (IS_ERR(q->crt))
		return PTR_ERR(q->crt);
	q->power = devm_gpiod_get(dev, "power", GPIOD_ASIS);
	if (IS_ERR(q->power))
		return dev_err_probe(dev, PTR_ERR(q->power), "disk-power GPIO\n");
	q->saved_direction = gpiod_get_direction(q->power);
	q->saved_power = gpiod_get_value_cansleep(q->power);
	if (q->saved_direction < 0 || q->saved_power < 0)
		return -EIO;
	ret = regmap_read(q->crt, 0, &q->saved_reset);
	if (ret)
		return ret;
	ret = regmap_read(q->crt, 0x0c, &q->saved_clocks);
	if (ret)
		return ret;
	/* Qualify the firmware handoff before any hardware writes. Do not take
	 * over an already clocked/released SATA instance with unknown DMA state.
	 */
	if ((q->saved_reset & QUA_RESET_BITS) ||
	    (q->saved_clocks & QUA_CLOCK_BITS))
		return dev_err_probe(dev, -EBUSY, "SATA firmware handoff is not reset/gated\n");
	q->saved_oob = readl(q->oob + 0x80);
	q->hpriv->plat_data = q;
	q->hpriv->flags = AHCI_HFLAG_32BIT_ONLY | AHCI_HFLAG_NO_NCQ |
			 AHCI_HFLAG_NO_FBS;
	q->hpriv->mask_port_map = BIT(0);
	ret = devm_add_action_or_reset(dev, qua_restore, q);
	if (ret)
		return ret;
	q->active = true;
	ret = regmap_update_bits(q->crt, 0, QUA_RESET_BITS, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(q->crt, 0x0c, QUA_CLOCK_BITS, QUA_CLOCK_BITS);
	if (ret)
		return ret;
	ret = gpiod_direction_output(q->power, 1);
	if (ret)
		return ret;
	msleep(200);
	ret = regmap_update_bits(q->crt, 0, BIT(5) | BIT(7), BIT(5) | BIT(7));
	if (ret)
		return ret;
	q->mmio_live = true;
	qua_mac_init(q);
	qua_phy_init(q);
	ret = regmap_update_bits(q->crt, 0, BIT(10), BIT(10));
	if (ret)
		return ret;
	writel(readl(q->oob + 0x80) | 0x115, q->oob + 0x80);
	readl(q->oob + 0x80);
	dev_info(dev, "SATA0 powered; integrated PHY initialized; 32-bit DMA\n");
	return ahci_platform_init_host(pdev, q->hpriv, &qua_port_info, &qua_sht);
}

static void qua_remove(struct platform_device *pdev)
{
	ata_platform_remove_one(pdev);
}

static void qua_shutdown(struct platform_device *pdev)
{
	struct ata_host *host = platform_get_drvdata(pdev);
	struct ahci_host_priv *hpriv;

	if (!host)
		return;
	hpriv = host->private_data;
	ahci_platform_shutdown(pdev);
	qua_restore(hpriv->plat_data);
}

static struct platform_driver qua_driver = {
	.probe = qua_probe,
	.remove = qua_remove,
	.shutdown = qua_shutdown,
	.driver = { .name = "qua-sata" },
};

static int __init qua_init(void)
{
	struct device_node *parent;
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, 28, 4 } };
	struct resource resources[] = {
		DEFINE_RES_MEM(0x9803f000, 0x1000),
		DEFINE_RES_MEM(0x9801a900, 0x100),
		DEFINE_RES_IRQ(0),
	};
	struct platform_device_info info = {
		.name = "qua-sata", .id = PLATFORM_DEVID_NONE,
		.res = resources, .num_res = ARRAY_SIZE(resources),
		.dma_mask = DMA_BIT_MASK(32),
	};
	int irq, ret;

	if (!of_machine_is_compatible("kddi,quastation"))
		return -ENODEV;
	parent = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!parent)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(parent);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(parent);
	if (!irq)
		return -EINVAL;
	resources[2].start = irq;
	resources[2].end = irq;
	gpiod_add_lookup_table(&qua_gpio_lookup);
	ret = platform_driver_register(&qua_driver);
	if (ret)
		goto remove_lookup;
	qua_device = platform_device_register_full(&info);
	if (IS_ERR(qua_device)) {
		ret = PTR_ERR(qua_device);
		platform_driver_unregister(&qua_driver);
		goto remove_lookup;
	}
	return 0;
remove_lookup:
	gpiod_remove_lookup_table(&qua_gpio_lookup);
	return ret;
}

static void __exit qua_exit(void)
{
	platform_device_unregister(qua_device);
	platform_driver_unregister(&qua_driver);
	gpiod_remove_lookup_table(&qua_gpio_lookup);
}

module_init(qua_init);
module_exit(qua_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qua Station RTD1295 SATA0 integrated PHY/AHCI");
MODULE_ALIAS("platform:qua-sata");
