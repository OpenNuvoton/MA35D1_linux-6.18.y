// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35D1 Enhanced ADC driver
 *
 * Copyright (c) 2026 Nuvoton Technology Corp.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_data/dma-ma35d1.h>
#include <linux/property.h>
#include <linux/string.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

/* Register offsets */
#define MA35D1_EADC_DAT(_n)		(0x00 + ((_n) * 4))
#define MA35D1_EADC_CURDAT		0x4c
#define MA35D1_EADC_CTL			0x50
#define MA35D1_EADC_SWTRG		0x54
#define MA35D1_EADC_SCTL(_n)		(0x80 + ((_n) * 4))
#define MA35D1_EADC_INTSRC0		0xd0
#define MA35D1_EADC_STATUS2		0xf8
#define MA35D1_EADC_PDMACTL		0x130
#define MA35D1_EADC_SELSMP0		0x140
#define MA35D1_EADC_REFADJCTL		0x150

/* CTL */
#define MA35D1_EADC_CTL_ADCEN		BIT(0)
#define MA35D1_EADC_CTL_ADCIEN0		BIT(2)
#define MA35D1_EADC_CTL_DIFFEN		BIT(8)

/* SCTL */
#define MA35D1_EADC_SCTL_CHSEL_MASK	GENMASK(3, 0)
#define MA35D1_EADC_SCTL_TRGDLY_MASK	GENMASK(15, 8)
#define MA35D1_EADC_SCTL_TRGSEL_MASK	GENMASK(21, 16)
#define MA35D1_EADC_SCTL_ADINT0TRG	FIELD_PREP(MA35D1_EADC_SCTL_TRGSEL_MASK, 2)

/* STATUS2 */
#define MA35D1_EADC_STATUS2_ADIF0	BIT(0)

/* DAT */
#define MA35D1_EADC_DAT_OV		BIT(16)
#define MA35D1_EADC_DAT_VALID		BIT(17)

/* INTSRC0 */
#define MA35D1_EADC_INTSRC0_MASK	GENMASK(8, 0)
#define MA35D1_EADC_INTSRC0_SPLIEN(_n)	BIT(_n)

/* REFADJCTL */
#define MA35D1_EADC_REFADJCTL_PDREF	BIT(0)

/* SELSMP0 */
#define MA35D1_EADC_SELSMP0_SMPT0_MASK	GENMASK(1, 0)
#define MA35D1_EADC_SELSMP_LONG		FIELD_PREP(MA35D1_EADC_SELSMP0_SMPT0_MASK, 3)

/* PDMACTL */
#define MA35D1_EADC_PDMACTL_PDMABUSY	BIT(31)
#define MA35D1_EADC_PDMACTL_EN_MASK	GENMASK(8, 0)

#define MA35D1_EADC_DATA_MASK		GENMASK(11, 0)
#define MA35D1_EADC_MAX_CHANNELS	9
#define MA35D1_EADC_MAX_SAMPLE_MODULES	9
#define MA35D1_EADC_TIMEOUT_MS		1000
#define MA35D1_EADC_CLK_RATE_HZ		45000000UL
#define MA35D1_EADC_MAX_CLK_RATE_HZ	80000000UL

#define MA35D1_EADC_DMA_SCANS_PER_PERIOD	64
#define MA35D1_EADC_DMA_PERIODS			2
#define MA35D1_EADC_DMA_MAX_BUFFER_SIZE			\
	(MA35D1_EADC_MAX_CHANNELS * sizeof(u16) *		\
	 MA35D1_EADC_DMA_SCANS_PER_PERIOD * MA35D1_EADC_DMA_PERIODS)

struct ma35d1_adc_diff_channel {
	u32 vinp;
	u32 vinn;
};

struct ma35d1_adc_dma {
	struct dma_chan *chan;
	struct dma_async_tx_descriptor *desc;
	struct dma_slave_config config;
	struct ma35d1_peripheral peripheral;
	struct device *dma_dev;
	u8 *buf;
	dma_addr_t dma_addr;
	dma_cookie_t cookie;
	size_t alloc_len;
	size_t frame_bytes;
	size_t period_len;
	size_t buffer_len;
	unsigned int period;
	bool running;
};

