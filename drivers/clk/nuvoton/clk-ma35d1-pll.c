// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35D1 Clock PLL driver
 *
 * Copyright (C) 2022 Nuvoton Technology Corp.
 *
 * Author: Chi-Fang Li <cfli0@nuvoton.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/bitfield.h>

#include "clk-ma35d1.h"

#define to_ma35d1_clk_pll(_hw) \
	(container_of(_hw, struct ma35d1_clk_pll, hw))

#define PLL0CTL0_FBDIV_MSK	GENMASK(7, 0)
#define PLL0CTL0_INDIV_MSK	GENMASK(11, 8)
#define PLL0CTL0_OUTDIV_MSK	GENMASK(13, 12)
#define PLL0CTL0_PD_MSK		BIT(16)
#define PLL0CTL0_BP_MSK		BIT(17)
#define PLLXCTL0_FBDIV_MSK	GENMASK(10, 0)
#define PLLXCTL0_INDIV_MSK	GENMASK(17, 12)
#define PLLXCTL0_MODE_MSK	GENMASK(19, 18)
#define PLLXCTL0_SSRATE_MSK	GENMASK(30, 20)
#define PLLXCTL1_PD_MSK		BIT(0)
#define PLLXCTL1_BP_MSK		BIT(1)
#define PLLXCTL1_OUTDIV_MSK	GENMASK(6, 4)
#define PLLXCTL1_FRAC_MSK	GENMASK(31, 8)
#define PLLXCTL2_SLOPE_MSK	GENMASK(23, 0)

struct ma35d1_clk_pll {
	struct clk_hw hw;
	u8 type;
	u8 mode;
	unsigned long rate;
	void __iomem *ctl0_base;
	void __iomem *ctl1_base;
	void __iomem *ctl2_base;
	struct regmap *regmap;
};

struct vsipll_freq_conf_reg_tbl {
	unsigned long freq;
	u8 mode;
	u32 ctl0_reg;
	u32 ctl1_reg;
	u32 ctl2_reg;
};

static const struct vsipll_freq_conf_reg_tbl ma35d1pll_freq[] = {
	{ 1000000000, VSIPLL_INTEGER_MODE, 0x307d, 0x10, 0 },
	{ 884736000, VSIPLL_FRACTIONAL_MODE, 0x41024, 0xdd2f1b11, 0 },
	{ 533000000, VSIPLL_SS_MODE, 0x12b8102c, 0x6aaaab20, 0x12317 },
	{ }
};

static void clk_unlock_reg(struct ma35d1_clk_pll *pll)
{
	int ret;

	/* Unlock PLL registers */
	do {
		regmap_write(pll->regmap, REG_SYS_RLKTZNS, 0x59);
		regmap_write(pll->regmap, REG_SYS_RLKTZNS, 0x16);
		regmap_write(pll->regmap, REG_SYS_RLKTZNS, 0x88);
		regmap_read(pll->regmap, REG_SYS_RLKTZNS, &ret);
	} while (ret == 0);
}

static void clk_lock_reg(struct ma35d1_clk_pll *pll)
{
	/* Lock PLL registers */
	regmap_write(pll->regmap, REG_SYS_RLKTZNS, 0x0);
}

/* SMIC PLL for CAPLL */
static unsigned long clk_get_pll_freq_smicpll(struct ma35d1_clk_pll *pll,
					      unsigned long pll_src_clk)
{
	u32 m, n, p, out_div;
	u32 val;
	unsigned long pll_clk;
	u32 clk_div_table[] = { 1, 2, 4, 8 };

	val = __raw_readl(pll->ctl0_base);

	n = FIELD_GET(PLL0CTL0_FBDIV_MSK, val);
	m = FIELD_GET(PLL0CTL0_INDIV_MSK, val);
	p = FIELD_GET(PLL0CTL0_OUTDIV_MSK, val);
	out_div = clk_div_table[p];

	if (val & PLL0CTL0_BP_MSK) {
		pll_clk = pll_src_clk;
	} else {
		pll_clk = pll_src_clk * n;
		do_div(pll_clk, m * out_div);
	}

	return pll_clk;
}

