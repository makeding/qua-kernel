// SPDX-License-Identifier: GPL-2.0-or-later
/* RTD1295 DW-MSHC wrapper bringup; high speed tuning is deliberately absent. */
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mmc/host.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <asm/barrier.h>
#include <soc/realtek/rtd129x-rbus-guard.h>
#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

#define QUA_SB2_SYNC 0x20
#define QUA_MMC_BYTCNT 0x020
#define QUA_MMC_RINTSTS 0x044
#define QUA_MMC_TCBCNT 0x05c
#define QUA_MMC_TBBCNT 0x060
#define QUA_MMC_WRAPPER_ISR 0x424
#define QUA_MMC_IP_DESC0 0x4a0

struct qua_mmc_priv {
 void __iomem *sb2;
 u8 idmac_diag_count;
};

static void qua_mmc_rbus_sync(struct dw_mci *host)
{
 struct qua_mmc_priv *priv = host->priv;

 dmb(sy);
 writel_relaxed(0, priv->sb2 + QUA_SB2_SYNC);
 dmb(sy);
}

static void qua_mmc_io_lock(struct dw_mci *host, unsigned long *flags)
{
 (void)host;
 rtd129x_rbus_guard_lock(flags);
}

static void qua_mmc_io_unlock(struct dw_mci *host, unsigned long flags)
{
 (void)host;
 rtd129x_rbus_guard_unlock(flags);
}

static void qua_mmc_write(struct dw_mci *host, u32 value, u32 offset)
{
 unsigned long flags;

 qua_mmc_io_lock(host, &flags);
 qua_mmc_rbus_sync(host);
 writel(value, host->regs + offset);
 qua_mmc_io_unlock(host, flags);
}

static int qua_mmc_idmac_validate(struct dw_mci *host, u32 idsts)
{
 struct qua_mmc_priv *priv = host->priv;
 unsigned long flags;
 u32 bytcnt, tbbcnt, tcbcnt, rintsts, wrapper_isr;
 u32 ip_desc[4], desc0;

 qua_mmc_io_lock(host, &flags);
 bytcnt = readl_relaxed(host->regs + QUA_MMC_BYTCNT);
 tbbcnt = readl_relaxed(host->regs + QUA_MMC_TBBCNT);
 tcbcnt = readl_relaxed(host->regs + QUA_MMC_TCBCNT);
 rintsts = readl_relaxed(host->regs + QUA_MMC_RINTSTS);
 wrapper_isr = readl_relaxed(host->regs + QUA_MMC_WRAPPER_ISR);
 ip_desc[0] = readl_relaxed(host->regs + QUA_MMC_IP_DESC0);
 ip_desc[1] = readl_relaxed(host->regs + QUA_MMC_IP_DESC0 + 4);
 ip_desc[2] = readl_relaxed(host->regs + QUA_MMC_IP_DESC0 + 8);
 ip_desc[3] = readl_relaxed(host->regs + QUA_MMC_IP_DESC0 + 12);
 qua_mmc_io_unlock(host, flags);

 desc0 = le32_to_cpu(READ_ONCE(*(__le32 *)host->sg_cpu));
 if (priv->idmac_diag_count < 8) {
  dev_info(host->dev,
    "IDMAC completion %u: BYTCNT=%#x TBBCNT=%#x TCBCNT=%#x IDSTS=%#x RINTSTS=%#x wrapperISR=%#x desc0=%#x IP_DESC=%#x/%#x/%#x/%#x\n",
    priv->idmac_diag_count, bytcnt, tbbcnt, tcbcnt, idsts,
    rintsts, wrapper_isr, desc0, ip_desc[0], ip_desc[1],
    ip_desc[2], ip_desc[3]);
  priv->idmac_diag_count++;
 }

 return tbbcnt == bytcnt ? 0 : -EIO;
}

static int qua_mmc_init(struct dw_mci *host)
{
 struct platform_device *pdev = to_platform_device(host->dev);
 struct qua_mmc_priv *priv;
 /* Use the same conservative low-address DMA zone as the PCIe bringup. */
 int ret = dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(30));
 if (ret) return ret;

 priv = devm_kzalloc(host->dev, sizeof(*priv), GFP_KERNEL);
 if (!priv) return -ENOMEM;
 priv->sb2 = devm_platform_ioremap_resource(pdev, 1);
 if (IS_ERR(priv->sb2)) return PTR_ERR(priv->sb2);
 host->priv = priv;
 host->pre_write = qua_mmc_rbus_sync;
 host->io_lock = qua_mmc_io_lock;
 host->io_unlock = qua_mmc_io_unlock;
 host->idmac_validate = qua_mmc_idmac_validate;
 host->quirks |= DW_MMC_QUIRK_FIFO64_32 |
                 DW_MMC_QUIRK_IDMAC_NO_FIXED_BURST |
                 DW_MMC_QUIRK_FIXED_CARD_THR |
                 DW_MMC_QUIRK_IDMAC_KEEP_CHAIN;
 /* Diagnostic isolation: leave the default path unchanged unless requested. */
 if (of_property_read_bool(host->dev->of_node, "kddi,force-pio"))
  host->quirks |= DW_MMC_QUIRK_FORCE_PIO;

 /* BSP legacy timing: clear DDR/DQS before the core resets and selects clocks. */
 qua_mmc_write(host, 0, 0x10c);
 qua_mmc_write(host, 0, 0x498);
 /* Wrapper uses bit0 as set/clear selector. Mask wrapper DMA/descriptor
  * interrupts, expose only the DesignWare IP interrupt to the GIC. */
 qua_mmc_write(host, 0x1d, 0x424);
 qua_mmc_write(host, 0x10, 0x424);
 dev_info(host->dev, "Qua RTD1295 DW-MMC legacy-mode bringup\n");
 return 0;
}
static const struct dw_mci_drv_data qua_mmc_data = { .init=qua_mmc_init, .common_caps=MMC_CAP_NONREMOVABLE };
static int qua_mmc_probe(struct platform_device *pdev) { return dw_mci_pltfm_register(pdev, &qua_mmc_data); }
static const struct of_device_id qua_mmc_match[] = { { .compatible="kddi,quastation-emmc" }, {} };
static struct platform_driver qua_mmc_driver = { .probe=qua_mmc_probe, .remove=dw_mci_pltfm_remove, .driver={ .name="qua-emmc", .of_match_table=qua_mmc_match } };
builtin_platform_driver(qua_mmc_driver);
