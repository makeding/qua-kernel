// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTD1295 A-series RBUS access guard.
 *
 * The stock Qua Station kernel serializes eMMC register accesses against
 * RBUS device offsets 0x800..0x9ff and leaves a 120-NOP guard interval on
 * both sides. This is a software ordering erratum, not a hardware semaphore.
 */
#include <linux/export.h>
#include <linux/spinlock.h>

#include <soc/realtek/rtd129x-rbus-guard.h>

#define RTD129X_RBUS_GUARD_NOPS 120

static DEFINE_SPINLOCK(rtd129x_rbus_guard);

static __always_inline void rtd129x_rbus_guard_delay(void)
{
	unsigned int count = RTD129X_RBUS_GUARD_NOPS;

	while (count--)
		asm volatile("nop");
}

void rtd129x_rbus_guard_lock(unsigned long *flags)
{
	spin_lock_irqsave(&rtd129x_rbus_guard, *flags);
	rtd129x_rbus_guard_delay();
}
EXPORT_SYMBOL_GPL(rtd129x_rbus_guard_lock);

void rtd129x_rbus_guard_unlock(unsigned long flags)
{
	rtd129x_rbus_guard_delay();
	spin_unlock_irqrestore(&rtd129x_rbus_guard, flags);
}
EXPORT_SYMBOL_GPL(rtd129x_rbus_guard_unlock);
