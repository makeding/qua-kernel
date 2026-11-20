/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QUA_RTK_HSOTG_H
#define QUA_RTK_HSOTG_H

/* RTD1295 external USB2 UDC: vendor rtk-hsotg register contract. */
#define QUA_DWC2_GUSBCFG		0x00c
#define QUA_DWC2_GINTMSK		0x018
#define QUA_DWC2_GUSBCFG_PHYIF16	BIT(3)
#define QUA_DWC2_GUSBCFG_FORCEDEVMODE	BIT(30)
#define QUA_DWC2_GUSBCFG_FORCEHOSTMODE	BIT(29)

/* The UDC's second DT resource is 0x98013800, shared with EHCI/OHCI PHY2. */
#define QUA_PHY_CTRL0			0x0c
#define QUA_PHY_CTRL1			0x2c
#define QUA_PHY_CTRL2			0x40
#define QUA_PHY_CTRL3			0x44

#endif
