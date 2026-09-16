// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nuvoton MA35D1 I2S controller driver
 *
 * Copyright (c) 2026 Nuvoton Technology Corp.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_data/dma-ma35d1.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/spinlock.h>

#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define MA35D1_I2S_CTL0		0x00
#define MA35D1_I2S_CLKDIV	0x04
#define MA35D1_I2S_IEN		0x08
#define MA35D1_I2S_TXFIFO	0x10
#define MA35D1_I2S_RXFIFO	0x14
#define MA35D1_I2S_CTL1		0x20

#define MA35D1_I2S_CTL0_CHWIDTH_MASK	GENMASK(29, 28)
#define MA35D1_I2S_CTL0_CHWIDTH_16	FIELD_PREP(MA35D1_I2S_CTL0_CHWIDTH_MASK, 1)
#define MA35D1_I2S_CTL0_CHWIDTH_24	FIELD_PREP(MA35D1_I2S_CTL0_CHWIDTH_MASK, 2)
#define MA35D1_I2S_CTL0_CHWIDTH_32	FIELD_PREP(MA35D1_I2S_CTL0_CHWIDTH_MASK, 3)
#define MA35D1_I2S_CTL0_FORMAT_MASK	GENMASK(26, 24)
#define MA35D1_I2S_CTL0_FORMAT_I2S	FIELD_PREP(MA35D1_I2S_CTL0_FORMAT_MASK, 0)
#define MA35D1_I2S_CTL0_FORMAT_MSB	FIELD_PREP(MA35D1_I2S_CTL0_FORMAT_MASK, 1)
#define MA35D1_I2S_CTL0_FORMAT_LSB	FIELD_PREP(MA35D1_I2S_CTL0_FORMAT_MASK, 2)
#define MA35D1_I2S_CTL0_RXLCH		BIT(23)
#define MA35D1_I2S_CTL0_RXPDMAEN	BIT(21)
#define MA35D1_I2S_CTL0_TXPDMAEN	BIT(20)
#define MA35D1_I2S_CTL0_RXFBCLR		BIT(19)
#define MA35D1_I2S_CTL0_TXFBCLR		BIT(18)
#define MA35D1_I2S_CTL0_MCLKEN		BIT(15)
#define MA35D1_I2S_CTL0_SLAVE		BIT(8)
#define MA35D1_I2S_CTL0_ORDER		BIT(7)
#define MA35D1_I2S_CTL0_MONO		BIT(6)
#define MA35D1_I2S_CTL0_DATWIDTH_MASK	GENMASK(5, 4)
#define MA35D1_I2S_CTL0_DATWIDTH_16	FIELD_PREP(MA35D1_I2S_CTL0_DATWIDTH_MASK, 1)
#define MA35D1_I2S_CTL0_DATWIDTH_24	FIELD_PREP(MA35D1_I2S_CTL0_DATWIDTH_MASK, 2)
#define MA35D1_I2S_CTL0_DATWIDTH_32	FIELD_PREP(MA35D1_I2S_CTL0_DATWIDTH_MASK, 3)
#define MA35D1_I2S_CTL0_RXEN		BIT(2)
#define MA35D1_I2S_CTL0_TXEN		BIT(1)
#define MA35D1_I2S_CTL0_I2SEN		BIT(0)

#define MA35D1_I2S_CLKDIV_BCLK_MASK	GENMASK(17, 8)
#define MA35D1_I2S_CLKDIV_MCLK_MASK	GENMASK(6, 0)

#define MA35D1_I2S_CTL1_PB16ORD		BIT(25)
#define MA35D1_I2S_CTL1_PBWIDTH		BIT(24)

#define MA35D1_I2S_DEFAULT_MCLK		12000000U
#define MA35D1_I2S_FRAME_SLOTS		2U

#define MA35D1_I2S_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S24_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

struct ma35d1_i2s {
	void __iomem *base;
	struct clk *clk;
	struct device *dev;
	spinlock_t lock;

