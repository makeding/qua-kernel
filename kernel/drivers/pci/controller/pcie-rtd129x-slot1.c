/*
 * Realtek PCIe host controller driver
 *
 * Copyright (c) 2017 Realtek Semiconductor Corp.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/resource.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <linux/reset.h>
#include <linux/suspend.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>

#include "rtk_pcie.h"
#include "../pci.h"
#include "rtk_cpu.h"
#include "pcie-rtd129x.h"
#include <soc/realtek/rtd129x-rbus-guard.h>

spinlock_t rtk_pcie1_lock;
#define DEV_NAME "[RTK PCIE 1]"
#define PCIE_IO_2K_MASK 0xFFFFF800
#define PCIE_IO_4K_MASK 0xFFFFF000

static void __iomem *PCIE_CTRL_BASE;
static void __iomem *PCIE_CFG_BASE;
static void __iomem *SYSTEM_BASE1;
static void __iomem *SYSTEM_BASE2;

/* Qua vendor DT enables rtk_rbus_barrier_flag. Flush the RTD write
 * bridge before publishing new controller/aperture writes. */
static void qua_pcie_rbus_sync(void)
{
 if (!SYSTEM_BASE2)
  return;
 dmb(sy);
 writel_relaxed(0, SYSTEM_BASE2 + 0x20);
 dmb(sy);
}


#ifdef CONFIG_ARCH_REALTEK
static void __iomem *EMMC_MUXPAD;
#endif /* CONFIG_ARCH_REALTEK */

static u32 PCIE2_MMIO_PHY_ADDR;
static u32 PCIE2_MMIO_PHY_ADDR_LEN;

static struct gpio_desc *pcie_gpio_18;

static bool cfg_direct_access;
static bool trace_config;
module_param(trace_config, bool, 0600);
MODULE_PARM_DESC(trace_config, "Trace bounded PCI configuration accesses for board bringup");
static unsigned int trace_count;

static int rtk_cpu_id;
static int rtk_cpu_revision;

static struct clk *pcie0_clk;
static struct reset_control *rstn_pcie0_stitch;
static struct reset_control *rstn_pcie0;
static struct reset_control *rstn_pcie0_core;
static struct reset_control *rstn_pcie0_power;
static struct reset_control *rstn_pcie0_nonstitch;
static struct reset_control *rstn_pcie0_phy;
static struct reset_control *rstn_pcie0_phy_mdio;

static struct pci_bus *bus;
static struct platform_device *local_pdev;

static void rtk_pci_ctrl_write(unsigned long addr, unsigned int val)
{
	qua_pcie_rbus_sync();
	writel(val, addr + PCIE_CTRL_BASE);
}

static unsigned int rtk_pci_ctrl_read(unsigned long addr)
{
	unsigned int val = readl(addr + PCIE_CTRL_BASE);
	return val;
}

static void rtk_pci_direct_cfg_write(unsigned long addr, unsigned int val)
{
	qua_pcie_rbus_sync();
	writel(val, addr + PCIE_CFG_BASE);
}

static void rtk_pci_direct_cfg_write_word(unsigned long addr, u16 val)
{
	qua_pcie_rbus_sync();
	writew(val, addr + PCIE_CFG_BASE);
}

static void rtk_pci_direct_cfg_write_byte(unsigned long addr, u8 val)
{
	qua_pcie_rbus_sync();
	writeb(val, addr + PCIE_CFG_BASE);
}

static unsigned int rtk_pci_direct_cfg_read(unsigned long addr)
{
	unsigned int val = readl(addr + PCIE_CFG_BASE);
	return val;
}

static u16 rtk_pci_direct_cfg_read_word(unsigned long addr)
{
	u16 val = readw(addr + PCIE_CFG_BASE);
	return val;
}

static u8 rtk_pci_direct_cfg_read_byte(unsigned long addr)
{
	u8 val = readb(addr + PCIE_CFG_BASE);
	return val;
}

u32 rtk_pcie_mmio_start(void)
{
	return PCIE2_MMIO_PHY_ADDR;
}

u32 rtk_pcie_mmio_end(void)
{
	return PCIE2_MMIO_PHY_ADDR + (PCIE2_MMIO_PHY_ADDR_LEN - 1);
}

