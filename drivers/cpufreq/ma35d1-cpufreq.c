// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Nuvoton technology corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;version 2 of the License.
 *
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <soc/nuvoton/ma35d1_sip.h>

#define TRANSITION_LATENCY	(10 * 1000)	/* 10 us */

static struct cpufreq_frequency_table ma35d1_freq_table[] = {
	{0, 0x00000364, 800000},
	{0, 0x000006AF, 700000},
	{0, 0x00001396, 600000},
	{0, 0x0000137D, 500000},
	{0, 0x0000237D, 250000},
	{0, 0x0000337D, 125000},
	{0, 0, CPUFREQ_TABLE_END},
};

static struct cpufreq_frequency_table ma35d05k_freq_table[] = {
	{ 0, 0x000006A2, 650000 },
	{ 0, 0x00001396, 600000 },
	{ 0, 0x0000137D, 500000 },
	{ 0, 0x0000237D, 250000 },
	{ 0, 0x0000337D, 125000 },
	{ 0, 0, CPUFREQ_TABLE_END },
};

struct ma35d1_cpufreq_soc_data {
	struct cpufreq_frequency_table *freq_table;
};

static const struct ma35d1_cpufreq_soc_data ma35d1_soc_data = {
	.freq_table = ma35d1_freq_table,
};

static const struct ma35d1_cpufreq_soc_data ma35d05k_soc_data = {
	.freq_table = ma35d05k_freq_table,
};

static struct regmap *clk_regmap;
static const struct ma35d1_cpufreq_soc_data *soc_data;

static int ma35d1_cpufreq_set_target(struct cpufreq_policy *policy, unsigned int index)
{
	unsigned int freq;
	struct arm_smccc_res res;

	freq = soc_data->freq_table[index].frequency / 1000;
	arm_smccc_smc(MA35D1_SIP_CPU_CLK, freq, 0, 0, 0, 0, 0, 0, &res);
	return res.a0 ? -EIO : 0;
}

static unsigned int ma35d1_cpufreq_get(unsigned int cpu)
{
	struct cpufreq_frequency_table *freq_table = soc_data->freq_table;
	u32 capll, idx;
	int ret;

	ret = regmap_read(clk_regmap, 0x60, &capll);
	if (ret)
		return 0;
	// printk("%s - capll is 0x%x\n", __func__, capll);

	for (idx = 0; freq_table[idx].frequency != CPUFREQ_TABLE_END; idx++) {
		if (capll == freq_table[idx].driver_data)
			return freq_table[idx].frequency;
	}

	return 0;
}

static int ma35d1_cpufreq_init(struct cpufreq_policy *policy)
{
	cpufreq_generic_init(policy, soc_data->freq_table, TRANSITION_LATENCY);
	return 0;
}

static void ma35d1_cpufreq_exit(struct cpufreq_policy *policy)
{
}

static struct cpufreq_driver ma35d1_driver = {
	.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_NO_AUTO_DYNAMIC_SWITCHING,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = ma35d1_cpufreq_set_target,
	.get = ma35d1_cpufreq_get,
	.init = ma35d1_cpufreq_init,
	.exit = ma35d1_cpufreq_exit,
	.name = "ma35d1-cpufreq",
};

static int ma35d1_cpufreq_probe(struct platform_device *pdev)
{
	int ret;

	soc_data = of_device_get_match_data(&pdev->dev);
	if (!soc_data)
		return -EINVAL;

	clk_regmap = syscon_regmap_lookup_by_compatible("nuvoton,ma35d1-clk");
	if (IS_ERR(clk_regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk_regmap),
				     "failed to get clock regmap\n");

	ret = cpufreq_register_driver(&ma35d1_driver);
	if (ret)
		dev_err(&pdev->dev, "failed to register ma35d1 cpufreq driver\n");

	return ret;
}

static void ma35d1_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&ma35d1_driver);
}

static const struct of_device_id ma35d1_cpufreq_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-cpufreq", .data = &ma35d1_soc_data },
	{ .compatible = "nuvoton,ma35d05k-cpufreq", .data = &ma35d05k_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ma35d1_cpufreq_of_match);

static struct platform_driver ma35d1_cpufreq_platdrv = {
	.driver = {
		.name	= "ma35d1-cpufreq",
		.of_match_table = ma35d1_cpufreq_of_match,
	},
	.probe		= ma35d1_cpufreq_probe,
	.remove		= ma35d1_cpufreq_remove,
};

module_platform_driver(ma35d1_cpufreq_platdrv);

MODULE_DESCRIPTION("Nuvoton MA35D1 CPUFreq driver");
MODULE_LICENSE("GPL");
