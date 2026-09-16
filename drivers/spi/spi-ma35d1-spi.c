// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Nuvoton MA35 Series SPI controller driver
 *
 * Copyright (c) 2026 Nuvoton Technology Corp.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_data/dma-ma35d1.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>

/* SPI register offsets */
#define MA35_SPI_CTL			0x00
#define MA35_SPI_CLKDIV			0x04
#define MA35_SPI_SSCTL			0x08
#define MA35_SPI_PDMACTL		0x0c
#define MA35_SPI_FIFOCTL		0x10
#define MA35_SPI_STATUS			0x14
#define MA35_SPI_TX			0x20
#define MA35_SPI_RX			0x30

/* SPI Control Register */
#define MA35_SPI_CTL_DATDIR		BIT(20)
#define MA35_SPI_CTL_REORDER		BIT(19)
#define MA35_SPI_CTL_SLAVE		BIT(18)
#define MA35_SPI_CTL_UNITIEN		BIT(17)
#define MA35_SPI_CTL_RXONLY		BIT(15)
#define MA35_SPI_CTL_HALFDPX		BIT(14)
#define MA35_SPI_CTL_LSB		BIT(13)
#define MA35_SPI_CTL_DWIDTH_MASK	GENMASK(12, 8)
#define MA35_SPI_CTL_SUSPITV_MASK	GENMASK(7, 4)
#define MA35_SPI_CTL_CLKPOL		BIT(3)
#define MA35_SPI_CTL_TXNEG		BIT(2)
#define MA35_SPI_CTL_RXNEG		BIT(1)
#define MA35_SPI_CTL_SPIEN		BIT(0)

/* SPI Clock Divider Register */
#define MA35_SPI_CLKDIV_MASK		GENMASK(8, 0)
#define MA35_SPI_MIN_DIVISOR		2U
#define MA35_SPI_MAX_DIVISOR		512U

/* SPI Slave Select Control Register */
#define MA35_SPI_SSCTL_SS0		BIT(0)
#define MA35_SPI_SSCTL_SS1		BIT(1)
#define MA35_SPI_SSCTL_SSACTPOL		BIT(2)
#define MA35_SPI_SSCTL_AUTOSS		BIT(3)
#define MA35_SPI_SSCTL_SLV3WIRE		BIT(4)
#define MA35_SPI_SSCTL_SLVBEIEN		BIT(8)
#define MA35_SPI_SSCTL_SLVURIEN		BIT(9)
#define MA35_SPI_SSCTL_SSACTIEN		BIT(12)
#define MA35_SPI_SSCTL_SSINAIEN		BIT(13)

/* SPI PDMA Control Register */
#define MA35_SPI_PDMACTL_TXPDMAEN	BIT(0)
#define MA35_SPI_PDMACTL_RXPDMAEN	BIT(1)

/* SPI FIFO Control Register */
#define MA35_SPI_FIFOCTL_SLVBERX	BIT(10)
#define MA35_SPI_FIFOCTL_TXUFIEN	BIT(7)
#define MA35_SPI_FIFOCTL_TXUFPOL	BIT(6)
#define MA35_SPI_FIFOCTL_RXOVIEN	BIT(5)
#define MA35_SPI_FIFOCTL_RXTOIEN	BIT(4)
#define MA35_SPI_FIFOCTL_TXTHIEN	BIT(3)
#define MA35_SPI_FIFOCTL_RXTHIEN	BIT(2)
#define MA35_SPI_FIFOCTL_TXRST		BIT(1)
#define MA35_SPI_FIFOCTL_RXRST		BIT(0)

/* SPI Status Register */
#define MA35_SPI_STATUS_TXRXRST		BIT(23)
#define MA35_SPI_STATUS_TXFULL		BIT(17)
#define MA35_SPI_STATUS_TXEMPTY		BIT(16)
#define MA35_SPI_STATUS_SPIENSTS	BIT(15)
#define MA35_SPI_STATUS_RXEMPTY		BIT(8)
#define MA35_SPI_STATUS_BUSY		BIT(0)

#define MA35_SPI_MAX_NATIVE_CS		2U
#define MA35_SPI_DEFAULT_NUM_CS		2U
#define MA35_SPI_DEFAULT_BPW		8U
#define MA35_SPI_MAX_SPEED_HZ		100000000U
#define MA35_SPI_POLL_TIMEOUT_US	10000U
#define MA35_SPI_DMA_MIN_BYTES		64U
#define MA35_SPI_MAX_DMA_SEGMENT	0x10000U

struct ma35_spi {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk;
	dma_addr_t phys_base;

	/* Protects read-modify-write accesses to SSCTL. */
	spinlock_t ssctl_lock;