struct ma35d1_adc_scan {
	u16 channels[MA35D1_EADC_MAX_CHANNELS];
	s64 timestamp __aligned(8);
};

struct ma35d1_adc {
	struct device *dev;
	void __iomem *regs;
	dma_addr_t phys_base;
	struct clk *clk;
	int irq;
	struct completion completion;
	struct mutex lock;
	struct iio_trigger *trig;
	struct ma35d1_adc_scan scan;
	unsigned int scan_chancnt;
	struct ma35d1_adc_dma dma;
};

static void ma35d1_adc_clk_disable(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);
}

static int ma35d1_adc_setup_clock(struct device *dev, struct ma35d1_adc *adc)
{
	unsigned long rate;
	int ret;

	adc->clk = devm_clk_get(dev, "eadc_gate");
	if (IS_ERR(adc->clk))
		return dev_err_probe(dev, PTR_ERR(adc->clk),
				     "failed to get EADC clock\n");

	/*
	 * EADC_CLK is generated from PCLK2 through CLK_CLKDIV4.EADCDIV:
	 *
	 *   EADC_CLK = PCLK2 / (2 * (EADCDIV + 1))
	 *
	 * With the normal MA35D1 PCLK2 rate of 180 MHz, requesting 45 MHz
	 * selects EADCDIV = 1 (divide by 4).  EADC_GATE has
	 * CLK_SET_RATE_PARENT, so clk_set_rate() propagates the request to
	 * the eadc_div clock.
	 */
	ret = clk_set_rate(adc->clk, MA35D1_EADC_CLK_RATE_HZ);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set EADC clock to %lu Hz\n",
				     MA35D1_EADC_CLK_RATE_HZ);

	rate = clk_get_rate(adc->clk);
	if (rate != MA35D1_EADC_CLK_RATE_HZ)
		return dev_err_probe(dev, -EINVAL,
				     "EADC clock is %lu Hz, expected %lu Hz\n",
				     rate, MA35D1_EADC_CLK_RATE_HZ);

	if (rate > MA35D1_EADC_MAX_CLK_RATE_HZ)
		return dev_err_probe(dev, -EINVAL,
				     "EADC clock %lu Hz exceeds %lu Hz maximum\n",
				     rate, MA35D1_EADC_MAX_CLK_RATE_HZ);

	ret = clk_prepare_enable(adc->clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to enable EADC clock\n");

	ret = devm_add_action_or_reset(dev, ma35d1_adc_clk_disable, adc->clk);
	if (ret)
		return ret;

	dev_info(dev, "EADC clock set to %lu Hz\n", rate);

	return 0;
}

static void ma35d1_adc_dma_release(void *data)
{
	struct ma35d1_adc *adc = data;

	if (!adc->dma.chan)
		return;

	dma_release_channel(adc->dma.chan);
	adc->dma.chan = NULL;
}

static void ma35d1_adc_dma_free_buffer(void *data)
{
	struct ma35d1_adc *adc = data;

	if (!adc->dma.buf)
		return;

	dma_free_coherent(adc->dma.dma_dev, adc->dma.alloc_len,
			  adc->dma.buf, adc->dma.dma_addr);
	adc->dma.buf = NULL;
}

static void ma35d1_adc_dma_stop(struct ma35d1_adc *adc)
{
	if (!adc->dma.chan)
		return;

	writel(0, adc->regs + MA35D1_EADC_PDMACTL);
	adc->dma.running = false;
	dmaengine_terminate_sync(adc->dma.chan);

	/* PDMABUSY is write-one-to-clear. */
	writel(MA35D1_EADC_PDMACTL_PDMABUSY,
	       adc->regs + MA35D1_EADC_PDMACTL);
}