u32 rtk_pcie_mmio_len(void)
{
	return PCIE2_MMIO_PHY_ADDR_LEN;
}

u32 rtk_pcie1_read(u32 addr, u8 size)
{
	u32 rval = 0;
	u32 mask;
	u32 translate_val = 0;
	u32 tmp_addr = addr & 0xFFF;
	u32 pci_error_status = 0;
	int retry_cnt = 0;
	u8 retry = 5;
	unsigned long irqL;
	unsigned long flags;

	spin_lock_irqsave(&rtk_pcie1_lock, irqL);

	/* PCIE1.1 0x9804FCEC, PCIE2.0 0x9803CCEC & 0x9803CC68
	 * can't be used because of 129x hardware issue.
	 */
	if (tmp_addr == 0xCEC || tmp_addr == 0xC68) {
		mask = PCIE_IO_2K_MASK;
		rtk_pci_ctrl_write(0xD00, PCIE_IO_2K_MASK);
		translate_val = rtk_pci_ctrl_read(0xD04);
		rtk_pci_ctrl_write(0xD04, translate_val | (addr & mask));
	} else if (addr >= 0x1000) {
		mask = PCIE_IO_4K_MASK;
		translate_val = rtk_pci_ctrl_read(0xD04);
		rtk_pci_ctrl_write(0xD04, translate_val | (addr & mask));
	} else
		mask = 0x0;

pci_read_retry:

	/* All RBUS1 driver need to have a workaround
	 * for emmc hardware error
	 *
	 * Need to protect 0xXXXX_X8XX~ 0xXXXX_X9XX.
	 */
	if ((tmp_addr > 0x7FF) && (tmp_addr < 0xA00))
		rtd129x_rbus_guard_lock(&flags);

	switch (size) {
	case 1:
		rval = rtk_pci_direct_cfg_read_byte((addr & ~mask));
		break;
	case 2:
		rval = rtk_pci_direct_cfg_read_word((addr & ~mask));
		break;
	case 4:
		rval = rtk_pci_direct_cfg_read((addr & ~mask));
		break;
	default:
		pr_info("%s: wrong size %d\n", __func__, size);
		break;
	}

	if ((tmp_addr > 0x7FF) && (tmp_addr < 0xA00))
		rtd129x_rbus_guard_unlock(flags);

	/* DLLP error patch */
	pci_error_status = rtk_pci_ctrl_read(0xc7c);
	if (pci_error_status & 0x1F) {
		rtk_pci_ctrl_write(0xc7c, pci_error_status);
		pr_info("%s: DLLP(#%d) 0x%x reg=0x%x val=0x%x\n",
			__func__, retry_cnt, pci_error_status, addr, rval);

		if (retry_cnt < retry) {
			retry_cnt++;
			goto pci_read_retry;
		}
	}

	/* PCIE1.1 0x9804FCEC, PCIE2.0 0x9803CCEC & 0x9803CC68
	 * can't be used because of 129x hardware issue.
	 */
	if (tmp_addr == 0xCEC || tmp_addr == 0xC68) {
		rtk_pci_ctrl_write(0xD04, translate_val);
		rtk_pci_ctrl_write(0xD00, PCIE_IO_4K_MASK);
	} else if (addr >= 0x1000) {
		rtk_pci_ctrl_write(0xD04, translate_val);
	}

	spin_unlock_irqrestore(&rtk_pcie1_lock, irqL);

	return rval;
}
EXPORT_SYMBOL(rtk_pcie1_read);