	struct dma_chan *dma_tx;
	struct dma_chan *dma_rx;
	struct ma35d1_peripheral tx_peripheral;
	struct ma35d1_peripheral rx_peripheral;
	struct completion dma_tx_done;
	struct completion dma_rx_done;
};

static u32 ma35_spi_read(struct ma35_spi *hw, u32 reg)
{
	return readl(hw->regs + reg);
}

static void ma35_spi_write(struct ma35_spi *hw, u32 reg, u32 val)
{
	writel(val, hw->regs + reg);
}

static void ma35_spi_update_bits(struct ma35_spi *hw, u32 reg,
				 u32 mask, u32 val)
{
	u32 tmp;

	tmp = ma35_spi_read(hw, reg);
	tmp &= ~mask;
	tmp |= val & mask;
	ma35_spi_write(hw, reg, tmp);
}

static void ma35_spi_update_ssctl_bits(struct ma35_spi *hw,
				       u32 mask, u32 val)
{
	unsigned long flags;
	u32 tmp;

	spin_lock_irqsave(&hw->ssctl_lock, flags);
	tmp = ma35_spi_read(hw, MA35_SPI_SSCTL);
	tmp &= ~mask;
	tmp |= val & mask;
	ma35_spi_write(hw, MA35_SPI_SSCTL, tmp);
	spin_unlock_irqrestore(&hw->ssctl_lock, flags);
}

static int ma35_spi_disable(struct ma35_spi *hw)
{
	u32 val;

	ma35_spi_update_bits(hw, MA35_SPI_CTL, MA35_SPI_CTL_SPIEN, 0);

	return readl_poll_timeout(hw->regs + MA35_SPI_STATUS, val,
				  !(val & MA35_SPI_STATUS_SPIENSTS),
				  1, MA35_SPI_POLL_TIMEOUT_US);
}

static int ma35_spi_enable(struct ma35_spi *hw)
{
	u32 val;

	ma35_spi_update_bits(hw, MA35_SPI_CTL, MA35_SPI_CTL_SPIEN,
			       MA35_SPI_CTL_SPIEN);

	return readl_poll_timeout(hw->regs + MA35_SPI_STATUS, val,
				  val & MA35_SPI_STATUS_SPIENSTS,
				  1, MA35_SPI_POLL_TIMEOUT_US);
}

static int ma35_spi_wait_idle(struct ma35_spi *hw)
{
	u32 val;

	return readl_poll_timeout(hw->regs + MA35_SPI_STATUS, val,
				  !(val & MA35_SPI_STATUS_BUSY) &&
				  (val & MA35_SPI_STATUS_TXEMPTY) &&
				  (val & MA35_SPI_STATUS_RXEMPTY),
				  1, MA35_SPI_POLL_TIMEOUT_US);
}

static int ma35_spi_wait_tx_not_full(struct ma35_spi *hw)
{
	u32 val;

	return readl_poll_timeout(hw->regs + MA35_SPI_STATUS, val,
				  !(val & MA35_SPI_STATUS_TXFULL),
				  0, MA35_SPI_POLL_TIMEOUT_US);
}

static int ma35_spi_wait_rx_not_empty(struct ma35_spi *hw)
{
	u32 val;

	return readl_poll_timeout(hw->regs + MA35_SPI_STATUS, val,
				  !(val & MA35_SPI_STATUS_RXEMPTY),
				  0, MA35_SPI_POLL_TIMEOUT_US);
}

static int ma35_spi_reset_fifo(struct ma35_spi *hw)
{
	u32 val;

	ma35_spi_update_bits(hw, MA35_SPI_FIFOCTL,
			       MA35_SPI_FIFOCTL_TXRST | MA35_SPI_FIFOCTL_RXRST,
			       MA35_SPI_FIFOCTL_TXRST | MA35_SPI_FIFOCTL_RXRST);

	/* Allow the reset request to be latched before polling status. */
	udelay(1);

	return readl_poll_timeout(hw->regs + MA35_SPI_STATUS, val,
				  !(val & MA35_SPI_STATUS_TXRXRST),
				  1, MA35_SPI_POLL_TIMEOUT_US);
}

static int ma35_spi_calc_divisor(unsigned long clk_rate, u32 speed_hz,
				 unsigned int *divisor)
{
	unsigned int div;

	if (!speed_hz)
		return -EINVAL;

	div = DIV_ROUND_UP(clk_rate, speed_hz);
	div = max(div, MA35_SPI_MIN_DIVISOR);

	/* CLKDIV encodes an odd value, i.e. an even clock divisor. */
	if (div & 1)
		div++;

	if (div > MA35_SPI_MAX_DIVISOR)
		return -EINVAL;

	*divisor = div;

	return 0;
}