static void ma35d1_adc_hw_init(struct ma35d1_adc *adc)
{
	u32 val;

	/* Start from a quiescent interrupt/PDMA state. */
	val = readl(adc->regs + MA35D1_EADC_CTL);
	val &= ~(MA35D1_EADC_CTL_ADCIEN0 | MA35D1_EADC_CTL_DIFFEN);
	writel(val, adc->regs + MA35D1_EADC_CTL);
	writel(0, adc->regs + MA35D1_EADC_PDMACTL);

	val = readl(adc->regs + MA35D1_EADC_INTSRC0);
	val &= ~MA35D1_EADC_INTSRC0_MASK;
	writel(val, adc->regs + MA35D1_EADC_INTSRC0);
	writel(MA35D1_EADC_STATUS2_ADIF0,
	       adc->regs + MA35D1_EADC_STATUS2);

	/*
	 * Keep the reference/sample-time setup used by the MA35D1 BSP.
	 * PDREF powers down the internal reference so conversions use the
	 * external VREF pin.  Use the longest sample time for sample module 0.
	 */
	val = readl(adc->regs + MA35D1_EADC_REFADJCTL);
	val |= MA35D1_EADC_REFADJCTL_PDREF;
	writel(val, adc->regs + MA35D1_EADC_REFADJCTL);

	val = readl(adc->regs + MA35D1_EADC_SELSMP0);
	val &= ~MA35D1_EADC_SELSMP0_SMPT0_MASK;
	val |= MA35D1_EADC_SELSMP_LONG;
	writel(val, adc->regs + MA35D1_EADC_SELSMP0);

	val = readl(adc->regs + MA35D1_EADC_CTL);
	val |= MA35D1_EADC_CTL_ADCEN;
	writel(val, adc->regs + MA35D1_EADC_CTL);
}

static void ma35d1_adc_hw_disable(void *data)
{
	struct iio_dev *indio_dev = data;
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	u32 val;

	ma35d1_adc_dma_stop(adc);

	val = readl(adc->regs + MA35D1_EADC_CTL);
	val &= ~(MA35D1_EADC_CTL_ADCIEN0 | MA35D1_EADC_CTL_ADCEN);
	writel(val, adc->regs + MA35D1_EADC_CTL);
}

static void ma35d1_adc_chan_init(struct iio_chan_spec *chan,
				 unsigned int vinp, unsigned int vinn,
				 unsigned int scan_index, bool differential)
{
	chan->type = IIO_VOLTAGE;
	chan->indexed = 1;
	chan->channel = vinp;
	chan->address = vinp;
	chan->scan_index = scan_index;
	chan->info_mask_separate = BIT(IIO_CHAN_INFO_RAW);
	chan->differential = differential;
	if (differential)
		chan->channel2 = vinn;
	chan->scan_type.sign = 'u';
	chan->scan_type.realbits = 12;
	chan->scan_type.storagebits = 16;
	chan->scan_type.shift = 0;
	chan->scan_type.endianness = IIO_LE;
}

static int ma35d1_adc_parse_channels(struct device *dev,
				     struct iio_dev *indio_dev)
{
	struct iio_chan_spec *channels;
	unsigned int count;
	unsigned int index = 0;

	count = device_get_child_node_count(dev);
	if (!count)
		return dev_err_probe(dev, -EINVAL,
				     "no ADC channel child nodes\n");

	if (count > MA35D1_EADC_MAX_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "too many ADC channels: %u\n", count);