void rtk_pcie1_write(u32 addr, u8 size, u32 wval)
{
	u32 mask;
	u32 translate_val = 0;
	u32 tmp_addr = addr & 0xFFF;
	unsigned long irqL;
	unsigned long flags;

	spin_lock_irqsave(&rtk_pcie1_lock, irqL);

	/* PCIE1.1 0x9804FCEC, PCIE2.0 0x9803CCEC & 0x9803CC68
	 * can't be used because of 129x hardware issue.
	 */
	if (tmp_addr == 0xCEC || tmp_addr == 0xC68) {
		mask = PCIE_IO_2K_MASK;
		rtk_pci_ctrl_write(0xD00, PCIE_IO_2K_MASK);
		translate_val = rtk_pci_ctrl_read(0xD04);
		rtk_pci_ctrl_write(0xD04, translate_val | (addr & mask));
	} else if (addr >= 0x1000) {
		mask = PCIE_IO_4K_MASK;
		translate_val = rtk_pci_ctrl_read(0xD04);
		rtk_pci_ctrl_write(0xD04, translate_val | (addr & mask));
	} else
		mask = 0x0;

	/* All RBUS1 driver need to have a workaround
	 * for emmc hardware error.
	 *
	 * Need to protect 0xXXXX_X8XX~ 0xXXXX_X9XX.
	 */
	if ((tmp_addr > 0x7FF) && (tmp_addr < 0xA00))
		rtd129x_rbus_guard_lock(&flags);

	switch (size) {
	case 1:
		rtk_pci_direct_cfg_write_byte((addr&~mask), wval);
		break;
	case 2:
		rtk_pci_direct_cfg_write_word((addr&~mask), wval);
		break;
	case 4:
		rtk_pci_direct_cfg_write((addr&~mask), wval);
		break;
	default:

		pr_info("%s: wrong size %d\n", __func__, size);
		break;
	}

	if ((tmp_addr > 0x7FF) && (tmp_addr < 0xA00))
		rtd129x_rbus_guard_unlock(flags);

	/* PCIE1.1 0x9804FCEC, PCIE2.0 0x9803CCEC & 0x9803CC68
	 * can't be used because of 129x hardware issue.
	 */
	if (tmp_addr == 0xCEC || tmp_addr == 0xC68) {
		rtk_pci_ctrl_write(0xD04, translate_val);
		rtk_pci_ctrl_write(0xD00, PCIE_IO_4K_MASK);
	} else if (addr >= 0x1000) {
		rtk_pci_ctrl_write(0xD04, translate_val);
	}

	spin_unlock_irqrestore(&rtk_pcie1_lock, irqL);
}
EXPORT_SYMBOL(rtk_pcie1_write);