static int ma35_spi_set_speed(struct ma35_spi *hw,
			      struct spi_transfer *xfer, u32 speed_hz)
{
	unsigned long clk_rate;
	unsigned int divisor;
	u32 clkdiv;
	int ret;

	clk_rate = clk_get_rate(hw->clk);
	if (!clk_rate)
		return -EINVAL;

	ret = ma35_spi_calc_divisor(clk_rate, speed_hz, &divisor);
	if (ret)
		return ret;

	clkdiv = divisor - 1;
	ma35_spi_update_bits(hw, MA35_SPI_CLKDIV, MA35_SPI_CLKDIV_MASK,
			       FIELD_PREP(MA35_SPI_CLKDIV_MASK, clkdiv));

	xfer->effective_speed_hz = clk_rate / divisor;

	return 0;
}

static unsigned int ma35_spi_word_bytes(u8 bpw)
{
	if (bpw <= 8)
		return 1;
	if (bpw <= 16)
		return 2;

	return 4;
}

static int ma35_spi_configure_transfer(struct ma35_spi *hw,
				       struct spi_device *spi,
				       struct spi_transfer *xfer,
				       u8 *bits_per_word)
{
	u32 speed_hz = xfer->speed_hz ?: spi->max_speed_hz;
	u8 bpw = xfer->bits_per_word ?: spi->bits_per_word;
	u32 ctl_mask;
	u32 dwidth;
	u32 ctl = 0;
	int ret;

	if (!bpw)
		bpw = MA35_SPI_DEFAULT_BPW;

	if (bpw < 8 || bpw > 32)
		return -EINVAL;

	if (xfer->len % ma35_spi_word_bytes(bpw))
		return -EINVAL;

	ret = ma35_spi_wait_idle(hw);
	if (ret) {
		dev_err(hw->dev, "controller did not become idle\n");
		return ret;
	}

	ret = ma35_spi_disable(hw);
	if (ret) {
		dev_err(hw->dev, "failed to disable controller\n");
		return ret;
	}

	ret = ma35_spi_set_speed(hw, xfer, speed_hz);
	if (ret) {
		dev_err(hw->dev, "unsupported SPI clock %u Hz\n", speed_hz);
		return ret;
	}

	ctl_mask = MA35_SPI_CTL_DATDIR |
		   MA35_SPI_CTL_REORDER |
		   MA35_SPI_CTL_SLAVE |
		   MA35_SPI_CTL_UNITIEN |
		   MA35_SPI_CTL_RXONLY |
		   MA35_SPI_CTL_HALFDPX |
		   MA35_SPI_CTL_LSB |
		   MA35_SPI_CTL_DWIDTH_MASK |
		   MA35_SPI_CTL_CLKPOL |
		   MA35_SPI_CTL_TXNEG |
		   MA35_SPI_CTL_RXNEG;

	dwidth = bpw == 32 ? 0 : bpw;
	ctl |= FIELD_PREP(MA35_SPI_CTL_DWIDTH_MASK, dwidth);

	if (spi->mode & SPI_CPOL)
		ctl |= MA35_SPI_CTL_CLKPOL;

	/*
	 * Mode 0/3: transmit on falling edge and receive on rising edge.
	 * Mode 1/2: transmit on rising edge and receive on falling edge.
	 */
	if (!!(spi->mode & SPI_CPOL) == !!(spi->mode & SPI_CPHA))
		ctl |= MA35_SPI_CTL_TXNEG;
	else
		ctl |= MA35_SPI_CTL_RXNEG;

	if (spi->mode & SPI_LSB_FIRST)
		ctl |= MA35_SPI_CTL_LSB;

	ma35_spi_update_bits(hw, MA35_SPI_CTL, ctl_mask, ctl);

	ret = ma35_spi_reset_fifo(hw);
	if (ret) {
		dev_err(hw->dev, "FIFO reset timed out\n");
		return ret;
	}

	*bits_per_word = bpw;

	return 0;
}

static u32 ma35_spi_get_tx_word(const void *txbuf, unsigned int offset,
				unsigned int bytes_per_word)
{
	if (!txbuf)
		return 0;

	switch (bytes_per_word) {
	case 1:
		return ((const u8 *)txbuf)[offset];
	case 2:
		return get_unaligned((const u16 *)((const u8 *)txbuf + offset));
	case 4:
		return get_unaligned((const u32 *)((const u8 *)txbuf + offset));
	default:
		return 0;
	}
}

