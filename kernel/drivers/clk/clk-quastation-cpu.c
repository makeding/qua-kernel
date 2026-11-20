// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * RTD1295 SCPU PLL clock.
 *
 * The SB2 hardware semaphore serializes PLL snapshots and changes with the
 * resident A/V firmware. Voltage ordering is owned by the standard OPP core.
 */
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define RTD129X_SCPU_DIV	0x030
#define RTD129X_SCPU_SSC	0x500
#define RTD129X_SCPU_PLL	0x504
#define RTD129X_SCPU_READY	0x51c

#define RTD129X_SCPU_PLL_MASK	0x7ffff
#define RTD129X_SB2_SEM_RETRIES	10240
#define RTD129X_PLL_READY	BIT(20)
#define RTD129X_LSM_STEP_HIGH	(0x1ff << 17)

#define RTD129X_NF(_n, _f) (((_n) << 11) | (_f))

struct rtd129x_cpu_rate {
	u32 value;
	unsigned long rate;
};

static const struct rtd129x_cpu_rate rtd129x_cpu_rates[] = {
	{ RTD129X_NF(34, 75),   1000000000 },
	{ RTD129X_NF(37, 1517), 1100000000 },
	{ RTD129X_NF(41, 910),  1200000000 },
	{ RTD129X_NF(45, 303),  1300000000 },
	{ RTD129X_NF(48, 1745), 1400000000 },
	{ RTD129X_NF(52, 1137), 1500000000 },
	{ RTD129X_NF(56, 531),  1600000000 },
	{ RTD129X_NF(63, 1365), 1800000000 },
	{ RTD129X_NF(41, 1024), 1200000000 },
	{ RTD129X_NF(45, 1024), 1300000000 },
	{ RTD129X_NF(48, 1744), 1503000000 },
};

static const unsigned long rtd129x_output_rates[] = {
	300000000, 400000000, 500000000, 600000000,
	700000000, 800000000, 900000000, 1000000000,
	1100000000, 1200000000, 1300000000, 1400000000,
	1500000000, 1503000000, 1600000000, 1800000000,
};

struct rtd129x_cpu_clk {
	struct clk_hw hw;
	struct regmap *crt;
	struct regmap *sb2;
	bool warned_sem;
};

#define to_rtd129x_cpu_clk(_hw) \
	container_of(_hw, struct rtd129x_cpu_clk, hw)

static bool rtd129x_cpu_sem_lock(struct rtd129x_cpu_clk *cpu)
{
	unsigned int retry = RTD129X_SB2_SEM_RETRIES;
	unsigned int value;

	while (retry--)
		if (!regmap_read(cpu->sb2, 0, &value) && value)
			return true;
	return false;
}

static void rtd129x_cpu_sem_unlock(struct rtd129x_cpu_clk *cpu)
{
	regmap_write(cpu->sb2, 0, 0);
}

static unsigned int rtd129x_cpu_divisor(u32 value)
{
	switch ((value >> 7) & 0x3) {
	case 2:
		return 2;
	case 3:
		return 4;
	default:
		return 1;
	}
}

static const struct rtd129x_cpu_rate *
rtd129x_cpu_find_pll_rate(unsigned long rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rtd129x_cpu_rates); i++)
		if (rtd129x_cpu_rates[i].rate == rate)
			return &rtd129x_cpu_rates[i];
	return NULL;
}

static unsigned int rtd129x_cpu_target_divisor(unsigned long rate)
{
	if (rate >= 1000000000)
		return 1;
	if (rate >= 500000000)
		return 2;
	return 4;
}

static u32 rtd129x_cpu_divisor_value(unsigned int divisor)
{
	switch (divisor) {
	case 2:
		return 2;
	case 4:
		return 3;
	default:
		return 1;
	}
}

