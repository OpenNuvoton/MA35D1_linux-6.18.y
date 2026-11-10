/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Header file for the MA35D1 EBI driver
 *
 * Copyright (C) 2020 Nuvoton Technology corporation
 */

#ifndef __REGS_MA35D1_EBI_H__
#define __REGS_MA35D1_EBI_H__

/* External Bus Interface Bank0~2 Control Register */
#define REG_EBI_CTL		(0x00)
/* External Bus Interface Bank0~2 Timing Control Register */
#define REG_EBI_TCTL		(0x04)

#define EBI_BANK0_BASE_ADDR	0x68000000UL /*!< EBI bank0 base address */
#define EBI_BANK1_BASE_ADDR	0x68100000UL /*!< EBI bank1 base address */
#define EBI_BANK2_BASE_ADDR	0x68200000UL /*!< EBI bank2 base address */

#define EBI_BANK0		0UL    /*!< EBI bank 0 */
#define EBI_BANK1		1UL    /*!< EBI bank 1 */
#define EBI_BANK2		2UL    /*!< EBI bank 2 */

#define EBI_BUSWIDTH_8BIT	0UL    /*!< EBI bus width is 8-bit */
#define EBI_BUSWIDTH_16BIT	1UL    /*!< EBI bus width is 16-bit */

#define EBI_CS_ACTIVE_LOW	0UL    /*!< EBI CS active level is low */
#define EBI_CS_ACTIVE_HIGH	1UL    /*!< EBI CS active level is high */

#define EBI_MCLKDIV_1		0x0UL /*!< EBI output clock(MCLK) is HCLK/1 */
#define EBI_MCLKDIV_2		0x1UL /*!< EBI output clock(MCLK) is HCLK/2 */
#define EBI_MCLKDIV_4		0x2UL /*!< EBI output clock(MCLK) is HCLK/4 */
#define EBI_MCLKDIV_8		0x3UL /*!< EBI output clock(MCLK) is HCLK/8 */
#define EBI_MCLKDIV_16		0x4UL /*!< EBI output clock(MCLK) is HCLK/16 */
#define EBI_MCLKDIV_32		0x5UL /*!< EBI output clock(MCLK) is HCLK/32 */
#define EBI_MCLKDIV_64		0x6UL /*!< EBI output clock(MCLK) is HCLK/64 */
#define EBI_MCLKDIV_128		0x7UL /*!< EBI output clock(MCLK) is HCLK/128 */

#define EBI_OPMODE_NORMAL	0x0UL /*!< EBI bus operate in normal mode */
#define EBI_OPMODE_ADSEPARATE	0x1UL /*!< EBI bus operate in AD Separate mode */
/*!< EBI bus operate in Continuous Data Access mode */
#define EBI_OPMODE_CACCESS	(EBI_CTL_CACCESS_MSK)

#define EBI_CTL_EN_POS		(0)
#define EBI_CTL_EN_MSK		(0x1ul << EBI_CTL_EN_POS)

#define EBI_CTL_DW16_POS	(1)
#define EBI_CTL_DW16_MSK	(0x1ul << EBI_CTL_DW16_POS)

#define EBI_CTL_CSPOLINV_POS	(2)
#define EBI_CTL_CSPOLINV_MSK	(0x1ul << EBI_CTL_CSPOLINV_POS)

#define EBI_CTL_ADSEPEN_POS	(3)
#define EBI_CTL_ADSEPEN_MSK	(0x1ul << EBI_CTL_ADSEPEN_POS)

#define EBI_CTL_CACCESS_POS	(4)
#define EBI_CTL_CACCESS_MSK	(0x1ul << EBI_CTL_CACCESS_POS)

#define EBI_CTL_MCLKDIV_POS	(8)
#define EBI_CTL_MCLKDIV_MSK	(0x7ul << EBI_CTL_MCLKDIV_POS)

#define EBI_CTL_TALE_POS	(16)
#define EBI_CTL_TALE_MSK	(0x7ul << EBI_CTL_TALE_POS)

#define EBI_CTL_WBUFEN_POS	(24)
#define EBI_CTL_WBUFEN_MSK	(0x1ul << EBI_CTL_WBUFEN_POS)

#define EBI_TCTL_TACC_POS	(3)
#define EBI_TCTL_TACC_MSK	(0x1ful << EBI_TCTL_TACC_POS)

#define EBI_TCTL_TAHD_POS	(8)
#define EBI_TCTL_TAHD_MSK	(0x7ul << EBI_TCTL_TAHD_POS)

#define EBI_TCTL_W2X_POS	(12)
#define EBI_TCTL_W2X_MSK	(0xful << EBI_TCTL_W2X_POS)

#define EBI_TCTL_RAHDOFF_POS	(22)
#define EBI_TCTL_RAHDOFF_MSK	(0x1ul << EBI_TCTL_RAHDOFF_POS)

#define EBI_TCTL_WAHDOFF_POS	(23)
#define EBI_TCTL_WAHDOFF_MSK	(0x1ul << EBI_TCTL_WAHDOFF_POS)

#define EBI_TCTL_R2R_POS	(24)
#define EBI_TCTL_R2R_MSK	(0xful << EBI_TCTL_R2R_POS)

#endif /* __REGS_MA35D1_EBI_H__ */