static int _indirect_cfg_write(unsigned long addr, unsigned long data,
	unsigned char size)
{
	unsigned long status;
	unsigned char mask;
	int try_count = 1000;

#if 0
	pr_alert("_indirect_cfg_write: addr(%x): %x size %d\n",
		addr, data, size);
#endif

	if (ADDR_TO_DEVICE_NO(addr) != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	mask = _pci_byte_mask(addr, size);

	if (!mask)
		return PCIBIOS_SET_FAILED;

	data = (data << _pci_bit_shift(addr)) & _pci_bit_mask(mask);

	rtk_pci_ctrl_write(PCIE_INDIR_CTR, 0x12);
	rtk_pci_ctrl_write(PCIE_CFG_ST, CFG_ST_ERROR|CFG_ST_DONE);
	rtk_pci_ctrl_write(PCIE_CFG_ADDR, addr);
	rtk_pci_ctrl_write(PCIE_CFG_WDATA, data);

	if (size == 4) {
		rtk_pci_ctrl_write(PCIE_CFG_EN, 0x1);
	} else {
		rtk_pci_ctrl_write(PCIE_CFG_EN, BYTE_CNT(mask) |
			BYTE_EN | WRRD_EN(1));
	}

	rtk_pci_ctrl_write(PCIE_CFG_CT, GO_CT);

	do {
		status = rtk_pci_ctrl_read(PCIE_CFG_ST);
		udelay(50);
	} while (!(status & CFG_ST_DONE) && try_count--);

	if (try_count < 0) {
		PCI_CFG_WARNING("write config data (%p) failed - timeout\n",
				(void *) addr);
		goto error_occur;
	}

	if (rtk_pci_ctrl_read(PCIE_CFG_ST) & CFG_ST_ERROR) {

		PCI_CFG_WARNING("write config data failed - ");

		if (status & CFG_ST_DETEC_PAR_ERROR)
			PCI_CFG_WARNING("PAR error detected\n");
		if (status & CFG_ST_SIGNAL_SYS_ERROR)
			PCI_CFG_WARNING("system error\n");
		if (status & CFG_ST_REC_MASTER_ABORT)
			PCI_CFG_WARNING(" master abort\n");
		if (status & CFG_ST_REC_TARGET_ABORT)
			PCI_CFG_WARNING("target abort\n");
		if (status & CFG_ST_SIG_TAR_ABORT)
			PCI_CFG_WARNING("tar abort\n");

		goto error_occur;
	}

	rtk_pci_ctrl_write(PCIE_CFG_ST, CFG_ST_ERROR|CFG_ST_DONE);

	return PCIBIOS_SUCCESSFUL;

error_occur:

	rtk_pci_ctrl_write(PCIE_CFG_ST, CFG_ST_ERROR|CFG_ST_DONE);

	return PCIBIOS_SET_FAILED;
}

static int _indirect_cfg_read(unsigned long addr, u32 *pdata,
	unsigned char size)
{
	unsigned long status;
	unsigned char mask;
	int try_count = 20000;

	if (ADDR_TO_DEVICE_NO(addr) != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	mask = _pci_byte_mask(addr, size);

	if (!mask)
		return PCIBIOS_SET_FAILED;

	rtk_pci_ctrl_write(PCIE_INDIR_CTR, 0x10);
	rtk_pci_ctrl_write(PCIE_CFG_ST, 0x3);
	rtk_pci_ctrl_write(PCIE_CFG_ADDR, (addr & ~0x3));
	rtk_pci_ctrl_write(PCIE_CFG_EN, BYTE_CNT(mask) | BYTE_EN | WRRD_EN(0));
	rtk_pci_ctrl_write(PCIE_CFG_CT, GO_CT);

	do {
		status = rtk_pci_ctrl_read(PCIE_CFG_ST);
		udelay(50);
	} while (!(status & CFG_ST_DONE) && try_count--);

	if (try_count < 0) {
		PCI_CFG_WARNING("read config data (%p) failed - timeout\n",
			(void *) addr);
		goto error_occur;
	}

	if (rtk_pci_ctrl_read(PCIE_CFG_ST) & CFG_ST_ERROR) {

		PCI_CFG_WARNING("read config data failed - ");

		if (status & CFG_ST_DETEC_PAR_ERROR)
			PCI_CFG_WARNING("PAR error detected\n");
		if (status & CFG_ST_SIGNAL_SYS_ERROR)
			PCI_CFG_WARNING("system error\n");
		if (status & CFG_ST_REC_MASTER_ABORT)
			PCI_CFG_WARNING("master abort\n");
		if (status & CFG_ST_REC_TARGET_ABORT)
			PCI_CFG_WARNING("target abort\n");
		if (status & CFG_ST_SIG_TAR_ABORT)
			PCI_CFG_WARNING("tar abort\n");
		goto error_occur;
	}

	rtk_pci_ctrl_write(PCIE_CFG_ST, 0x3);

	*pdata = (rtk_pci_ctrl_read(PCIE_CFG_RDATA) & _pci_bit_mask(mask))
		>> _pci_bit_shift(addr);
	return PCIBIOS_SUCCESSFUL;

error_occur:
	rtk_pci_ctrl_write(PCIE_CFG_ST, 0x3);
	return PCIBIOS_SET_FAILED;
}

static int rtk_pcie_rd_conf(struct pci_bus *bus, unsigned int devfn,
	int reg, int size, u32 *pval)
{
	unsigned long address;
	int ret = PCIBIOS_DEVICE_NOT_FOUND;
	u32 val = 0;
	u8 retry = 5;
	bool trace = trace_config && trace_count++ < 512;
	if (trace) dev_info(&bus->dev, "cfg read start %02x:%02x reg=%x size=%d\n", bus->number, devfn, reg, size);

	if (rtk_cpu_revision == RTD129x_CHIP_REVISION_A00)
		udelay(200);

again:
	if (bus->number == 0 && PCI_SLOT(devfn) == 0 && PCI_FUNC(devfn) == 0) {
		if (cfg_direct_access) {
			rtk_pci_ctrl_write(0xC00, 0x40012);

			if (size == 1)
				*pval = rtk_pci_direct_cfg_read_byte(reg);
			else if (size == 2)
				*pval = rtk_pci_direct_cfg_read_word(reg);
			else if (size == 4)
				*pval = rtk_pci_direct_cfg_read(reg);

			rtk_pci_ctrl_write(0xC00, 0x1E0002);
			ret = PCIBIOS_SUCCESSFUL;

		} else {
			address = _pci_address_conversion(bus, devfn, reg);
			ret = _indirect_cfg_read(address, pval, size);
		}
	}

	val = rtk_pci_ctrl_read(0xC7C);
	if ((val & 0x1f) && retry) {
		rtk_pci_ctrl_write(0xC7C, val);
		retry--;
		dev_err(&bus->dev, "pcie1 dllp error occur = 0x%x\n", val);
		goto again;
	}

#if 0
	pr_info("%s devfn = 0x%x, reg = 0x%x, *pval = 0x%x\n",
		_func_, devfn, reg, *pval);
#endif

	if (trace) dev_info(&bus->dev, "cfg read done reg=%x ret=%d value=%08x\n", reg, ret, ret ? 0 : *pval);
	return ret;
}

static int rtk_pcie_wr_conf(struct pci_bus *bus, unsigned int devfn,
	int reg, int size, u32 val)
{
	unsigned long address;
	int ret = PCIBIOS_DEVICE_NOT_FOUND;
	bool trace = trace_config && trace_count++ < 512;
	if (trace) dev_info(&bus->dev, "cfg write start %02x:%02x reg=%x size=%d value=%08x\n", bus->number, devfn, reg, size, val);

	if (rtk_cpu_revision == RTD129x_CHIP_REVISION_A00)
		udelay(200);

#if 0
	pr_info("%s devfn = 0x%x, reg = 0x%x, *pval = 0x%x\n",
		_func_, devfn, reg, *pval);
#endif

	if (bus->number == 0 && PCI_SLOT(devfn) == 0 && PCI_FUNC(devfn) == 0) {
		if ((reg == 0x10) || (reg == 0x18) || (reg == 0x20) ||
			(reg == 0x24)) {

			if ((val & 0xc0000000) == 0xc0000000)
				rtk_pci_ctrl_write(0xD04, (val&0xfffffff0));
		}

		if (cfg_direct_access) {
			rtk_pci_ctrl_write(0xC00, 0x40012);

			if (size == 1)
				rtk_pci_direct_cfg_write_byte(reg, val);
			else if (size == 2)
				rtk_pci_direct_cfg_write_word(reg, val);
			else if (size == 4)
				rtk_pci_direct_cfg_write(reg, val);

			rtk_pci_ctrl_write(0xC00, 0x1E0002);
			ret = PCIBIOS_SUCCESSFUL;
		} else {
			address = _pci_address_conversion(bus, devfn, reg);
			ret = _indirect_cfg_write(address, val, size);
		}
	}

	if (trace) dev_info(&bus->dev, "cfg write done reg=%x ret=%d\n", reg, ret);
	return ret;
}

static struct pci_ops rtk_pcie_ops = {
	.read = rtk_pcie_rd_conf,
	.write = rtk_pcie_wr_conf,
};

static int rtk_pcie_hw_initial(struct device *dev)
{
	bool pci_link_detected;
	int timeout = 0;
	int ret = 0;
	u32 val;
	u32 *phy;
	int size = 0, i = 0;

	if (rtk_cpu_revision == RTD129x_CHIP_REVISION_A00 ||
		(rtk_cpu_id == RTK1296_CPU_ID &&
		rtk_cpu_revision != RTD129x_CHIP_REVISION_B00)) {

		/* 0x9801C614[2:0] = 1 */
		val = readl(SYSTEM_BASE1 + 0x14);
		val &= (~0x7);
		val |= 0x1;
		writel(val, (SYSTEM_BASE1 + 0x14));

		/* 0x9801C600[3:0] = 0 --PCIE */
		val = readl(SYSTEM_BASE1);
		val &= (~0xf);
		writel(val, SYSTEM_BASE1);

		/* 0x9801C608[7:0] = 8’b01010000 */
		writeb(0x51, SYSTEM_BASE1 + 0x8);
	}

	reset_control_deassert(rstn_pcie0_stitch);
	reset_control_deassert(rstn_pcie0);
	reset_control_deassert(rstn_pcie0_core);
	reset_control_deassert(rstn_pcie0_power);
	reset_control_deassert(rstn_pcie0_nonstitch);
	reset_control_deassert(rstn_pcie0_phy);
	reset_control_deassert(rstn_pcie0_phy_mdio);
	ret = clk_prepare_enable(pcie0_clk);
	if (ret) {
		dev_err(dev, "unable to enable pcie0_clk clock\n");
		clk_disable_unprepare(pcie0_clk);
		return -EINVAL;
	}

	/* #bit17 null for mdio rstN */
	rtk_pci_ctrl_write(0xC00, 0x00140010);

	if (rtk_cpu_revision == RTD129x_CHIP_REVISION_A01 ||
		rtk_cpu_revision == RTD129x_CHIP_REVISION_A00) {

		size = of_property_count_u32_elems(dev->of_node, "phys_a");
		if (size <= 0 || size > 128) return -EINVAL;
		phy = kmalloc(size * sizeof(u32), GFP_KERNEL);
	if (!phy) return -ENOMEM;
		of_property_read_u32_array(dev->of_node, "phys_a", phy, size);
		for (i = 0; i < size; i++) {
			rtk_pci_ctrl_write(0xC1C, phy[i]);
			mdelay(1);
		}
		kfree(phy);

	} else {
		size = of_property_count_u32_elems(dev->of_node, "phys_b");
		if (size <= 0 || size > 128) return -EINVAL;
		phy = kmalloc(size * sizeof(u32), GFP_KERNEL);
	if (!phy) return -ENOMEM;
		of_property_read_u32_array(dev->of_node, "phys_b", phy, size);
		for (i = 0; i < size; i++) {
			rtk_pci_ctrl_write(0xC1C, phy[i]);
			mdelay(1);
		}
		kfree(phy);

	}

	/* Reset PCIE1 device */
	gpiod_set_raw_value_cansleep(pcie_gpio_18, 0);
	ret = 0;
	mdelay(100);
	gpiod_set_raw_value_cansleep(pcie_gpio_18, 1);
	ret = 0;

	if (cfg_direct_access)
		rtk_pci_ctrl_write(0xC00, 0x00040012);
	else
		rtk_pci_ctrl_write(0xC00, 0x001E0022);

	mdelay(50);

	/* #Link initial setting */
	rtk_pci_ctrl_write(0x710, 0x00010120);

	do {
		pci_link_detected = (rtk_pci_ctrl_read(0xCB4) & 0x800);
		if (!pci_link_detected) {
			mdelay(TIMEOUT_RESOLUTION);
			timeout += TIMEOUT_RESOLUTION;
		}
	} while (!pci_link_detected && timeout < PCIE_CONNECT_TIMEOUT);

	if (pci_link_detected) {
		dev_err(dev, "pcie device has link up in slot 1\n");
	} else {
		reset_control_assert(rstn_pcie0_stitch);
		reset_control_assert(rstn_pcie0);
		reset_control_assert(rstn_pcie0_core);
		reset_control_assert(rstn_pcie0_power);
		reset_control_assert(rstn_pcie0_nonstitch);
		reset_control_assert(rstn_pcie0_phy);
		reset_control_assert(rstn_pcie0_phy_mdio);
		clk_disable_unprepare(pcie0_clk);
		/* PERST GPIO is device-managed. */
		dev_err(dev, "pcie device has link down in slot 1\n");
		return -ENODEV;
	}

#ifdef CONFIG_ARCH_REALTEK
	//writel(readl(EMMC_MUXPAD + 0x61C) | 0x00000004, EMMC_MUXPAD + 0x61C);
#endif /* CONFIG_ARCH_REALTEK */

	/* #make sure DBI is working */
	rtk_pci_ctrl_write(0x04, 0x00000007);

	/* #Base */
	rtk_pci_ctrl_write(0xCFC, 0x9804f000);

	/* #Mask */
	rtk_pci_ctrl_write(0xD00, 0xFFFFF000);

	/* #translate for CFG R/W */
	rtk_pci_ctrl_write(0xD04, 0xC0000000);

	/* prevent pcie hang if dllp error occur*/
	rtk_pci_ctrl_write(0xC78, 0x200001);

	/* set limit and base register */
	rtk_pci_ctrl_write(0x20, 0x0000FFF0);
	rtk_pci_ctrl_write(0x24, 0x0000FFF0);

	return 0;
}

static int rtk_pcie_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct pci_host_bridge *bridge;
	struct resource pcie_mmio_res;

	local_pdev = pdev;
	cfg_direct_access = false;

	dev_info(&pdev->dev, "pcie host driver initial begin\n");

	spin_lock_init(&rtk_pcie1_lock);

	PCIE_CTRL_BASE = of_iomap(pdev->dev.of_node, 0);
	if (!PCIE_CTRL_BASE) {
		dev_err(&pdev->dev, "can't request 'ctrl' address\n");
		ret = -EINVAL;
		goto err_out;
	}

	PCIE_CFG_BASE = of_iomap(pdev->dev.of_node, 1);
	if (!PCIE_CFG_BASE) {
		dev_err(&pdev->dev, "can't request 'cfg' address\n");
		ret = -EINVAL;
		goto err_out;
	} else {
		of_address_to_resource(pdev->dev.of_node, 1, &pcie_mmio_res);
		PCIE2_MMIO_PHY_ADDR = pcie_mmio_res.start;
		PCIE2_MMIO_PHY_ADDR_LEN = resource_size(&pcie_mmio_res);
	}

	SYSTEM_BASE1 = of_iomap(pdev->dev.of_node, 2);
	if (!SYSTEM_BASE1) {
		dev_err(&pdev->dev, "can't request 'base1' address\n");
		ret = -EINVAL;
		goto err_out;
	}

	SYSTEM_BASE2 = of_iomap(pdev->dev.of_node, 3);
	if (!SYSTEM_BASE2) {
		dev_err(&pdev->dev, "can't request 'base2' address\n");
		ret = -EINVAL;
		goto err_out;
	}

#ifdef CONFIG_ARCH_REALTEK
	EMMC_MUXPAD = of_iomap(pdev->dev.of_node, 4);
	if (!EMMC_MUXPAD) {
		dev_err(&pdev->dev, "can't request 'EMMC_MUXPAD' address\n");
		ret = -EINVAL;
		goto err_out;
	}

	pcie_gpio_18 = devm_gpiod_get(&pdev->dev, NULL, GPIOD_OUT_LOW);
	if (IS_ERR(pcie_gpio_18))
		return dev_err_probe(&pdev->dev, PTR_ERR(pcie_gpio_18), "PERST GPIO unavailable\n");

#endif /* CONFIG_ARCH_REALTEK */

	pcie0_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(pcie0_clk)) {
		dev_err(&pdev->dev, "clk_en_pcie0 source missing or invalid\n");
		ret = PTR_ERR(pcie0_clk);
		goto err_out;
	}

	rstn_pcie0_stitch = devm_reset_control_get(&pdev->dev, "stitch");
	if (IS_ERR(rstn_pcie0_stitch)) {
		dev_err(&pdev->dev, "stitch source missing or invalid\n");
		ret = PTR_ERR(rstn_pcie0_stitch);
		goto err_out;
	}

	rstn_pcie0 = devm_reset_control_get(&pdev->dev, "rstn");
	if (IS_ERR(rstn_pcie0)) {
		dev_err(&pdev->dev, "rstn_pcie0 source missing or invalid\n");
		ret = PTR_ERR(rstn_pcie0);
		goto err_out;
	}

	rstn_pcie0_core = devm_reset_control_get(&pdev->dev, "core");
	if (IS_ERR(rstn_pcie0_core)) {
		dev_err(&pdev->dev, "core source missing or invalid\n");
		ret = PTR_ERR(rstn_pcie0_core);
		goto err_out;
	}

	rstn_pcie0_power = devm_reset_control_get(&pdev->dev, "power");
	if (IS_ERR(rstn_pcie0_power)) {
		dev_err(&pdev->dev, "power source missing or invalid\n");
		ret = PTR_ERR(rstn_pcie0_power);
		goto err_out;
	}

	rstn_pcie0_nonstitch = devm_reset_control_get(&pdev->dev, "nonstitch");
	if (IS_ERR(rstn_pcie0_nonstitch)) {
		dev_err(&pdev->dev, "nonstitch source missing or invalid\n");
		ret = PTR_ERR(rstn_pcie0_nonstitch);
		goto err_out;
	}

	rstn_pcie0_phy = devm_reset_control_get(&pdev->dev, "phy");
	if (IS_ERR(rstn_pcie0_phy)) {
		dev_err(&pdev->dev, "phy source missing or invalid\n");
		ret = PTR_ERR(rstn_pcie0_phy);
		goto err_out;
	}

	rstn_pcie0_phy_mdio = devm_reset_control_get(&pdev->dev, "phy_mdio");
	if (IS_ERR(rstn_pcie0_phy_mdio)) {
		dev_err(&pdev->dev, "phy_mdio source missing or invalid\n");
		ret = PTR_ERR(rstn_pcie0_phy_mdio);
		goto err_out;
	}

	reset_control_assert(rstn_pcie0_stitch);
	reset_control_assert(rstn_pcie0);
	reset_control_assert(rstn_pcie0_core);
	reset_control_assert(rstn_pcie0_power);
	reset_control_assert(rstn_pcie0_nonstitch);
	reset_control_assert(rstn_pcie0_phy);
	reset_control_assert(rstn_pcie0_phy_mdio);

	rtk_cpu_id = get_rtd129x_cpu_id();
	rtk_cpu_revision = get_rtd129x_cpu_revision();

	if (rtk_pcie_hw_initial(&pdev->dev) < 0) {
		dev_err(&pdev->dev, "rtk_pcie_hw_initial fail\n");
		ret = -EINVAL;
		goto err_out;
	}

	/* Modern PCI core parses windows and domains from this host's DT. */
	bridge = devm_pci_alloc_host_bridge(&pdev->dev, 0);
	if (!bridge)
		return -ENOMEM;
	bridge->ops = &rtk_pcie_ops;
	ret = pci_host_probe(bridge);
	if (ret)
		return ret;
	bus = bridge->bus;

	dev_info(&pdev->dev, "pcie host driver initial done\n");

err_out:

	return ret;
}