	struct snd_dmaengine_dai_dma_data dma_tx;
	struct snd_dmaengine_dai_dma_data dma_rx;
	struct ma35d1_peripheral pdma_tx;
	struct ma35d1_peripheral pdma_rx;

	unsigned int mclk_rate;
	unsigned int active_streams;
	unsigned int configured_streams;
	snd_pcm_format_t configured_format;
	bool is_master;
};

static u32 ma35d1_i2s_read(struct ma35d1_i2s *i2s, unsigned int reg)
{
	return readl(i2s->base + reg);
}

static void ma35d1_i2s_write(struct ma35d1_i2s *i2s, unsigned int reg,
			     u32 value)
{
	writel(value, i2s->base + reg);
}

static void ma35d1_i2s_update_bits(struct ma35d1_i2s *i2s, unsigned int reg,
				   u32 mask, u32 value)
{
	u32 tmp;

	tmp = ma35d1_i2s_read(i2s, reg);
	tmp = (tmp & ~mask) | (value & mask);
	ma35d1_i2s_write(i2s, reg, tmp);
}

static int ma35d1_i2s_read_reqsel(struct device *dev, const char *property,
				  const char *legacy_property, u32 *value)
{
	int ret;

	ret = device_property_read_u32(dev, property, value);
	if (ret)
		ret = device_property_read_u32(dev, legacy_property, value);

	return ret;
}

static int ma35d1_i2s_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				 unsigned int freq, int dir)
{
	struct ma35d1_i2s *i2s = dev_get_drvdata(dai->dev);

	if (clk_id != 0 || dir != SND_SOC_CLOCK_OUT)
		return -EINVAL;

	if (freq && freq != MA35D1_I2S_DEFAULT_MCLK)
		return -EINVAL;

	i2s->mclk_rate = freq ? freq : MA35D1_I2S_DEFAULT_MCLK;

	return 0;
}

static int ma35d1_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct ma35d1_i2s *i2s = dev_get_drvdata(dai->dev);
	unsigned long flags;
	u32 value;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		value = MA35D1_I2S_CTL0_FORMAT_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		value = MA35D1_I2S_CTL0_FORMAT_MSB;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		value = MA35D1_I2S_CTL0_FORMAT_LSB;
		break;
	default:
		return -EINVAL;
	}

	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		i2s->is_master = true;
		break;
	case SND_SOC_DAIFMT_BC_FC:
		i2s->is_master = false;
		value |= MA35D1_I2S_CTL0_SLAVE;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&i2s->lock, flags);
	ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL0,
				 MA35D1_I2S_CTL0_FORMAT_MASK |
				 MA35D1_I2S_CTL0_SLAVE,
				 value);
	spin_unlock_irqrestore(&i2s->lock, flags);

	return 0;
}

static int ma35d1_i2s_calc_clkdiv(struct ma35d1_i2s *i2s,
				  unsigned int rate,
				  unsigned int channel_width,
				  u32 *clkdiv)
{
	unsigned long parent_rate;
	unsigned long bclk_div = 0;
	unsigned long mclk_div;
	u64 bclk;

	parent_rate = clk_get_rate(i2s->clk);
	if (!parent_rate || !i2s->mclk_rate)
		return -EINVAL;

	if (parent_rate == i2s->mclk_rate) {
		mclk_div = 0;
	} else {
		mclk_div = DIV_ROUND_CLOSEST_ULL(parent_rate,
						 2ULL * i2s->mclk_rate);
		if (!mclk_div)
			return -EINVAL;
	}

	if (i2s->is_master) {
		bclk = (u64)rate * MA35D1_I2S_FRAME_SLOTS * channel_width;
		bclk_div = DIV_ROUND_CLOSEST_ULL(parent_rate, 2ULL * bclk);
		if (!bclk_div)
			return -EINVAL;
		bclk_div--;
	}

	if (bclk_div > FIELD_MAX(MA35D1_I2S_CLKDIV_BCLK_MASK) ||
	    mclk_div > FIELD_MAX(MA35D1_I2S_CLKDIV_MCLK_MASK))
		return -EINVAL;

	*clkdiv = FIELD_PREP(MA35D1_I2S_CLKDIV_BCLK_MASK, bclk_div) |
		  FIELD_PREP(MA35D1_I2S_CLKDIV_MCLK_MASK, mclk_div);

	return 0;
}

