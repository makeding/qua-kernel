/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _RTD129X_RBUS_GUARD_H
#define _RTD129X_RBUS_GUARD_H

void rtd129x_rbus_guard_lock(unsigned long *flags);
void rtd129x_rbus_guard_unlock(unsigned long flags);

#endif