static void ma35_spi_put_rx_word(void *rxbuf, unsigned int offset,
				 unsigned int bytes_per_word, u32 val)
{
	if (!rxbuf)
		return;

	switch (bytes_per_word) {
	case 1:
		((u8 *)rxbuf)[offset] = val;
		break;
	case 2:
		put_unaligned((u16)val, (u16 *)((u8 *)rxbuf + offset));
		break;
	case 4:
		put_unaligned(val, (u32 *)((u8 *)rxbuf + offset));
		break;
	}
}

static int ma35_spi_pio_transfer(struct ma35_spi *hw,
				 struct spi_transfer *xfer, u8 bpw)
{
	unsigned int bytes_per_word = ma35_spi_word_bytes(bpw);
	u32 data_mask = U32_MAX;
	unsigned int offset;
	u32 val;
	int ret;

	if (bpw < 32)
		data_mask = GENMASK(bpw - 1, 0);

	/*
	 * The controller is full duplex. TX-only transfers still generate RX
	 * data which must be drained, while RX-only transfers require dummy TX
	 * words to provide the serial clock.
	 */
	for (offset = 0; offset < xfer->len; offset += bytes_per_word) {
		ret = ma35_spi_wait_tx_not_full(hw);
		if (ret) {
			dev_err(hw->dev, "TX FIFO full timeout\n");
			return ret;
		}

		val = ma35_spi_get_tx_word(xfer->tx_buf, offset, bytes_per_word);
		ma35_spi_write(hw, MA35_SPI_TX, val & data_mask);

		ret = ma35_spi_wait_rx_not_empty(hw);
		if (ret) {
			dev_err(hw->dev, "RX FIFO empty timeout\n");
			return ret;
		}

		val = ma35_spi_read(hw, MA35_SPI_RX) & data_mask;
		ma35_spi_put_rx_word(xfer->rx_buf, offset, bytes_per_word, val);
	}

	ret = ma35_spi_wait_idle(hw);
	if (ret)
		dev_err(hw->dev, "PIO transfer did not complete\n");

	return ret;
}

static enum dma_slave_buswidth ma35_spi_dma_width(u8 bpw)
{
	switch (bpw) {
	case 8:
		return DMA_SLAVE_BUSWIDTH_1_BYTE;
	case 16:
		return DMA_SLAVE_BUSWIDTH_2_BYTES;
	case 32:
		return DMA_SLAVE_BUSWIDTH_4_BYTES;
	default:
		return DMA_SLAVE_BUSWIDTH_UNDEFINED;
	}
}

static bool ma35_spi_can_dma(struct spi_controller *ctlr,
			     struct spi_device *spi,
			     struct spi_transfer *xfer)
{
	struct ma35_spi *hw = spi_controller_get_devdata(ctlr);
	u8 bpw = xfer->bits_per_word ?: spi->bits_per_word;
	unsigned int align;

	if (!hw->dma_tx || !hw->dma_rx || xfer->len < MA35_SPI_DMA_MIN_BYTES)
		return false;

	if (!bpw)
		bpw = MA35_SPI_DEFAULT_BPW;

	switch (bpw) {
	case 8:
		align = 1;
		break;
	case 16:
		align = 2;
		break;
	case 32:
		align = 4;
		break;
	default:
		return false;
	}

	if (!IS_ALIGNED(xfer->len, align))
		return false;

	if (xfer->tx_buf && !IS_ALIGNED((unsigned long)xfer->tx_buf, align))
		return false;

	if (xfer->rx_buf && !IS_ALIGNED((unsigned long)xfer->rx_buf, align))
		return false;

	return true;
}

static void ma35_spi_dma_complete(void *arg)
{
	complete(arg);
}

static unsigned long ma35_spi_dma_timeout(struct spi_transfer *xfer)
{
	u32 speed_hz = xfer->effective_speed_hz ?: xfer->speed_hz;
	u64 ms;

	if (!speed_hz)
		speed_hz = 100000;

	ms = DIV_ROUND_UP_ULL((u64)xfer->len * 8 * MSEC_PER_SEC, speed_hz);
	ms = ms * 2 + 200;
	ms = max_t(u64, ms, 1000);
	ms = min_t(u64, ms, UINT_MAX);

	return msecs_to_jiffies((unsigned int)ms);
}

static int ma35_spi_config_dma(struct ma35_spi *hw,
			       enum dma_slave_buswidth width)
{
	struct dma_slave_config config = { };
	int ret;

	config.direction = DMA_DEV_TO_MEM;
	config.src_addr = hw->phys_base + MA35_SPI_RX;
	config.src_addr_width = width;
	config.src_maxburst = 1;
	config.peripheral_config = &hw->rx_peripheral;
	config.peripheral_size = sizeof(hw->rx_peripheral);