	channels = devm_kcalloc(dev, count + 1, sizeof(*channels), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	device_for_each_child_node_scoped(dev, child) {
		u32 diff[2];
		u32 vinp;
		int ret;

		ret = fwnode_property_read_u32_array(child, "diff-channels",
						     diff, ARRAY_SIZE(diff));
		if (!ret) {
			if (diff[0] >= MA35D1_EADC_MAX_CHANNELS ||
			    diff[1] >= MA35D1_EADC_MAX_CHANNELS)
				return dev_err_probe(dev, -EINVAL,
					"invalid differential channel <%u %u>\n",
					diff[0], diff[1]);

			ma35d1_adc_chan_init(&channels[index], diff[0], diff[1],
					     index, true);
			index++;
			continue;
		}

		if (!fwnode_property_read_u32(child, "single-channel", &vinp)) {
			/* Standard generic ADC binding for an explicit input pin. */
		} else if (fwnode_property_read_u32(child, "reg", &vinp)) {
			return dev_err_probe(dev, -EINVAL,
					     "channel node is missing reg\n");
		}

		if (vinp >= MA35D1_EADC_MAX_CHANNELS)
			return dev_err_probe(dev, -EINVAL,
					     "invalid ADC channel %u\n", vinp);

		ma35d1_adc_chan_init(&channels[index], vinp, 0, index, false);
		index++;
	}

	channels[index] = (struct iio_chan_spec)IIO_CHAN_SOFT_TIMESTAMP(index);

	indio_dev->channels = channels;
	indio_dev->num_channels = index + 1;

	return 0;
}

static int ma35d1_adc_config_scan(struct iio_dev *indio_dev,
				  const unsigned long *scan_mask)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	const struct iio_chan_spec *chan;
	bool differential = false;
	bool have_mode = false;
	unsigned int module = 0;
	unsigned int bit;
	u32 val;

	for_each_set_bit(bit, scan_mask, indio_dev->num_channels) {
		chan = &indio_dev->channels[bit];
		if (chan->type == IIO_TIMESTAMP)
			continue;

		if (module >= MA35D1_EADC_MAX_SAMPLE_MODULES)
			return -EINVAL;

		if (!have_mode) {
			differential = chan->differential;
			have_mode = true;
		} else if (differential != chan->differential) {
			/* DIFFEN is global for the controller. */
			return -EINVAL;
		}

		val = readl(adc->regs + MA35D1_EADC_SCTL(module));
		val &= ~(MA35D1_EADC_SCTL_CHSEL_MASK |
			 MA35D1_EADC_SCTL_TRGSEL_MASK);
		val |= FIELD_PREP(MA35D1_EADC_SCTL_CHSEL_MASK, chan->channel);
		val |= MA35D1_EADC_SCTL_ADINT0TRG;
		writel(val, adc->regs + MA35D1_EADC_SCTL(module));

		module++;
	}

	if (!module)
		return -EINVAL;

	val = readl(adc->regs + MA35D1_EADC_CTL);
	if (differential)
		val |= MA35D1_EADC_CTL_DIFFEN;
	else
		val &= ~MA35D1_EADC_CTL_DIFFEN;
	writel(val, adc->regs + MA35D1_EADC_CTL);

	adc->scan_chancnt = module;

	return 0;
}

static int ma35d1_adc_update_scan_mode(struct iio_dev *indio_dev,
				       const unsigned long *scan_mask)
{
	return ma35d1_adc_config_scan(indio_dev, scan_mask);
}

static irqreturn_t ma35d1_adc_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ma35d1_adc *adc = iio_priv(indio_dev);

	iio_push_to_buffers_with_timestamp(indio_dev, &adc->scan,
					   pf->timestamp);
	iio_trigger_notify_done(indio_dev->trig);

	/* CPU IRQ mode only. DMA mode leaves ADCIEN0 disabled. */
	if (!adc->dma.running)
		writel(readl(adc->regs + MA35D1_EADC_CTL) |
		       MA35D1_EADC_CTL_ADCIEN0,
		       adc->regs + MA35D1_EADC_CTL);

	return IRQ_HANDLED;
}

static irqreturn_t ma35d1_adc_isr(int irq, void *data)
{
	struct iio_dev *indio_dev = data;
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	u32 status;
	unsigned int i;

	status = readl(adc->regs + MA35D1_EADC_STATUS2);
	if (!(status & MA35D1_EADC_STATUS2_ADIF0))
		return IRQ_NONE;

	writel(MA35D1_EADC_STATUS2_ADIF0,
	       adc->regs + MA35D1_EADC_STATUS2);

	if (iio_buffer_enabled(indio_dev)) {
		if (adc->dma.running)
			return IRQ_HANDLED;

		for (i = 0; i < adc->scan_chancnt; i++)
			adc->scan.channels[i] =
				readl(adc->regs + MA35D1_EADC_DAT(i)) &
				MA35D1_EADC_DATA_MASK;

		writel(readl(adc->regs + MA35D1_EADC_CTL) &
		       ~MA35D1_EADC_CTL_ADCIEN0,
		       adc->regs + MA35D1_EADC_CTL);
		iio_trigger_poll(indio_dev->trig);
	} else {
		/* Direct conversion reads DAT0 after the completion wakes it. */
		complete(&adc->completion);
	}

	return IRQ_HANDLED;
}