/* VSI-PLL: INTEGER_MODE */
static unsigned long clk_cal_pll_freq_mode0(unsigned long pll_src_clk,
					    unsigned long pll_freq, u32 *reg)
{
	u32 tmp_m, tmp_n, tmp_p;
	u32 rng_min_n, rng_min_m, rng_min_p;
	u32 rng_max_n, rng_max_m, rng_max_p;
	u32 tmp, min, min_n, min_m, min_p;
	unsigned long pll_clk;
	unsigned long con1, con2, con3;

	pll_clk = 0;
	min = (u32)-1;

	if (!(pll_freq >= VSIPLL_FCLKO_MIN_FREQ &&
	      pll_freq <= VSIPLL_FCLKO_MAX_FREQ)) {
		reg[0] = ma35d1pll_freq[0].ctl0_reg;
		reg[1] = ma35d1pll_freq[0].ctl1_reg;
		pll_clk = ma35d1pll_freq[0].freq;

		return pll_clk;
	}

	rng_min_m = 1UL;
	rng_max_m = 63UL;

	rng_min_m = ((pll_src_clk / VSIPLL_FREFDIVM_MAX_FREQ) > 1) ?
			(pll_src_clk / VSIPLL_FREFDIVM_MAX_FREQ) : 1;
	rng_max_m = ((pll_src_clk / VSIPLL_FREFDIVM_MIN_FREQ0) < rng_max_m) ?
			(pll_src_clk / VSIPLL_FREFDIVM_MIN_FREQ0) : rng_max_m;

	for (tmp_m = rng_min_m; tmp_m < (rng_max_m + 1); tmp_m++) {
		con1 = pll_src_clk / tmp_m;

		rng_min_n = 16UL;
		rng_max_n = 2047UL;

		rng_min_n = ((VSIPLL_FCLK_MIN_FREQ / con1) > rng_min_n) ?
				(VSIPLL_FCLK_MIN_FREQ / con1) : rng_min_n;
		rng_max_n = ((VSIPLL_FCLK_MAX_FREQ / con1) < rng_max_n) ?
				(VSIPLL_FCLK_MAX_FREQ / con1) : rng_max_n;

		for (tmp_n = rng_min_n; tmp_n < (rng_max_n + 1); tmp_n++) {
			con2 = con1 * tmp_n;

			rng_min_p = 1UL;
			rng_max_p = 7UL;

			rng_min_p = ((con2 / VSIPLL_FCLKO_MAX_FREQ) > 1) ?
					(con2 / VSIPLL_FCLKO_MAX_FREQ) : 1;
			rng_max_p = ((con2 / VSIPLL_FCLKO_MIN_FREQ) < rng_max_p) ?
					(con2 / VSIPLL_FCLKO_MIN_FREQ) : rng_max_p;
			for (tmp_p = rng_min_p; tmp_p < (rng_max_p + 1); tmp_p++) {
				con3 = con2 / tmp_p;
				if (con3 > pll_freq)
					tmp = con3 - pll_freq;
				else
					tmp = pll_freq - con3;

				if (tmp < min) {
					min = tmp;
					min_m = tmp_m;
					min_n = tmp_n;
					min_p = tmp_p;

					if (min == 0UL) {
						reg[0] = (min_m << 12) | min_n;
						reg[1] = min_p << 4;
						return (pll_src_clk * min_n) / (min_p * min_m);
					}
				}
			}
		}
	}

	reg[0] = (min_m << 12) | min_n;
	reg[1] = min_p << 4;
	pll_clk = (pll_src_clk * min_n) / (min_p * min_m);
	return pll_clk;
}

static unsigned long
ma35d1_pll_frac_mode_find_closest(struct ma35d1_clk_pll *pll,
				  unsigned long rate, unsigned long parent_rate,
				  u32 *reg_ctl, unsigned long *freq)
{
	unsigned long min_diff = ULONG_MAX;
	unsigned long fref, fclko_expect, fclko_best;
	int p, m, n, p_best, m_best, n_best;

	fref = parent_rate / 1000;
	fclko_expect = rate / 1000;

	for (m = 2; m <= INDIV_MAX; m++) {
		for (n = 32; n <= FBDIV_MAX; n++) {
			for (p = 1; p <= OUTDIV_MAX; p++) {
				unsigned long fout, fclk, diff;

				fclk = (fref * n) / m;
				if ((fclk < (VSIPLL_FCLK_MIN_FREQ / 1000) ||
				     (fclk > VSIPLL_FCLK_MAX_FREQ / 1000)))
					continue; /* constrain */

				fout = fclk / p;
				diff = abs(fclko_expect - fout);

				if (diff < min_diff) {
					min_diff = diff;
					p_best = p;
					m_best = m;
					n_best = n;
					fclko_best = fout;
					if (min_diff == 0)
						goto search_done;
				}
			}
		}
	}
search_done:
	pr_info("p_best = %d m_best = %d n_best = %d, fclko_best = %ld\n",
		p_best, m_best, n_best, fclko_best);

	if (pll->mode == VSIPLL_SS_MODE) {
		unsigned long ssrate, slope;

		ssrate = (fref / m) / ((VSIPLL_MODULATION_FREQ / 1000) * 2) - 1;
		slope = ((u64)n_best * VSIPLL_SPREAD_RANGE * (1ULL << 24) +
			(ssrate * 10000) / 2) / (ssrate * 10000);
		pr_info("ssrate = 0x%lx slope = 0x%lx\n", ssrate, slope);
		reg_ctl[0] = FIELD_PREP(PLLXCTL0_SSRATE_MSK, ssrate) |
			     FIELD_PREP(PLLXCTL0_MODE_MSK, VSIPLL_SS_MODE) |
			     FIELD_PREP(PLLXCTL0_INDIV_MSK, m_best) | n_best;
		reg_ctl[1] = FIELD_PREP(PLLXCTL1_OUTDIV_MSK, p_best);
		reg_ctl[2] = FIELD_PREP(PLLXCTL2_SLOPE_MSK, slope);
	} else {   /* VSIPLL_FRACTIONAL_MODE */
		reg_ctl[0] = FIELD_PREP(PLLXCTL0_MODE_MSK, VSIPLL_FRACTIONAL_MODE) |
			     FIELD_PREP(PLLXCTL0_INDIV_MSK, m_best) | n_best;
		reg_ctl[1] = FIELD_PREP(PLLXCTL1_OUTDIV_MSK, p_best);
		reg_ctl[2] = 0;
	}

	return fclko_best * 1000;
}