	ret = dmaengine_slave_config(hw->dma_rx, &config);
	if (ret) {
		dev_err(hw->dev, "failed to configure RX DMA: %d\n", ret);
		return ret;
	}

	memset(&config, 0, sizeof(config));
	config.direction = DMA_MEM_TO_DEV;
	config.dst_addr = hw->phys_base + MA35_SPI_TX;
	config.dst_addr_width = width;
	config.dst_maxburst = 1;
	config.peripheral_config = &hw->tx_peripheral;
	config.peripheral_size = sizeof(hw->tx_peripheral);

	ret = dmaengine_slave_config(hw->dma_tx, &config);
	if (ret)
		dev_err(hw->dev, "failed to configure TX DMA: %d\n", ret);

	return ret;
}

static void ma35_spi_dma_abort(struct ma35_spi *hw)
{
	ma35_spi_write(hw, MA35_SPI_PDMACTL, 0);
	ma35_spi_disable(hw);
	dmaengine_terminate_sync(hw->dma_tx);
	dmaengine_terminate_sync(hw->dma_rx);
	ma35_spi_reset_fifo(hw);
}

static int ma35_spi_dma_transfer(struct ma35_spi *hw,
				 struct spi_transfer *xfer, u8 bpw)
{
	struct dma_async_tx_descriptor *rxdesc;
	struct dma_async_tx_descriptor *txdesc;
	enum dma_slave_buswidth width;
	dma_cookie_t rx_cookie;
	dma_cookie_t tx_cookie;
	unsigned long timeout;
	int disable_ret;
	int ret;

	width = ma35_spi_dma_width(bpw);
	if (width == DMA_SLAVE_BUSWIDTH_UNDEFINED)
		return -EINVAL;

	ret = ma35_spi_config_dma(hw, width);
	if (ret)
		return ret;

	reinit_completion(&hw->dma_rx_done);
	reinit_completion(&hw->dma_tx_done);

	rxdesc = dmaengine_prep_slave_sg(hw->dma_rx,
					xfer->rx_sg.sgl, xfer->rx_sg.nents,
					DMA_DEV_TO_MEM,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!rxdesc)
		return -EIO;

	rxdesc->callback = ma35_spi_dma_complete;
	rxdesc->callback_param = &hw->dma_rx_done;
	rx_cookie = dmaengine_submit(rxdesc);
	ret = dma_submit_error(rx_cookie);
	if (ret)
		goto err_abort;

	txdesc = dmaengine_prep_slave_sg(hw->dma_tx,
					xfer->tx_sg.sgl, xfer->tx_sg.nents,
					DMA_MEM_TO_DEV,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!txdesc) {
		ret = -EIO;
		goto err_abort;
	}

	txdesc->callback = ma35_spi_dma_complete;
	txdesc->callback_param = &hw->dma_tx_done;
	tx_cookie = dmaengine_submit(txdesc);
	ret = dma_submit_error(tx_cookie);
	if (ret)
		goto err_abort;

	/* Arm both channels before enabling peripheral DMA requests. */
	dma_async_issue_pending(hw->dma_rx);
	dma_async_issue_pending(hw->dma_tx);

	ma35_spi_write(hw, MA35_SPI_PDMACTL,
			 MA35_SPI_PDMACTL_TXPDMAEN |
			 MA35_SPI_PDMACTL_RXPDMAEN);

	ret = ma35_spi_enable(hw);
	if (ret) {
		dev_err(hw->dev, "failed to enable controller for DMA\n");
		goto err_abort;
	}

	timeout = ma35_spi_dma_timeout(xfer);

	if (!wait_for_completion_timeout(&hw->dma_tx_done, timeout)) {
		dev_err(hw->dev, "TX DMA timeout\n");
		ret = -ETIMEDOUT;
		goto err_abort;
	}

	if (!wait_for_completion_timeout(&hw->dma_rx_done, timeout)) {
		dev_err(hw->dev, "RX DMA timeout\n");
		ret = -ETIMEDOUT;
		goto err_abort;
	}

	if (dma_async_is_tx_complete(hw->dma_tx, tx_cookie, NULL, NULL) !=
	    DMA_COMPLETE ||
	    dma_async_is_tx_complete(hw->dma_rx, rx_cookie, NULL, NULL) !=
	    DMA_COMPLETE) {
		dev_err(hw->dev, "DMA transfer completed with an error\n");
		ret = -EIO;
		goto err_abort;
	}

	ma35_spi_write(hw, MA35_SPI_PDMACTL, 0);

	ret = ma35_spi_wait_idle(hw);
	if (ret)
		dev_err(hw->dev, "SPI did not become idle after DMA\n");