static void ma35d1_adc_dma_complete(void *data)
{
	struct iio_dev *indio_dev = data;
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	u8 *period_buf;
	unsigned int scan;
	unsigned int chan;

	if (!adc->dma.running)
		return;

	period_buf = adc->dma.buf + adc->dma.period * adc->dma.period_len;

	for (scan = 0; scan < MA35D1_EADC_DMA_SCANS_PER_PERIOD; scan++) {
		u16 *frame = (u16 *)(period_buf + scan * adc->dma.frame_bytes);

		for (chan = 0; chan < adc->scan_chancnt; chan++)
			adc->scan.channels[chan] = frame[chan] &
						   MA35D1_EADC_DATA_MASK;

		iio_push_to_buffers_with_timestamp(indio_dev, &adc->scan,
						   iio_get_time_ns(indio_dev));
	}

	adc->dma.period++;
	if (adc->dma.period >= MA35D1_EADC_DMA_PERIODS)
		adc->dma.period = 0;
}

static int ma35d1_adc_dma_start(struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	u32 pdma_mask;
	int ret;

	if (!adc->dma.chan)
		return -ENODEV;

	if (!adc->scan_chancnt ||
	    adc->scan_chancnt > MA35D1_EADC_MAX_SAMPLE_MODULES)
		return -EINVAL;

	adc->dma.frame_bytes = adc->scan_chancnt * sizeof(u16);
	adc->dma.period_len = adc->dma.frame_bytes *
				  MA35D1_EADC_DMA_SCANS_PER_PERIOD;
	adc->dma.buffer_len = adc->dma.period_len * MA35D1_EADC_DMA_PERIODS;
	if (adc->dma.buffer_len > adc->dma.alloc_len)
		return -EINVAL;

	ma35d1_adc_dma_stop(adc);
	memset(adc->dma.buf, 0, adc->dma.buffer_len);
	adc->dma.period = 0;

	ret = dmaengine_slave_config(adc->dma.chan, &adc->dma.config);
	if (ret) {
		dev_err(adc->dev, "failed to configure RX DMA: %d\n", ret);
		return ret;
	}

	adc->dma.desc = dmaengine_prep_dma_cyclic(adc->dma.chan,
						  adc->dma.dma_addr,
						  adc->dma.buffer_len,
						  adc->dma.period_len,
						  DMA_DEV_TO_MEM,
						  DMA_PREP_INTERRUPT |
						  DMA_CTRL_ACK);
	if (!adc->dma.desc)
		return -EIO;

	adc->dma.desc->callback = ma35d1_adc_dma_complete;
	adc->dma.desc->callback_param = indio_dev;

	adc->dma.cookie = dmaengine_submit(adc->dma.desc);
	ret = dma_submit_error(adc->dma.cookie);
	if (ret)
		return ret;

	adc->dma.running = true;
	dma_async_issue_pending(adc->dma.chan);

	pdma_mask = GENMASK(adc->scan_chancnt - 1, 0) &
		    MA35D1_EADC_PDMACTL_EN_MASK;
	writel(pdma_mask, adc->regs + MA35D1_EADC_PDMACTL);

	writel(1, adc->regs + MA35D1_EADC_SWTRG);

	return 0;
}