static unsigned long rtd129x_cpu_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct rtd129x_cpu_clk *cpu = to_rtd129x_cpu_clk(hw);
	unsigned long rate = 0;
	unsigned int divisor;
	bool sem_locked;
	u32 pll = 0;
	u32 ssc = 0;
	u32 div = 0;
	int i;
	int ret;

	sem_locked = rtd129x_cpu_sem_lock(cpu);
	if (!sem_locked) {
		if (!cpu->warned_sem) {
			pr_warn("quastation-cpu-clk: SB2 semaphore busy\n");
			cpu->warned_sem = true;
		}
		return 0;
	}
	ret = regmap_read(cpu->crt, RTD129X_SCPU_SSC, &ssc);
	ret |= regmap_read(cpu->crt, RTD129X_SCPU_PLL, &pll);
	ret |= regmap_read(cpu->crt, RTD129X_SCPU_DIV, &div);
	pll &= RTD129X_SCPU_PLL_MASK;
	rtd129x_cpu_sem_unlock(cpu);
	if (ret)
		return 0;

	divisor = rtd129x_cpu_divisor(div);
	if (!(ssc & BIT(0)))
		return 199999999 / divisor;

	for (i = 0; i < ARRAY_SIZE(rtd129x_cpu_rates); i++) {
		if (rtd129x_cpu_rates[i].value == pll) {
			rate = rtd129x_cpu_rates[i].rate;
			break;
		}
	}
	if (!rate) {
		u32 n = (pll >> 11) & 0xff;
		u32 f = pll & 0x7ff;

		rate = parent_rate * (n + 3);
		rate += DIV_ROUND_CLOSEST_ULL((u64)parent_rate * f, 2048);
		pr_warn_ratelimited("quastation-cpu-clk: unknown PLL value %#x\n",
				    pll);
	}

	return rate / divisor;
}

static int rtd129x_cpu_determine_rate(struct clk_hw *hw,
				    struct clk_rate_request *req)
{
	unsigned long selected = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(rtd129x_output_rates); i++) {
		unsigned long rate = rtd129x_output_rates[i];

		if (rate < req->min_rate || rate > req->max_rate)
			continue;
		if (!selected || rate <= req->rate)
			selected = rate;
		if (rate >= req->rate)
			break;
	}
	if (!selected)
		return -EINVAL;
	req->rate = selected;
	return 0;
}

static int rtd129x_cpu_set_divisor(struct rtd129x_cpu_clk *cpu,
				   unsigned int divisor)
{
	return regmap_update_bits(cpu->crt, RTD129X_SCPU_DIV, 0x3 << 7,
				  rtd129x_cpu_divisor_value(divisor) << 7);
}

static int rtd129x_cpu_program_pll(struct rtd129x_cpu_clk *cpu,
				   const struct rtd129x_cpu_rate *target)
{
	unsigned int value;
	int ret;

	ret = regmap_update_bits(cpu->crt, RTD129X_SCPU_SSC + 8,
				 RTD129X_LSM_STEP_HIGH,
				 RTD129X_LSM_STEP_HIGH);
	if (ret)
		return ret;
	ret = regmap_update_bits(cpu->crt, RTD129X_SCPU_SSC, 0x7, 0x4);
	if (ret)
		return ret;
	ret = regmap_update_bits(cpu->crt, RTD129X_SCPU_PLL,
				 RTD129X_SCPU_PLL_MASK, target->value);
	if (ret)
		return ret;
	ret = regmap_update_bits(cpu->crt, RTD129X_SCPU_SSC, 0x7, 0x5);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout_atomic(cpu->crt, RTD129X_SCPU_READY,
					      value,
					      value & RTD129X_PLL_READY,
					      1, 2000000);
	return ret;
}