	disable_ret = ma35_spi_disable(hw);
	if (disable_ret) {
		dev_err(hw->dev, "failed to disable controller after DMA\n");
		if (!ret)
			ret = disable_ret;
	}

	return ret;

err_abort:
	ma35_spi_dma_abort(hw);
	return ret;
}

static int ma35_spi_transfer_one(struct spi_controller *ctlr,
				 struct spi_device *spi,
				 struct spi_transfer *xfer)
{
	struct ma35_spi *hw = spi_controller_get_devdata(ctlr);
	u8 bpw;
	int disable_ret;
	int ret;

	if (!xfer->len)
		return 0;

	ret = ma35_spi_configure_transfer(hw, spi, xfer, &bpw);
	if (ret)
		return ret;

	if (ma35_spi_can_dma(ctlr, spi, xfer))
		return ma35_spi_dma_transfer(hw, xfer, bpw);

	ret = ma35_spi_enable(hw);
	if (ret) {
		dev_err(hw->dev, "failed to enable controller\n");
		goto out_disable;
	}

	ret = ma35_spi_pio_transfer(hw, xfer, bpw);

out_disable:
	disable_ret = ma35_spi_disable(hw);
	if (disable_ret) {
		dev_err(hw->dev, "failed to disable controller\n");
		if (!ret)
			ret = disable_ret;
	}

	return ret;
}

static void ma35_spi_set_cs_level(struct ma35_spi *hw, unsigned int cs,
				   bool assert)
{
	unsigned long flags;
	u32 mask;
	u32 val;

	switch (cs) {
	case 0:
		mask = MA35_SPI_SSCTL_SS0;
		break;
	case 1:
		mask = MA35_SPI_SSCTL_SS1;
		break;
	default:
		dev_warn(hw->dev, "invalid native chip select %u\n", cs);
		return;
	}

	spin_lock_irqsave(&hw->ssctl_lock, flags);

	val = ma35_spi_read(hw, MA35_SPI_SSCTL);
	if (assert)
		val |= mask;
	else
		val &= ~mask;
	ma35_spi_write(hw, MA35_SPI_SSCTL, val);

	spin_unlock_irqrestore(&hw->ssctl_lock, flags);
}

static int ma35_spi_setup(struct spi_device *spi)
{
	unsigned int cs = spi_get_chipselect(spi, 0);

	if (spi_get_csgpiod(spi, 0))
		return 0;

	if (cs >= MA35_SPI_MAX_NATIVE_CS) {
		dev_err(&spi->dev, "invalid native chip select %u\n", cs);
		return -EINVAL;
	}

	if (spi->mode & SPI_CS_HIGH) {
		dev_err(&spi->dev,
			"active-high native chip select is not supported\n");
		return -EINVAL;
	}

	return 0;
}

static void ma35_spi_set_cs(struct spi_device *spi, bool level)
{
	struct ma35_spi *hw = spi_controller_get_devdata(spi->controller);

	/*
	 * The SPI core passes the physical CS level to ->set_cs(). Native
	 * chip selects are active low.
	 */
	ma35_spi_set_cs_level(hw, spi_get_chipselect(spi, 0), !level);
}

static void ma35_spi_handle_err(struct spi_controller *ctlr,
				struct spi_message *message)
{
	struct ma35_spi *hw = spi_controller_get_devdata(ctlr);
	int ret;

	(void)message;

	ma35_spi_write(hw, MA35_SPI_PDMACTL, 0);
	ma35_spi_disable(hw);

	if (hw->dma_tx)
		dmaengine_terminate_sync(hw->dma_tx);
	if (hw->dma_rx)
		dmaengine_terminate_sync(hw->dma_rx);

	ret = ma35_spi_reset_fifo(hw);
	if (ret)
		dev_warn(hw->dev, "failed to reset FIFO after transfer error: %d\n",
			 ret);
}