static int ma35d1_adc_dma_request(struct device *dev,
				  struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int ret;

	if (!device_property_present(dev, "dmas"))
		return 0;

	ret = device_property_read_u32(dev, "nuvoton,pdma-reqsel-rx",
				       &adc->dma.peripheral.reqsel);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing nuvoton,pdma-reqsel-rx\n");

	adc->dma.chan = dma_request_chan(dev, "rx");
	if (IS_ERR(adc->dma.chan)) {
		ret = PTR_ERR(adc->dma.chan);
		adc->dma.chan = NULL;
		return dev_err_probe(dev, ret,
				     "failed to request RX DMA channel\n");
	}

	ret = devm_add_action_or_reset(dev, ma35d1_adc_dma_release, adc);
	if (ret)
		return ret;

	adc->dma.dma_dev = dmaengine_get_dma_device(adc->dma.chan);
	adc->dma.alloc_len = MA35D1_EADC_DMA_MAX_BUFFER_SIZE;
	adc->dma.buf = dma_alloc_coherent(adc->dma.dma_dev,
					  adc->dma.alloc_len,
					  &adc->dma.dma_addr, GFP_KERNEL);
	if (!adc->dma.buf)
		return -ENOMEM;

	ret = devm_add_action_or_reset(dev, ma35d1_adc_dma_free_buffer, adc);
	if (ret)
		return ret;

	adc->dma.config.direction = DMA_DEV_TO_MEM;
	adc->dma.config.src_addr = adc->phys_base + MA35D1_EADC_CURDAT;
	adc->dma.config.src_addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
	adc->dma.config.src_maxburst = 1;
	adc->dma.config.peripheral_config = &adc->dma.peripheral;
	adc->dma.config.peripheral_size = sizeof(adc->dma.peripheral);

	return 0;
}

static int ma35d1_adc_read_raw(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       int *val, int *val2, long mask)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	long timeout;
	u32 data;
	u32 ctl;
	u32 sctl;
	u32 intsrc;
	int ret;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	if (!iio_device_claim_direct(indio_dev))
		return -EBUSY;

	mutex_lock(&adc->lock);
	reinit_completion(&adc->completion);

	/*
	 * Direct reads always use sample module 0.  ADINT0 only fires when
	 * INTSRC0 routes that sample module into interrupt source 0, so set
	 * the route explicitly instead of depending on buffered-mode state.
	 */
	ctl = readl(adc->regs + MA35D1_EADC_CTL);
	ctl &= ~MA35D1_EADC_CTL_ADCIEN0;
	writel(ctl, adc->regs + MA35D1_EADC_CTL);

	writel(MA35D1_EADC_STATUS2_ADIF0,
	       adc->regs + MA35D1_EADC_STATUS2);

	intsrc = readl(adc->regs + MA35D1_EADC_INTSRC0);
	intsrc &= ~MA35D1_EADC_INTSRC0_MASK;
	intsrc |= MA35D1_EADC_INTSRC0_SPLIEN(0);
	writel(intsrc, adc->regs + MA35D1_EADC_INTSRC0);

	sctl = readl(adc->regs + MA35D1_EADC_SCTL(0));
	sctl &= ~(MA35D1_EADC_SCTL_CHSEL_MASK |
		  MA35D1_EADC_SCTL_TRGSEL_MASK);
	sctl |= FIELD_PREP(MA35D1_EADC_SCTL_CHSEL_MASK, chan->channel);
	writel(sctl, adc->regs + MA35D1_EADC_SCTL(0));

	ctl = readl(adc->regs + MA35D1_EADC_CTL);
	if (chan->differential)
		ctl |= MA35D1_EADC_CTL_DIFFEN;
	else
		ctl &= ~MA35D1_EADC_CTL_DIFFEN;
	ctl |= MA35D1_EADC_CTL_ADCEN | MA35D1_EADC_CTL_ADCIEN0;
	writel(ctl, adc->regs + MA35D1_EADC_CTL);

	writel(BIT(0), adc->regs + MA35D1_EADC_SWTRG);

	timeout = wait_for_completion_interruptible_timeout(
			&adc->completion,
			msecs_to_jiffies(MA35D1_EADC_TIMEOUT_MS));

	/* Always return the direct-conversion interrupt path to idle. */
	ctl = readl(adc->regs + MA35D1_EADC_CTL);
	ctl &= ~MA35D1_EADC_CTL_ADCIEN0;
	writel(ctl, adc->regs + MA35D1_EADC_CTL);

	intsrc = readl(adc->regs + MA35D1_EADC_INTSRC0);
	intsrc &= ~MA35D1_EADC_INTSRC0_MASK;
	writel(intsrc, adc->regs + MA35D1_EADC_INTSRC0);

	if (timeout < 0) {
		ret = timeout;
		goto out_clear_status;
	}

	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out_clear_status;
	}

	data = readl(adc->regs + MA35D1_EADC_DAT(0));
	if (!(data & MA35D1_EADC_DAT_VALID)) {
		ret = -EIO;
		goto out_clear_status;
	}

	if (data & MA35D1_EADC_DAT_OV) {
		ret = -EOVERFLOW;
		goto out_clear_status;
	}

	*val = data & MA35D1_EADC_DATA_MASK;
	ret = IIO_VAL_INT;