#ifdef CONFIG_SUSPEND
static int rtk_pcie1_suspend(struct device *dev)
{
	dev_info(dev, "suspend enter ...\n");
	if (RTK_PM_STATE == PM_SUSPEND_STANDBY) {
		rtk_pci_ctrl_write(0x178, 0xA3FF0001);
		rtk_pci_ctrl_write(0x098, 0x400);
		rtk_pci_ctrl_write(0xC6C, 0x00000031);
		dev_info(dev, "Idle mode\n");
	} else {
		dev_info(dev, "Suspend mode\n");
		reset_control_assert(rstn_pcie0_stitch);
		reset_control_assert(rstn_pcie0);
		reset_control_assert(rstn_pcie0_core);
		reset_control_assert(rstn_pcie0_power);
		reset_control_assert(rstn_pcie0_nonstitch);
		reset_control_assert(rstn_pcie0_phy);
		reset_control_assert(rstn_pcie0_phy_mdio);
		clk_disable_unprepare(pcie0_clk);
		/* PERST GPIO is device-managed. */
	}
	dev_info(dev, "suspend exit ...\n");

	return 0;
}

static int rtk_pcie1_resume(struct device *dev)
{
	int ret = 0;

	dev_info(dev, "resume enter ...\n");

	if (RTK_PM_STATE == PM_SUSPEND_STANDBY) {
		rtk_pci_ctrl_write(0xC6C, 0x00000032);
		dev_info(dev, "Idle mode\n");
	} else {
		dev_info(dev, "Suspend mode\n");
		if (IS_ERR(pcie_gpio_18))
			return PTR_ERR(pcie_gpio_18);

	dev_info(dev, "resume exit ...\n");

	return ret;
}

static const struct dev_pm_ops rtk_pcie1_pm_ops = {
	.suspend_noirq = rtk_pcie1_suspend,
	.resume_noirq = rtk_pcie1_resume,
};
#endif /* CONFIG_SUSPEND */

static const struct of_device_id rtk_pcie_match_table[] = {
	{.compatible = "realtek,rtk-pcie-slot1",},
	{},
};

static struct platform_driver rtk_pcie_driver = {
	.driver = {
		.name = DEV_NAME,
		.of_match_table = of_match_ptr(rtk_pcie_match_table),
#ifdef CONFIG_SUSPEND
		.pm = &rtk_pcie1_pm_ops,
#endif
	},
	.probe = rtk_pcie_probe,
};
module_platform_driver(rtk_pcie_driver);

MODULE_AUTHOR("James Tai <james.tai@realtek.com>");
MODULE_DESCRIPTION("Realtek PCIe slot1 host controller driver");

MODULE_LICENSE("GPL");
