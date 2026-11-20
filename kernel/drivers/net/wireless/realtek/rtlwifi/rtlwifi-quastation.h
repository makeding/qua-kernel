/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Qua RTD1295 PCIe uses a serialized, programmable 4 KiB CPU aperture.
 * PCI BAR bus addresses are not directly usable CPU MMIO on this board.
 */
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <soc/realtek/rtk_pcie.h>

#if IS_REACHABLE(CONFIG_PCI_RTD129X_QUA)
/* The shared-line handler returns without acknowledging while irq_enabled is
 * false. Hold this Qua radio's Linux IRQ until the entire start path is ready.
 * Live gating allowed cold IQK and subsequent C2H/network traffic with DMA on.
 */
static bool qua_rtl_pci_irq_prepare(struct ieee80211_hw *hw)
{
 struct pci_dev *pdev=rtl_pcidev(rtl_pcipriv(hw))->pdev;
 if (!of_machine_is_compatible("kddi,quastation") ||
     pdev->vendor != PCI_VENDOR_ID_REALTEK || pdev->device != 0x818b)
  return false;
 disable_irq(pdev->irq);
 return true;
}

static void qua_rtl_pci_irq_complete(struct ieee80211_hw *hw, bool gated,
                                     bool failed)
{
 struct rtl_priv *priv=rtl_priv(hw);
 struct pci_dev *pdev=rtl_pcidev(rtl_pcipriv(hw))->pdev;
 if (!gated)
  return;
 if (failed)
  priv->cfg->ops->disable_interrupt(hw);
 enable_irq(pdev->irq);
}

#define QUA_PCIE_IO(n) \
static u8 qua##n##_read8(struct rtl_priv *priv, u32 addr) \
{ return rtk_pcie##n##_read(addr, 1); } \
static u16 qua##n##_read16(struct rtl_priv *priv, u32 addr) \
{ return rtk_pcie##n##_read(addr, 2); } \
static u32 qua##n##_read32(struct rtl_priv *priv, u32 addr) \
{ return rtk_pcie##n##_read(addr, 4); } \
static void qua##n##_write8(struct rtl_priv *priv, u32 addr, u8 val) \
{ rtk_pcie##n##_write(addr, 1, val); } \
static void qua##n##_write16(struct rtl_priv *priv, u32 addr, u16 val) \
{ rtk_pcie##n##_write(addr, 2, val); } \
static void qua##n##_write32(struct rtl_priv *priv, u32 addr, u32 val) \
{ rtk_pcie##n##_write(addr, 4, val); }
QUA_PCIE_IO(1)
QUA_PCIE_IO(2)
#undef QUA_PCIE_IO

static void qua_rtl_pci_io_init(struct device *dev, struct rtl_priv *priv)
{
 struct pci_host_bridge *host = pci_find_host_bridge(to_pci_dev(dev)->bus);
 struct device_node *np = dev_of_node(host->dev.parent);
#define QUA_INSTALL_IO(n) do { \
 priv->io.read8 = qua##n##_read8; \
 priv->io.read16 = qua##n##_read16; \
 priv->io.read32 = qua##n##_read32; \
 priv->io.write8 = qua##n##_write8; \
 priv->io.write16 = qua##n##_write16; \
 priv->io.write32 = qua##n##_write32; \
 } while (0)
 if (of_device_is_compatible(np, "realtek,rtk-pcie-slot1")) {
  QUA_INSTALL_IO(1);
  dev_info(dev, "Qua PCIe0 programmable aperture I/O enabled\n");
 } else if (of_device_is_compatible(np, "realtek,rtk-pcie-slot2")) {
  QUA_INSTALL_IO(2);
  dev_info(dev, "Qua PCIe1 programmable aperture I/O enabled\n");
 }
 if (of_device_is_compatible(np, "realtek,rtk-pcie-slot1") ||
     of_device_is_compatible(np, "realtek,rtk-pcie-slot2")) {
  if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(30)))
   dev_err(dev, "Qua low-address DMA constraint failed\n");
  dev_info(dev, "Qua DMA mask=%#llx coherent=%#llx bus_limit=%#llx\n",
           (unsigned long long)*dev->dma_mask,
           (unsigned long long)dev->coherent_dma_mask,
           (unsigned long long)dev->bus_dma_limit);
 }
#undef QUA_INSTALL_IO
}
#else
static inline void qua_rtl_pci_io_init(struct device *dev, struct rtl_priv *priv) {}
static inline bool qua_rtl_pci_irq_prepare(struct ieee80211_hw *hw) { return false; }
static inline void qua_rtl_pci_irq_complete(struct ieee80211_hw *hw, bool gated,
                                           bool failed) {}

#endif
