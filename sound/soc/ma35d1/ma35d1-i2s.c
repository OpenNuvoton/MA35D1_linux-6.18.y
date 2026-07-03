// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 Nuvoton technology corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;version 2 of the License.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/suspend.h>
#include <sound/core.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/soc-dai.h>
#include <linux/device.h>
#include <linux/clk.h>
#include <linux/of.h>

#include "ma35d1-pcm.h"
#include "ma35d1-i2s.h"

static DEFINE_MUTEX(i2s_mutex);

static inline void ma35d1_i2s_write_reg(struct ma35d1_i2s_info *info,
                                        unsigned reg, unsigned val)
{
	__raw_writel(val, info->regs + reg);
}

static inline unsigned ma35d1_i2s_read_reg(struct ma35d1_i2s_info *info,
        unsigned reg)
{
	return __raw_readl(info->regs + reg);
}

static int ma35d1_i2s_hw_params(struct snd_pcm_substream *substream,
                                struct snd_pcm_hw_params *params,
                                struct snd_soc_dai *dai)
{
	struct ma35d1_i2s_info *info = dev_get_drvdata(dai->dev);
	unsigned channels = params_channels(params);
	unsigned long val = ma35d1_i2s_read_reg(info, I2S_CTL0);

	//printk("Enter %s.....\n", __FUNCTION__);

	switch (params_width(params)) {
	case 8:
		val = (val & ~DATWIDTH) | DATWIDTH_8;
		val = (val & ~CHWIDTH) | CHWIDTH_8;
		break;

	case 16:
		val = (val & ~DATWIDTH) | DATWIDTH_16;
		val = (val & ~CHWIDTH) | CHWIDTH_16;
		//ctl1 = AUDIO_READ(ma35d1_audio->mmio + I2S_CTL1);
		//ctl1 = ctl1 | PBWIDTH_16;	//set Peripheral Bus Data Width to 16 bit
		//AUDIO_WRITE(ma35d1_audio->mmio + I2S_CTL1, ctl1);
		break;

	case 24:
		val = (val & ~DATWIDTH) | DATWIDTH_24;
		val = (val & ~CHWIDTH) | CHWIDTH_24;
		break;

	case 32:
		val = (val & ~DATWIDTH) | DATWIDTH_32;
		val = (val & ~CHWIDTH) | CHWIDTH_32;
		break;

	default:
		return -EINVAL;
	}

	if (info->raw_pdm_mono && substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		val = PBWIDTH_16 | ORDER;
	} else {
		/* set MONO if channel number is 1 */
		if (channels == 1)
			val |= MONO;
		else
			val &= ~MONO;
	}

	ma35d1_i2s_write_reg(info, I2S_CTL0, val);

	return 0;
}

static int ma35d1_i2s_set_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{
	struct ma35d1_i2s_info *info = dev_get_drvdata(cpu_dai->dev);
	unsigned long val = ma35d1_i2s_read_reg(info, I2S_CTL0);

	//printk("Enter %s.....\n", __FUNCTION__);

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_MSB:
		val |= ORDER; //MSB
		break;
	case SND_SOC_DAIFMT_I2S:
		val &= ~ORDER; //LSB
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		val |= SLAVE; //Slave
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		val &= ~SLAVE; //Master
		break;
	default:
		return -EINVAL;
	}

	ma35d1_i2s_write_reg(info, I2S_CTL0, val);

	return 0;
}

static int ma35d1_i2s_set_sysclk(struct snd_soc_dai *cpu_dai, int clk_id, unsigned int freq, int dir)
{
	struct ma35d1_i2s_info *info = dev_get_drvdata(cpu_dai->dev);
	unsigned int i2s_clk, bitrate, mclkdiv, bclkdiv;

	//printk("Enter %s.....\n", __FUNCTION__);

	i2s_clk = clk_get_rate(info->clk);

	if (info->raw_pdm_mono && cpu_dai->channels == 1)
		bitrate = freq * cpu_dai->sample_bits;
	else if (info->pdm_decimation)
		bitrate = freq * info->pdm_decimation;
	else
		bitrate = freq * 2U * 16U;
	bclkdiv = ((((i2s_clk * 10UL / bitrate) >> 1U) + 5UL) / 10UL) - 1U;


	mclkdiv = (i2s_clk / info->mclk_out) >> 1;

	ma35d1_i2s_write_reg(info, I2S_CLKDIV, (bclkdiv << 8) | mclkdiv);

	return 0;

}