static unsigned long clk_set_pll_freq(struct ma35d1_clk_pll *pll,
				      unsigned long pll_src_clk,
				      unsigned long pll_freq)
{
	u32 reg[3] = { 0 }, val_ctl0, val_ctl1, val_ctl2;
	unsigned long pll_clk;

	val_ctl0 = __raw_readl(pll->ctl0_base);
	val_ctl1 = __raw_readl(pll->ctl1_base);
	val_ctl2 = __raw_readl(pll->ctl2_base);

	switch (pll->mode) {
	case VSIPLL_INTEGER_MODE:
		pll_clk = clk_cal_pll_freq_mode0(pll_src_clk, pll_freq, reg);
		val_ctl0 = reg[0] | (VSIPLL_INTEGER_MODE << VSIPLLCTL0_MODE_POS);
		break;
	case VSIPLL_FRACTIONAL_MODE:
	case VSIPLL_SS_MODE:
		pll_clk = ma35d1_pll_frac_mode_find_closest(pll, pll_freq,
							    pll_src_clk, reg,
							    &pll_clk);
		val_ctl0 = reg[0];
		break;
	}

	/*
	 * Keep the current power state: a PLL that is already running (prepare
	 * count held by others, e.g. VPLL) must not be left powered down.
	 */
	val_ctl1 = (val_ctl1 & VSIPLLCTL1_PD_MSK) | reg[1];
	val_ctl2 = reg[2];

	__raw_writel(val_ctl0, pll->ctl0_base);
	__raw_writel(val_ctl1, pll->ctl1_base);
	__raw_writel(val_ctl2, pll->ctl2_base);

	return pll_clk;
}

static unsigned long clk_get_pll_freq_vsipll(struct ma35d1_clk_pll *pll,
					     unsigned long pll_src_clk)
{
	u32 m, n, p, x;
	u32 val_ctl0, val_ctl1;
	unsigned long pll_clk, x64;

	val_ctl0 = __raw_readl(pll->ctl0_base);
	val_ctl1 = __raw_readl(pll->ctl1_base);

	if (val_ctl1 & PLLXCTL1_BP_MSK) {
		pll_clk = pll_src_clk;
		return pll_clk;
	}

	n = FIELD_GET(PLLXCTL0_FBDIV_MSK, val_ctl0);
	m = FIELD_GET(PLLXCTL0_INDIV_MSK, val_ctl0);
	p = FIELD_GET(PLLXCTL1_OUTDIV_MSK, val_ctl1);

	if (pll->mode == VSIPLL_INTEGER_MODE) {
		pll_clk = pll_src_clk * n;
		do_div(pll_clk, m * p);

	} else if (pll->mode == VSIPLL_FRACTIONAL_MODE) {
		x = FIELD_GET(PLLXCTL1_FRAC_MSK, val_ctl1);

		x64 = (u64)x;
		x64 = ((x64 * 1000) + 500) >> 24;
		pll_clk = (pll_src_clk * ((n * 1000) + x64)) / 1000 / p / m;

	} else {
		x = FIELD_GET(PLLXCTL1_FRAC_MSK, val_ctl1);

		x64 = (u64)x;
		x64 = (x64 * 1000) >> 24;

		pll_clk = (pll_src_clk * ((n * 1000) + x64)) / 1000 / p / m;
	}

	return pll_clk;
}