out_clear_status:
	writel(MA35D1_EADC_STATUS2_ADIF0,
	       adc->regs + MA35D1_EADC_STATUS2);
	mutex_unlock(&adc->lock);
	iio_device_release_direct(indio_dev);

	return ret;
}

static int ma35d1_adc_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	u32 val;
	int ret;

	if (!adc->scan_chancnt)
		return -EINVAL;

	writel(MA35D1_EADC_STATUS2_ADIF0,
	       adc->regs + MA35D1_EADC_STATUS2);

	/* Route sample module 0 completion to ADINT0 for continuous chaining. */
	val = readl(adc->regs + MA35D1_EADC_INTSRC0);
	val &= ~MA35D1_EADC_INTSRC0_MASK;
	val |= MA35D1_EADC_INTSRC0_SPLIEN(0);
	writel(val, adc->regs + MA35D1_EADC_INTSRC0);

	val = readl(adc->regs + MA35D1_EADC_SCTL(0));
	val |= MA35D1_EADC_SCTL_TRGDLY_MASK;
	writel(val, adc->regs + MA35D1_EADC_SCTL(0));

	if (adc->dma.chan) {
		writel(readl(adc->regs + MA35D1_EADC_CTL) &
		       ~MA35D1_EADC_CTL_ADCIEN0,
		       adc->regs + MA35D1_EADC_CTL);

		ret = ma35d1_adc_dma_start(indio_dev);
		if (ret)
			return ret;
	} else {
		writel(readl(adc->regs + MA35D1_EADC_CTL) |
		       MA35D1_EADC_CTL_ADCIEN0,
		       adc->regs + MA35D1_EADC_CTL);
		writel(1, adc->regs + MA35D1_EADC_SWTRG);
	}

	return 0;
}

static int ma35d1_adc_buffer_predisable(struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	unsigned int i;

	ma35d1_adc_dma_stop(adc);

	writel(readl(adc->regs + MA35D1_EADC_CTL) &
	       ~MA35D1_EADC_CTL_ADCIEN0,
	       adc->regs + MA35D1_EADC_CTL);

	/*
	 * Disabling ADCIEN0 above only stops the EADC from generating
	 * *future* interrupts; it does not retract one that is already
	 * latched/in-flight on another CPU. Right after this callback
	 * returns, the IIO core calls free_irq() on the trigger's subirq,
	 * destroying the kernel thread that runs
	 * ma35d1_adc_trigger_handler(). If ma35d1_adc_isr() is still
	 * running at that moment, it can call iio_trigger_poll()
	 * concurrently with that free_irq(), permanently losing the EOC
	 * notification since the thread that would handle it is already
	 * gone. synchronize_irq() blocks until any in-flight ISR instance
	 * has returned and guarantees none can start afterwards, closing
	 * this race before the IIO core proceeds to free_irq().
	 */
	synchronize_irq(adc->irq);

	writel(readl(adc->regs + MA35D1_EADC_INTSRC0) &
	       ~MA35D1_EADC_INTSRC0_MASK,
	       adc->regs + MA35D1_EADC_INTSRC0);
	writel(MA35D1_EADC_STATUS2_ADIF0,
	       adc->regs + MA35D1_EADC_STATUS2);

	for (i = 0; i < adc->scan_chancnt; i++)
		writel(readl(adc->regs + MA35D1_EADC_SCTL(i)) &
		       ~MA35D1_EADC_SCTL_TRGSEL_MASK,
		       adc->regs + MA35D1_EADC_SCTL(i));

	return 0;
}