static int ma35_spi_hw_init(struct ma35_spi *hw)
{
	u32 ctl_mask;
	u32 fifo_mask;
	u32 ssctl_mask;
	int ret;

	ret = ma35_spi_disable(hw);
	if (ret)
		return ret;

	ctl_mask = MA35_SPI_CTL_DATDIR |
		   MA35_SPI_CTL_REORDER |
		   MA35_SPI_CTL_SLAVE |
		   MA35_SPI_CTL_UNITIEN |
		   MA35_SPI_CTL_RXONLY |
		   MA35_SPI_CTL_HALFDPX |
		   MA35_SPI_CTL_LSB |
		   MA35_SPI_CTL_DWIDTH_MASK |
		   MA35_SPI_CTL_SUSPITV_MASK |
		   MA35_SPI_CTL_CLKPOL |
		   MA35_SPI_CTL_TXNEG |
		   MA35_SPI_CTL_RXNEG;

	ma35_spi_update_bits(hw, MA35_SPI_CTL, ctl_mask,
			       MA35_SPI_CTL_TXNEG |
			       FIELD_PREP(MA35_SPI_CTL_DWIDTH_MASK,
					  MA35_SPI_DEFAULT_BPW));

	ssctl_mask = MA35_SPI_SSCTL_SS0 |
		     MA35_SPI_SSCTL_SS1 |
		     MA35_SPI_SSCTL_SSACTPOL |
		     MA35_SPI_SSCTL_AUTOSS |
		     MA35_SPI_SSCTL_SLV3WIRE |
		     MA35_SPI_SSCTL_SLVBEIEN |
		     MA35_SPI_SSCTL_SLVURIEN |
		     MA35_SPI_SSCTL_SSACTIEN |
		     MA35_SPI_SSCTL_SSINAIEN;
	ma35_spi_update_ssctl_bits(hw, ssctl_mask, 0);

	ma35_spi_write(hw, MA35_SPI_PDMACTL, 0);

	fifo_mask = MA35_SPI_FIFOCTL_SLVBERX |
		    MA35_SPI_FIFOCTL_TXUFIEN |
		    MA35_SPI_FIFOCTL_TXUFPOL |
		    MA35_SPI_FIFOCTL_RXOVIEN |
		    MA35_SPI_FIFOCTL_RXTOIEN |
		    MA35_SPI_FIFOCTL_TXTHIEN |
		    MA35_SPI_FIFOCTL_RXTHIEN;
	ma35_spi_update_bits(hw, MA35_SPI_FIFOCTL, fifo_mask, 0);

	ret = ma35_spi_reset_fifo(hw);
	if (ret)
		return ret;

	/* Keep the controller disabled until the SPI core starts a transfer. */
	return 0;
}

static void ma35_spi_hw_shutdown(void *data)
{
	struct ma35_spi *hw = data;

	ma35_spi_write(hw, MA35_SPI_PDMACTL, 0);
	if (hw->dma_tx)
		dmaengine_terminate_sync(hw->dma_tx);
	if (hw->dma_rx)
		dmaengine_terminate_sync(hw->dma_rx);
	ma35_spi_disable(hw);
}

static void ma35_spi_release_dma(void *data)
{
	struct ma35_spi *hw = data;

	if (hw->dma_tx) {
		dma_release_channel(hw->dma_tx);
		hw->dma_tx = NULL;
	}

	if (hw->dma_rx) {
		dma_release_channel(hw->dma_rx);
		hw->dma_rx = NULL;
	}
}

static int ma35_spi_request_dma(struct spi_controller *ctlr,
				struct ma35_spi *hw)
{
	struct device *dev = hw->dev;
	int ret;

	if (!device_property_present(dev, "dmas"))
		return 0;

	hw->dma_tx = dma_request_chan(dev, "tx");
	if (IS_ERR(hw->dma_tx)) {
		ret = PTR_ERR(hw->dma_tx);
		hw->dma_tx = NULL;
		return dev_err_probe(dev, ret, "failed to request TX DMA channel\n");
	}

	hw->dma_rx = dma_request_chan(dev, "rx");
	if (IS_ERR(hw->dma_rx)) {
		ret = PTR_ERR(hw->dma_rx);
		hw->dma_rx = NULL;
		dma_release_channel(hw->dma_tx);
		hw->dma_tx = NULL;
		return dev_err_probe(dev, ret, "failed to request RX DMA channel\n");
	}

	ret = device_property_read_u32(dev, "nuvoton,pdma-reqsel-tx",
				       &hw->tx_peripheral.reqsel);
	if (ret) {
		ret = dev_err_probe(dev, ret, "missing TX PDMA request selector\n");
		goto err_release;
	}

	ret = device_property_read_u32(dev, "nuvoton,pdma-reqsel-rx",
				       &hw->rx_peripheral.reqsel);
	if (ret) {
		ret = dev_err_probe(dev, ret, "missing RX PDMA request selector\n");
		goto err_release;
	}

	if (hw->tx_peripheral.reqsel > 0xff || hw->rx_peripheral.reqsel > 0xff) {
		ret = dev_err_probe(dev, -EINVAL, "invalid PDMA request selector\n");
		goto err_release;
	}

	init_completion(&hw->dma_tx_done);
	init_completion(&hw->dma_rx_done);

	ret = devm_add_action_or_reset(dev, ma35_spi_release_dma, hw);
	if (ret)
		return ret;