static int ma35d1_i2s_trigger(struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
	struct ma35d1_i2s_info *info = dev_get_drvdata(dai->dev);
	int ret = 0;
	unsigned long val;

	//printk("Enter %s.....\n", __FUNCTION__);

	val = ma35d1_i2s_read_reg(info, I2S_CTL0);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		val |= I2S_EN;
		val |= MCLKEN;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			val |= TX_EN | TXPDMAEN;
		else
			val |= RX_EN | RXPDMAEN;

		ma35d1_i2s_write_reg(info, I2S_CTL0, val);

		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		val &= ~I2S_EN;
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			val &= ~(TX_EN | TXPDMAEN);
		else
			val &= ~(RX_EN | RXPDMAEN);

		ma35d1_i2s_write_reg(info, I2S_CTL0, val);

		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static int ma35d1_i2s_probe(struct snd_soc_dai *dai)
{
	struct ma35d1_i2s_info *info = dev_get_drvdata(dai->dev);

	//printk("Enter %s.....\n", __FUNCTION__);

	mutex_lock(&i2s_mutex);

	/* Init DMA data */
	info->dma_params_rx.addr = info->phyaddr + I2S_RXFIFO;
	info->dma_params_rx.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	info->pcfg_rx.reqsel = info->pdma_reqsel_rx;
	info->dma_params_rx.peripheral_config = &info->pcfg_rx;
	info->dma_params_rx.peripheral_size = sizeof(info->pcfg_rx);
	info->dma_params_tx.addr = info->phyaddr + I2S_TXFIFO;
	info->dma_params_tx.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	info->pcfg_tx.reqsel = info->pdma_reqsel_tx;
	info->dma_params_tx.peripheral_config = &info->pcfg_tx;
	info->dma_params_tx.peripheral_size = sizeof(info->pcfg_tx);

	snd_soc_dai_init_dma_data(dai, &info->dma_params_tx,
	                          &info->dma_params_rx);

	snd_soc_dai_set_drvdata(dai, info);

	/* Set Audio_JKEN pin */
	info->pwdn_gpio = devm_gpiod_get_optional(dai->dev, "powerdown",
	                  GPIOD_OUT_HIGH);
	if (IS_ERR(info->pwdn_gpio))
		return PTR_ERR(info->pwdn_gpio);

	gpiod_set_value_cansleep(info->pwdn_gpio, 0);


	mutex_unlock(&i2s_mutex);

	return 0;
}

static int ma35d1_i2s_remove(struct snd_soc_dai *dai)
{
	struct ma35d1_i2s_info *info = dev_get_drvdata(dai->dev);

	clk_disable(info->clk);

	return 0;
}

static void ma35d1_i2s_enable(struct ma35d1_i2s_info *info, int stream)
{

	unsigned long val = ma35d1_i2s_read_reg(info, I2S_CTL0);

	if ((val & I2S_EN) == 0 ) {
		/* Enable clocks */
		clk_prepare_enable(info->clk);

		/* Enable i2s */
		val |= I2S_EN;
		val |= MCLKEN;

		ma35d1_i2s_write_reg(info, I2S_CTL0, val);
	}
}

static void ma35d1_i2s_disable(struct ma35d1_i2s_info *info, int stream)
{
	unsigned long val = ma35d1_i2s_read_reg(info, I2S_CTL0);

	/* Disable i2s */
	val &= ~I2S_EN;
	/* Disable TX or RX */
	if (stream == SNDRV_PCM_STREAM_PLAYBACK)
		val &= ~(TX_EN | TXPDMAEN);
	else
		val &= ~(RX_EN | RXPDMAEN);

	ma35d1_i2s_write_reg(info, I2S_CTL0, val);

	/* Disable clocks */
	clk_disable_unprepare(info->clk);
}

static int ma35d1_i2s_startup(struct snd_pcm_substream *substream,
                              struct snd_soc_dai *dai)
{
	struct ma35d1_i2s_info *info = snd_soc_dai_get_drvdata(dai);

	//printk("Enter %s.....\n", __FUNCTION__);

	ma35d1_i2s_enable(info, substream->stream);

	return 0;
}

static void ma35d1_i2s_shutdown(struct snd_pcm_substream *substream,
                                struct snd_soc_dai *dai)
{
	struct ma35d1_i2s_info *info = snd_soc_dai_get_drvdata(dai);

	//printk("Enter %s.....\n", __FUNCTION__);

	ma35d1_i2s_disable(info, substream->stream);
}

static struct snd_soc_dai_ops ma35d1_i2s_dai_ops = {
	/*
	.trigger    = ma35d1_i2s_trigger,
	.hw_params  = ma35d1_i2s_hw_params,
	.set_fmt    = ma35d1_i2s_set_fmt,
	.set_sysclk = ma35d1_i2s_set_sysclk,
	*/

	.probe          = ma35d1_i2s_probe,
	.startup        = ma35d1_i2s_startup,
	.shutdown       = ma35d1_i2s_shutdown,
	.hw_params      = ma35d1_i2s_hw_params,
	.set_sysclk     = ma35d1_i2s_set_sysclk,
	.set_fmt        = ma35d1_i2s_set_fmt,
	.trigger        = ma35d1_i2s_trigger,
};

struct snd_soc_dai_driver ma35d1_i2s_dai = {
	.name		= "i2s_pcm",
	/*
	.probe          = ma35d1_i2s_probe,
	.remove         = ma35d1_i2s_remove,
	*/
	.playback = {
		.rates      = SNDRV_PCM_RATE_8000_48000,
		.formats    = SNDRV_PCM_FMTBIT_S16_LE,
		.channels_min   = 1,
		.channels_max   = 2,
	},
	.capture = {
		.rates      = SNDRV_PCM_RATE_8000_192000,
		.formats    = SNDRV_PCM_FMTBIT_S16_LE,
		.channels_min   = 1,
		.channels_max   = 2,
	},
	.ops = &ma35d1_i2s_dai_ops,
};

static const struct snd_soc_component_driver ma35d1_i2s_component = {
	.name       = "ma35d1-i2s",
};


static int ma35d1_i2s_drvprobe(struct platform_device *pdev)
{
	struct ma35d1_i2s_info *info;
	u32	val32[4], mclk_out;
	u32	dma_tx_num, dma_rx_num;

	int ret;

	//printk("Enter %s.....\n", __FUNCTION__);

	info = devm_kzalloc(&pdev->dev, sizeof(struct ma35d1_i2s_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	spin_lock_init(&info->lock);
	spin_lock_init(&info->irqlock);

	if (of_property_read_u32_array(pdev->dev.of_node, "reg", val32, 4) != 0) {
		dev_err(&pdev->dev, "can not get bank!\n");
		return -EINVAL;
	}

	info->phyaddr = val32[1];

	if (of_property_read_u32(pdev->dev.of_node, "pdma_reqsel_tx", &dma_tx_num) != 0) {
		dev_err(&pdev->dev, "can not get bank!\n");
		return -EINVAL;
	}

	info->pdma_reqsel_tx = dma_tx_num;
	pr_debug("info->pdma_reqsel_tx = 0x%lx\n", (ulong)info->pdma_reqsel_tx);

	if (of_property_read_u32(pdev->dev.of_node, "pdma_reqsel_rx", &dma_rx_num) != 0) {
		dev_err(&pdev->dev, "can not get bank!\n");
		return -EINVAL;
	}

	info->pdma_reqsel_rx = dma_rx_num;
	pr_debug("info->pdma_reqsel_rx = 0x%lx\n", (ulong)info->pdma_reqsel_rx);

	info->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!info->res) {
		dev_err(&pdev->dev, "platform_get_resource error\n");
		ret = -ENODEV;
		goto out1;
	}

	info->regs = devm_ioremap_resource(&pdev->dev, info->res);

	info->clk = of_clk_get(pdev->dev.of_node, 0);
	if (IS_ERR(info->clk)) {
		dev_err(&pdev->dev, "clk_get error\n");
		ret = PTR_ERR(info->clk);
		goto out2;
	}
	clk_prepare_enable(info->clk);

	info->irq_num = platform_get_irq(pdev, 0);
	if (!info->irq_num) {
		dev_err(&pdev->dev, "platform_get_irq error\n");
		ret = -EBUSY;
		goto out3;
	}

	if (of_property_read_u32(pdev->dev.of_node, "mclk_out", &mclk_out) != 0)
		info->mclk_out = 12000000;
	else
		info->mclk_out = mclk_out;

	of_property_read_u32(pdev->dev.of_node, "pdm-decimation",
			     &info->pdm_decimation);
	if (info->pdm_decimation &&
	    info->pdm_decimation != 64 &&
	    info->pdm_decimation != 128)
		return -EINVAL;

	info->raw_pdm_mono = of_property_read_bool(pdev->dev.of_node,
						   "nuvoton,raw-pdm-mono");

	dev_set_drvdata(&pdev->dev, info);

	ret = devm_snd_soc_register_component(&pdev->dev, &ma35d1_i2s_component,
	                                      &ma35d1_i2s_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "failed to register ASoC DAI\n");
		goto out3;
	}

	return 0;

out3:
	clk_put(info->clk);
out2:
	iounmap(info->regs);
	release_mem_region(info->res->start, resource_size(info->res));
out1:
	kfree(info);

	return ret;
}

static const struct of_device_id ma35d1_audio_i2s_of_match[] = {
	{   .compatible = "nuvoton,ma35d1-audio-i2s"    },
	{   },
};
MODULE_DEVICE_TABLE(of, ma35d1_audio_i2s_of_match);

static int ma35d1_i2s_drvremove(struct platform_device *pdev)
{
	struct ma35d1_i2s_info *info = dev_get_drvdata(&pdev->dev);

	snd_soc_unregister_component(&pdev->dev);

	clk_put(info->clk);
	iounmap(info->regs);
	release_mem_region(info->res->start, resource_size(info->res));

	kfree(info);

	return 0;
}

static struct platform_driver ma35d1_i2s_driver = {
	.driver = {
		.name   = "ma35d1-audio-i2s",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(ma35d1_audio_i2s_of_match),
	},
	.probe      = ma35d1_i2s_drvprobe,
	.remove     = ma35d1_i2s_drvremove,
};

module_platform_driver(ma35d1_i2s_driver);

MODULE_DESCRIPTION("MA35D1 IIS SoC driver!");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ma35d1-i2s");