static int ma35d1_i2s_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct ma35d1_i2s *i2s = dev_get_drvdata(dai->dev);
	struct snd_dmaengine_dai_dma_data *dma_data;
	unsigned int stream_bit = BIT(substream->stream);
	unsigned int channels = params_channels(params);
	unsigned int channel_width;
	enum dma_slave_buswidth dma_width;
	snd_pcm_format_t format = params_format(params);
	unsigned long flags;
	u32 ctl0;
	u32 ctl1;
	u32 clkdiv;
	int ret;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		ctl0 = MA35D1_I2S_CTL0_DATWIDTH_16 |
		       MA35D1_I2S_CTL0_CHWIDTH_16 |
		       MA35D1_I2S_CTL0_ORDER;
		ctl1 = MA35D1_I2S_CTL1_PBWIDTH;
		channel_width = 16;
		dma_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		ctl0 = MA35D1_I2S_CTL0_DATWIDTH_24 |
		       MA35D1_I2S_CTL0_CHWIDTH_24;
		ctl1 = 0;
		channel_width = 24;
		dma_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;
	case SNDRV_PCM_FORMAT_S32_LE:
		ctl0 = MA35D1_I2S_CTL0_DATWIDTH_32 |
		       MA35D1_I2S_CTL0_CHWIDTH_32;
		ctl1 = 0;
		channel_width = 32;
		dma_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;
	default:
		return -EINVAL;
	}

	if (channels == 1)
		ctl0 |= MA35D1_I2S_CTL0_MONO | MA35D1_I2S_CTL0_RXLCH;
	else if (channels != 2)
		return -EINVAL;

	ret = ma35d1_i2s_calc_clkdiv(i2s, params_rate(params),
				     channel_width, &clkdiv);
	if (ret) {
		dev_err(i2s->dev,
			"cannot derive clocks for %u Hz, %u-bit channels and %u Hz MCLK\n",
			params_rate(params), channel_width, i2s->mclk_rate);
		return ret;
	}

	spin_lock_irqsave(&i2s->lock, flags);
	if ((i2s->configured_streams & ~stream_bit) &&
	    i2s->configured_format != format) {
		spin_unlock_irqrestore(&i2s->lock, flags);
		return -EINVAL;
	}

	dma_data = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ?
		   &i2s->dma_tx : &i2s->dma_rx;
	dma_data->addr_width = dma_width;
	i2s->configured_format = format;
	i2s->configured_streams |= stream_bit;

	ma35d1_i2s_write(i2s, MA35D1_I2S_CLKDIV, clkdiv);
	ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL0,
				 MA35D1_I2S_CTL0_DATWIDTH_MASK |
				 MA35D1_I2S_CTL0_CHWIDTH_MASK |
				 MA35D1_I2S_CTL0_RXLCH |
				 MA35D1_I2S_CTL0_ORDER |
				 MA35D1_I2S_CTL0_MONO,
				 ctl0);
	ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL1,
				 MA35D1_I2S_CTL1_PB16ORD |
				 MA35D1_I2S_CTL1_PBWIDTH,
				 ctl1);
	spin_unlock_irqrestore(&i2s->lock, flags);

	return 0;
}

static int ma35d1_i2s_hw_free(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct ma35d1_i2s *i2s = dev_get_drvdata(dai->dev);
	unsigned long flags;

	spin_lock_irqsave(&i2s->lock, flags);
	i2s->configured_streams &= ~BIT(substream->stream);
	spin_unlock_irqrestore(&i2s->lock, flags);

	return 0;
}