static int rtd129x_cpu_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct rtd129x_cpu_clk *cpu = to_rtd129x_cpu_clk(hw);
	const struct rtd129x_cpu_rate *safe_1ghz;
	const struct rtd129x_cpu_rate *target;
	unsigned int old_divisor;
	unsigned int new_divisor;
	unsigned int div_reg;
	int ret;

	new_divisor = rtd129x_cpu_target_divisor(rate);
	target = rtd129x_cpu_find_pll_rate(rate * new_divisor);
	if (!target)
		return -EINVAL;
	safe_1ghz = rtd129x_cpu_find_pll_rate(1000000000);
	if (!safe_1ghz)
		return -EINVAL;
	if (!rtd129x_cpu_sem_lock(cpu))
		return -EBUSY;

	ret = regmap_read(cpu->crt, RTD129X_SCPU_DIV, &div_reg);
	if (ret)
		goto unlock;
	old_divisor = rtd129x_cpu_divisor(div_reg);

	/*
	 * The vendor sequence avoids a glitch when crossing between /1 and
	 * /2 or /4: first move the PLL to 1 GHz, then change the divider.
	 */
	if (new_divisor != old_divisor &&
	    (new_divisor == 1 || old_divisor == 1)) {
		ret = rtd129x_cpu_program_pll(cpu, safe_1ghz);
		if (ret)
			goto unlock;
		ret = rtd129x_cpu_set_divisor(cpu, new_divisor);
		if (ret)
			goto unlock;
		old_divisor = new_divisor;
	}

	if (new_divisor > old_divisor) {
		ret = rtd129x_cpu_set_divisor(cpu, new_divisor);
		if (ret)
			goto unlock;
	}
	ret = rtd129x_cpu_program_pll(cpu, target);
	if (ret)
		goto unlock;
	if (new_divisor < old_divisor)
		ret = rtd129x_cpu_set_divisor(cpu, new_divisor);

unlock:
	rtd129x_cpu_sem_unlock(cpu);
	return ret;
}

static const struct clk_ops rtd129x_cpu_clk_ops = {
	.recalc_rate = rtd129x_cpu_recalc_rate,
	.determine_rate = rtd129x_cpu_determine_rate,
	.set_rate = rtd129x_cpu_set_rate,
};

static int rtd129x_cpu_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct rtd129x_cpu_clk *cpu;
	const char *parent_name;
	int ret;

	cpu = devm_kzalloc(dev, sizeof(*cpu), GFP_KERNEL);
	if (!cpu)
		return -ENOMEM;
	cpu->crt = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "realtek,crt");
	if (IS_ERR(cpu->crt))
		return PTR_ERR(cpu->crt);
	cpu->sb2 = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "realtek,sb2");
	if (IS_ERR(cpu->sb2))
		return PTR_ERR(cpu->sb2);

	parent_name = of_clk_get_parent_name(dev->of_node, 0);
	init.name = "qua_cpu";
	init.ops = &rtd129x_cpu_clk_ops;
	init.parent_names = &parent_name;
	init.num_parents = 1;
	init.flags = CLK_GET_RATE_NOCACHE;
	cpu->hw.init = &init;

	ret = devm_clk_hw_register(dev, &cpu->hw);
	if (ret)
		return ret;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &cpu->hw);
	if (ret)
		return ret;

	dev_info(dev, "SCPU clock rate %lu Hz\n",
		 clk_hw_get_rate(&cpu->hw));
	return 0;
}

static const struct of_device_id rtd129x_cpu_clk_of_match[] = {
	{ .compatible = "kddi,quastation-rtd1295-cpu-clock" },
	{}
};
MODULE_DEVICE_TABLE(of, rtd129x_cpu_clk_of_match);

static struct platform_driver rtd129x_cpu_clk_driver = {
	.probe = rtd129x_cpu_clk_probe,
	.driver = {
		.name = "quastation-rtd1295-cpu-clock",
		.of_match_table = rtd129x_cpu_clk_of_match,
	},
};
module_platform_driver(rtd129x_cpu_clk_driver);

MODULE_DESCRIPTION("Qua Station RTD1295 SCPU clock");
MODULE_LICENSE("GPL");
