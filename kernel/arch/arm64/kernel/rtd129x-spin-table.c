// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTD1295 cold-boot spin table.
 *
 * The firmware release register is 32-bit even though the DT property is a
 * 64-bit address. Hotplug power sequencing is deliberately not implemented.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/smp.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/smp_plat.h>

extern void secondary_holding_pen(void);
extern volatile unsigned long secondary_holding_pen_release;

static phys_addr_t rtd129x_release_addr[NR_CPUS];

static void rtd129x_write_pen_release(u64 value)
{
	unsigned long start = (unsigned long)&secondary_holding_pen_release;

	secondary_holding_pen_release = value;
	dcache_clean_inval_poc(start, start +
			      sizeof(secondary_holding_pen_release));
}

static int rtd129x_spin_table_cpu_init(unsigned int cpu)
{
	struct device_node *dn;
	int ret;

	dn = of_get_cpu_node(cpu, NULL);
	if (!dn)
		return -ENODEV;
	ret = of_property_read_u64(dn, "cpu-release-addr",
				   &rtd129x_release_addr[cpu]);
	of_node_put(dn);
	if (ret)
		pr_err("CPU%u: missing RTD129x cpu-release-addr\n", cpu);
	return ret;
}

static int rtd129x_spin_table_cpu_prepare(unsigned int cpu)
{
	phys_addr_t pen = __pa_symbol(secondary_holding_pen);
	void __iomem *release;

	if (!rtd129x_release_addr[cpu])
		return -ENODEV;
	if (upper_32_bits(pen))
		return -ERANGE;

	release = ioremap(rtd129x_release_addr[cpu], sizeof(u32));
	if (!release)
		return -ENOMEM;

	writel_relaxed(lower_32_bits(pen), release);
	wmb();
	readl_relaxed(release);
	sev();
	iounmap(release);
	return 0;
}

static int rtd129x_spin_table_cpu_boot(unsigned int cpu)
{
	rtd129x_write_pen_release(cpu_logical_map(cpu));
	sev();
	return 0;
}

const struct cpu_operations rtd129x_spin_table_ops = {
	.name = "rtk-spin-table",
	.cpu_init = rtd129x_spin_table_cpu_init,
	.cpu_prepare = rtd129x_spin_table_cpu_prepare,
	.cpu_boot = rtd129x_spin_table_cpu_boot,
};