static int ma35d1_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
			      struct snd_soc_dai *dai)
{
	struct ma35d1_i2s *i2s = dev_get_drvdata(dai->dev);
	unsigned int stream_bit = BIT(substream->stream);
	unsigned long flags;
	u32 clear;
	u32 enable;
	u32 value;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		clear = MA35D1_I2S_CTL0_TXFBCLR;
		enable = MA35D1_I2S_CTL0_TXEN | MA35D1_I2S_CTL0_TXPDMAEN;
	} else {
		clear = MA35D1_I2S_CTL0_RXFBCLR;
		enable = MA35D1_I2S_CTL0_RXEN | MA35D1_I2S_CTL0_RXPDMAEN;
	}

	spin_lock_irqsave(&i2s->lock, flags);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL0, clear, clear);

		i2s->active_streams |= stream_bit;
		ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL0,
					 enable | MA35D1_I2S_CTL0_MCLKEN |
					 MA35D1_I2S_CTL0_I2SEN,
					 enable | MA35D1_I2S_CTL0_MCLKEN |
					 MA35D1_I2S_CTL0_I2SEN);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		i2s->active_streams &= ~stream_bit;
		value = i2s->active_streams ? 0 :
			MA35D1_I2S_CTL0_I2SEN | MA35D1_I2S_CTL0_MCLKEN;
		ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL0,
					 enable | value, 0);
		break;

	default:
		spin_unlock_irqrestore(&i2s->lock, flags);
		return -EINVAL;
	}

	spin_unlock_irqrestore(&i2s->lock, flags);

	return 0;
}

static int ma35d1_i2s_dai_probe(struct snd_soc_dai *dai)
{
	struct ma35d1_i2s *i2s = dev_get_drvdata(dai->dev);

	snd_soc_dai_init_dma_data(dai, &i2s->dma_tx, &i2s->dma_rx);

	return 0;
}

static const struct snd_soc_dai_ops ma35d1_i2s_dai_ops = {
	.probe = ma35d1_i2s_dai_probe,
	.set_sysclk = ma35d1_i2s_set_sysclk,
	.set_fmt = ma35d1_i2s_set_fmt,
	.hw_params = ma35d1_i2s_hw_params,
	.hw_free = ma35d1_i2s_hw_free,
	.trigger = ma35d1_i2s_trigger,
};

static struct snd_soc_dai_driver ma35d1_i2s_dai = {
	.name = "ma35d1-i2s",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = MA35D1_I2S_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = MA35D1_I2S_FORMATS,
	},
	.ops = &ma35d1_i2s_dai_ops,
	.symmetric_rate = 1,
	.symmetric_channels = 1,
	.symmetric_sample_bits = 1,
};

static const struct snd_soc_component_driver ma35d1_i2s_component = {
	.name = "ma35d1-i2s",
};

static const struct snd_pcm_hardware ma35d1_i2s_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_PAUSE |
		SNDRV_PCM_INFO_RESUME,
	.formats = MA35D1_I2S_FORMATS,
	.rates = SNDRV_PCM_RATE_8000_48000,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = 64 * 1024,
	.period_bytes_min = 4 * 1024,
	.period_bytes_max = 8 * 1024,
	.periods_min = 2,
	.periods_max = 16,
};

static const struct snd_dmaengine_pcm_config ma35d1_i2s_pcm_config = {
	.pcm_hardware = &ma35d1_i2s_pcm_hardware,
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
	.prealloc_buffer_size = 32 * 1024,
};

static void ma35d1_i2s_assert_reset(void *data)
{
	reset_control_assert(data);
}