	ctlr->dma_tx = hw->dma_tx;
	ctlr->dma_rx = hw->dma_rx;
	ctlr->can_dma = ma35_spi_can_dma;
	ctlr->max_dma_len = MA35_SPI_MAX_DMA_SEGMENT;
	ctlr->flags |= SPI_CONTROLLER_MUST_TX | SPI_CONTROLLER_MUST_RX;

	return 0;

err_release:
	dma_release_channel(hw->dma_rx);
	dma_release_channel(hw->dma_tx);
	hw->dma_rx = NULL;
	hw->dma_tx = NULL;

	return ret;
}

static void ma35_spi_assert_reset(void *data)
{
	reset_control_assert(data);
}

static int ma35_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct reset_control *rst;
	struct ma35_spi *hw;
	struct resource *res;
	unsigned long clk_rate;
	unsigned int max_divisor;
	u32 num_cs = MA35_SPI_DEFAULT_NUM_CS;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*hw));
	if (!ctlr)
		return -ENOMEM;

	hw = spi_controller_get_devdata(ctlr);
	hw->dev = dev;
	spin_lock_init(&hw->ssctl_lock);

	hw->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(hw->regs))
		return dev_err_probe(dev, PTR_ERR(hw->regs), "failed to map registers\n");
	hw->phys_base = res->start;

	hw->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(hw->clk))
		return dev_err_probe(dev, PTR_ERR(hw->clk),
				     "failed to get and enable clock\n");

	rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "failed to get reset\n");

	if (rst) {
		ret = reset_control_deassert(rst);
		if (ret)
			return dev_err_probe(dev, ret, "failed to deassert reset\n");

		ret = devm_add_action_or_reset(dev, ma35_spi_assert_reset, rst);
		if (ret)
			return ret;

		/* Hardware requires several peripheral clocks after reset release. */
		udelay(1);
	}

	clk_rate = clk_get_rate(hw->clk);
	if (!clk_rate || clk_rate > U32_MAX)
		return dev_err_probe(dev, -EINVAL, "invalid SPI clock rate %lu\n",
				     clk_rate);

	ret = device_property_read_u32(dev, "num-cs", &num_cs);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret, "failed to read num-cs\n");

	if (!num_cs)
		return dev_err_probe(dev, -EINVAL, "invalid num-cs %u\n", num_cs);

	ctlr->num_chipselect = num_cs;
	ctlr->max_native_cs = MA35_SPI_MAX_NATIVE_CS;
	ctlr->use_gpio_descriptors = true;
	ctlr->setup = ma35_spi_setup;
	ctlr->set_cs = ma35_spi_set_cs;
	ctlr->transfer_one = ma35_spi_transfer_one;
	ctlr->handle_err = ma35_spi_handle_err;
	ctlr->bits_per_word_mask = SPI_BPW_RANGE_MASK(8, 32);
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_LSB_FIRST;
	ctlr->min_speed_hz = DIV_ROUND_UP(clk_rate, MA35_SPI_MAX_DIVISOR);
	ctlr->dev.of_node = dev->of_node;

	ret = ma35_spi_calc_divisor(clk_rate, MA35_SPI_MAX_SPEED_HZ,
				    &max_divisor);
	if (ret)
		return dev_err_probe(dev, ret,
				     "clock rate does not support SPI transfers\n");
	ctlr->max_speed_hz = clk_rate / max_divisor;

	ret = ma35_spi_request_dma(ctlr, hw);
	if (ret)
		return ret;

	ret = ma35_spi_hw_init(hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize controller\n");

	ret = devm_add_action_or_reset(dev, ma35_spi_hw_shutdown, hw);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, ctlr);

	ret = devm_spi_register_controller(dev, ctlr);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register SPI controller\n");

	dev_info(dev, "SPI controller registered, parent %lu Hz, max %u Hz, %s\n",
		 clk_rate, ctlr->max_speed_hz,
		 hw->dma_tx && hw->dma_rx ? "PDMA enabled" : "PIO only");

	return 0;
}

static const struct of_device_id ma35_spi_of_match[] = {
	{ .compatible = "nuvoton,ma35d0-spi" },
	{ .compatible = "nuvoton,ma35d1-spi" },
	{ .compatible = "nuvoton,ma35h0-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, ma35_spi_of_match);

static struct platform_driver ma35_spi_driver = {
	.probe = ma35_spi_probe,
	.driver = {
		.name = "ma35d1-spi",
		.of_match_table = ma35_spi_of_match,
	},
};
module_platform_driver(ma35_spi_driver);

MODULE_AUTHOR("Chi-Wen Weng <cwweng@nuvoton.com>");
MODULE_DESCRIPTION("Nuvoton MA35 Series SPI controller driver");
MODULE_LICENSE("GPL");