static const struct iio_buffer_setup_ops ma35d1_adc_buffer_ops = {
	.postenable = ma35d1_adc_buffer_postenable,
	.predisable = ma35d1_adc_buffer_predisable,
};

static const struct iio_info ma35d1_adc_info = {
	.read_raw = ma35d1_adc_read_raw,
	.update_scan_mode = ma35d1_adc_update_scan_mode,
};

static const struct iio_trigger_ops ma35d1_adc_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static int ma35d1_adc_setup_trigger(struct device *dev,
				    struct iio_dev *indio_dev)
{
	struct ma35d1_adc *adc = iio_priv(indio_dev);
	int ret;

	adc->trig = devm_iio_trigger_alloc(dev, "%s-trigger",
					  dev_name(dev));
	if (!adc->trig)
		return -ENOMEM;

	adc->trig->ops = &ma35d1_adc_trigger_ops;
	iio_trigger_set_drvdata(adc->trig, indio_dev);

	ret = devm_iio_trigger_register(dev, adc->trig);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IIO trigger\n");

	ret = iio_trigger_set_immutable(indio_dev, adc->trig);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set immutable IIO trigger\n");

	return 0;
}

static int ma35d1_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ma35d1_adc *adc;
	struct iio_dev *indio_dev;
	struct resource *res;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->dev = dev;
	mutex_init(&adc->lock);
	init_completion(&adc->completion);

	adc->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(adc->regs))
		return PTR_ERR(adc->regs);
	adc->phys_base = res->start;

	ret = ma35d1_adc_setup_clock(dev, adc);
	if (ret)
		return ret;

	ret = ma35d1_adc_parse_channels(dev, indio_dev);
	if (ret)
		return ret;

	adc->irq = platform_get_irq(pdev, 0);
	if (adc->irq < 0)
		return adc->irq;

	ret = devm_request_irq(dev, adc->irq, ma35d1_adc_isr, 0,
			       dev_name(dev), indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request EADC IRQ\n");

	ret = ma35d1_adc_dma_request(dev, indio_dev);
	if (ret)
		return ret;

	indio_dev->name = dev_name(dev);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ma35d1_adc_info;

	ret = ma35d1_adc_setup_trigger(dev, indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      &iio_pollfunc_store_time,
					      ma35d1_adc_trigger_handler,
					      &ma35d1_adc_buffer_ops);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to setup triggered buffer\n");

	ret = devm_add_action_or_reset(dev, ma35d1_adc_hw_disable, indio_dev);
	if (ret)
		return ret;

	/* Common hardware state used by both direct and buffered conversions. */
	ma35d1_adc_hw_init(adc);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IIO device\n");

	platform_set_drvdata(pdev, indio_dev);

	dev_info(dev, "MA35D1 EADC registered%s at %lu Hz\n",
		 adc->dma.chan ? " with PDMA" : "",
		 clk_get_rate(adc->clk));

	return 0;
}

static const struct of_device_id ma35d1_adc_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-eadc" },
	{ }
};
MODULE_DEVICE_TABLE(of, ma35d1_adc_of_match);

static struct platform_driver ma35d1_adc_driver = {
	.probe = ma35d1_adc_probe,
	.driver = {
		.name = "ma35d1-eadc",
		.of_match_table = ma35d1_adc_of_match,
	},
};
module_platform_driver(ma35d1_adc_driver);

MODULE_DESCRIPTION("Nuvoton MA35D1 Enhanced ADC driver");
MODULE_AUTHOR("Nuvoton Technology Corp.");
MODULE_LICENSE("GPL");