static int ma35d1_i2s_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reset_control *reset;
	struct gpio_desc *powerdown;
	struct ma35d1_i2s *i2s;
	struct resource *res;
	int ret;

	i2s = devm_kzalloc(dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	i2s->dev = dev;
	i2s->mclk_rate = MA35D1_I2S_DEFAULT_MCLK;
	i2s->is_master = true;
	spin_lock_init(&i2s->lock);

	i2s->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(i2s->base))
		return PTR_ERR(i2s->base);

	i2s->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(i2s->clk))
		return dev_err_probe(dev, PTR_ERR(i2s->clk),
				     "failed to get and enable I2S clock\n");

	reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset),
				     "failed to get I2S reset\n");

	if (reset) {
		ret = reset_control_deassert(reset);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to deassert I2S reset\n");

		ret = devm_add_action_or_reset(dev, ma35d1_i2s_assert_reset,
					       reset);
		if (ret)
			return ret;
	}

	ret = ma35d1_i2s_read_reqsel(dev, "nuvoton,pdma-reqsel-tx",
				     "pdma_reqsel_tx", &i2s->pdma_tx.reqsel);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing TX PDMA request selection\n");

	ret = ma35d1_i2s_read_reqsel(dev, "nuvoton,pdma-reqsel-rx",
				     "pdma_reqsel_rx", &i2s->pdma_rx.reqsel);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing RX PDMA request selection\n");

	i2s->dma_tx.addr = res->start + MA35D1_I2S_TXFIFO;
	i2s->dma_tx.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	i2s->dma_tx.maxburst = 1;
	i2s->dma_tx.chan_name = "tx";
	i2s->dma_tx.peripheral_config = &i2s->pdma_tx;
	i2s->dma_tx.peripheral_size = sizeof(i2s->pdma_tx);

	i2s->dma_rx.addr = res->start + MA35D1_I2S_RXFIFO;
	i2s->dma_rx.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	i2s->dma_rx.maxburst = 1;
	i2s->dma_rx.chan_name = "rx";
	i2s->dma_rx.peripheral_config = &i2s->pdma_rx;
	i2s->dma_rx.peripheral_size = sizeof(i2s->pdma_rx);

	platform_set_drvdata(pdev, i2s);

	powerdown = devm_gpiod_get_optional(dev, "powerdown", GPIOD_OUT_LOW);
	if (IS_ERR(powerdown))
		return dev_err_probe(dev, PTR_ERR(powerdown),
				     "failed to configure power-down GPIO\n");

	ma35d1_i2s_write(i2s, MA35D1_I2S_IEN, 0);
	ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL0,
				 MA35D1_I2S_CTL0_RXPDMAEN |
				 MA35D1_I2S_CTL0_TXPDMAEN |
				 MA35D1_I2S_CTL0_RXEN |
				 MA35D1_I2S_CTL0_TXEN |
				 MA35D1_I2S_CTL0_MCLKEN |
				 MA35D1_I2S_CTL0_I2SEN,
				 0);
	ma35d1_i2s_update_bits(i2s, MA35D1_I2S_CTL1,
				 MA35D1_I2S_CTL1_PB16ORD |
				 MA35D1_I2S_CTL1_PBWIDTH,
				 MA35D1_I2S_CTL1_PBWIDTH);

	ret = devm_snd_soc_register_component(dev, &ma35d1_i2s_component,
					      &ma35d1_i2s_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register I2S component\n");

	ret = devm_snd_dmaengine_pcm_register(dev,
					      &ma35d1_i2s_pcm_config, 0);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register DMAengine PCM\n");

	dev_info(dev, "I2S registered, clock %lu Hz, PDMA TX/RX %u/%u\n",
		 clk_get_rate(i2s->clk), i2s->pdma_tx.reqsel,
		 i2s->pdma_rx.reqsel);

	return 0;
}

static const struct of_device_id ma35d1_i2s_of_match[] = {
	{ .compatible = "nuvoton,ma35d0-i2s" },
	{ .compatible = "nuvoton,ma35d1-i2s" },
	{ .compatible = "nuvoton,ma35h0-i2s" },

	{ }
};
MODULE_DEVICE_TABLE(of, ma35d1_i2s_of_match);

static struct platform_driver ma35d1_i2s_driver = {
	.probe = ma35d1_i2s_probe,
	.driver = {
		.name = "ma35d1-i2s",
		.of_match_table = ma35d1_i2s_of_match,
	},
};
module_platform_driver(ma35d1_i2s_driver);

MODULE_AUTHOR("Chi-Wen Weng <cwweng@nuvoton.com>");
MODULE_DESCRIPTION("Nuvoton MA35D1 I2S controller driver");
MODULE_LICENSE("GPL");