static int ma35d1_clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct ma35d1_clk_pll *pll = to_ma35d1_clk_pll(hw);

	if (parent_rate < VSIPLL_FREF_MIN_FREQ || parent_rate > VSIPLL_FREF_MAX_FREQ)
		return 0;

	if (pll->type == MA35D1_CAPLL || pll->type == MA35D1_DDRPLL) {
		pr_warn("Nuvoton MA35D1 CAPLL/DDRPLL Read Only.\n");
		return 0;
	}

	clk_unlock_reg(pll);
	pll->rate = clk_set_pll_freq(pll, parent_rate, rate);
	clk_lock_reg(pll);

	return 0;
}

static unsigned long ma35d1_clk_pll_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	unsigned long pllfreq;
	struct ma35d1_clk_pll *pll = to_ma35d1_clk_pll(hw);

	if (parent_rate < VSIPLL_FREF_MIN_FREQ ||
	    parent_rate > VSIPLL_FREF_MAX_FREQ)
		return 0;

	switch (pll->type) {
	case MA35D1_CAPLL:
		pllfreq = clk_get_pll_freq_smicpll(pll, parent_rate);
		break;
	case MA35D1_DDRPLL:
	case MA35D1_APLL:
	case MA35D1_EPLL:
	case MA35D1_VPLL:
		pllfreq = clk_get_pll_freq_vsipll(pll, parent_rate);
		break;
	}

	return pllfreq;
}

static long ma35d1_clk_pll_round_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long *prate)
{
	return rate;
}

static int ma35d1_clk_pll_is_prepared(struct clk_hw *hw)
{
	struct ma35d1_clk_pll *pll = to_ma35d1_clk_pll(hw);

	u32 val = __raw_readl(pll->ctl1_base);

	return (val & VSIPLLCTL1_PD_MSK) ? 0 : 1;
}

static int ma35d1_clk_pll_prepare(struct clk_hw *hw)
{
	struct ma35d1_clk_pll *pll = to_ma35d1_clk_pll(hw);
	u32 val;

	if (pll->type == MA35D1_CAPLL || pll->type == MA35D1_DDRPLL) {
		pr_warn("Nuvoton MA35D1 CAPLL/DDRPLL Enable Only.\n");
		return 0;
	}

	clk_unlock_reg(pll);
	val = __raw_readl(pll->ctl1_base);
	val &= ~VSIPLLCTL1_PD_MSK;
	__raw_writel(val, pll->ctl1_base);
	clk_lock_reg(pll);

	return 0;
}

static void ma35d1_clk_pll_unprepare(struct clk_hw *hw)
{
	struct ma35d1_clk_pll *pll = to_ma35d1_clk_pll(hw);
	u32 val;

	if (pll->type == MA35D1_CAPLL || pll->type == MA35D1_DDRPLL) {
		pr_warn("Nuvoton MA35D1 CAPLL/DDRPLL Cannot disable.\n");
	} else {
		val = __raw_readl(pll->ctl1_base);
		val |= VSIPLLCTL1_PD_MSK;
		__raw_writel(val, pll->ctl1_base);
	}
}

static const struct clk_ops ma35d1_clk_pll_ops = {
	.is_prepared = ma35d1_clk_pll_is_prepared,
	.prepare = ma35d1_clk_pll_prepare,
	.unprepare = ma35d1_clk_pll_unprepare,
	.set_rate = ma35d1_clk_pll_set_rate,
	.recalc_rate = ma35d1_clk_pll_recalc_rate,
	.round_rate = ma35d1_clk_pll_round_rate,
};

struct clk_hw *ma35d1_reg_clk_pll(enum ma35d1_pll_type type,
				  u8 u8mode, const char *name,
				  const char *parent, unsigned long target_freq,
				  void __iomem *base, struct regmap *regmap)
{
	struct ma35d1_clk_pll *pll;
	struct clk_hw *hw;
	struct clk_init_data init;
	int ret;

	pll = kmalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	pll->type = type;
	pll->mode = u8mode;
	pll->rate = target_freq;
	pll->ctl0_base = base + VSIPLL_CTL0;
	pll->ctl1_base = base + VSIPLL_CTL1;
	pll->ctl2_base = base + VSIPLL_CTL2;
	pll->regmap = regmap;

	init.name = name;
	init.flags = 0;
	init.parent_names = &parent;
	init.num_parents = 1;

	init.ops = &ma35d1_clk_pll_ops;
	pll->hw.init = &init;

	hw = &pll->hw;

	ret = clk_hw_register(NULL, hw);
	if (ret) {
		pr_err("failed to register vsi-pll clock!!!\n");
		kfree(pll);
		return ERR_PTR(ret);
	}

	return hw;
}
