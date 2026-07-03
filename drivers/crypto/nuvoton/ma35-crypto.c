// SPDX-License-Identifier: GPL-2.0
/*
 * linux/driver/crypto/nuvoton/ma35-crypto.c
 *
 * Copyright (c) 2025 Nuvoton technology corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;version 2 of the License.
 *
 */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/crypto.h>
#include <linux/spinlock.h>
#include <crypto/algapi.h>

#include <linux/io.h>
#include <linux/clk.h>

#include "ma35-crypto.h"

static struct nu_crypto_dev *_ma35_crypto_dev;

static irqreturn_t ma35_crypto_irq(int irq, void *data)
{
	struct nu_crypto_dev  *crypto_dev = (struct nu_crypto_dev *)data;
	struct nu_aes_dev  *aes_dd = &crypto_dev->aes_dd;
	struct nu_sha_dev  *sha_dd = &crypto_dev->sha_dd;
	u32  status, aes_sts, ret = IRQ_NONE;

	status = readl_relaxed(crypto_dev->reg_base + INTSTS);
	if (status & (INTSTS_AESIF | INTSTS_AESEIF)) {
		aes_sts = readl_relaxed(crypto_dev->reg_base + AES_STS);
		if (aes_sts & (AES_STS_KSERR | AES_STS_BUSERR))
			pr_err("AES H/W error: AES_STS = 0x%x!\n", aes_sts);

		writel_relaxed(INTSTS_AESIF | INTSTS_AESEIF, crypto_dev->reg_base + INTSTS);

		tasklet_schedule(&aes_dd->done_task);

		ret = IRQ_HANDLED;
	}

	if (status & (INTSTS_HMACIF | INTSTS_HMACEIF)) {
		writel_relaxed(INTSTS_HMACIF | INTSTS_HMACEIF, crypto_dev->reg_base + INTSTS);

		tasklet_schedule(&sha_dd->done_task);

		ret = IRQ_HANDLED;
	}

	return ret;
}

static int ma35_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nu_crypto_dev *crypto_dev;
	struct resource *res;
	int   irq;
	const char  *str;
	int   err;

	/* MA35D1 crypto engine clock should be enabled by firmware. */

	crypto_dev = devm_kzalloc(dev, sizeof(*crypto_dev), GFP_KERNEL);
	if (!crypto_dev)
		return -ENOMEM;

	dev_set_drvdata(dev, crypto_dev);
	_ma35_crypto_dev = crypto_dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	crypto_dev->reg_base = devm_ioremap_resource(dev, res);
	if (IS_ERR(crypto_dev->reg_base)) {
		err = PTR_ERR(crypto_dev->reg_base);
		return err;
	}

	/*
	 *  Get irq number and install irq handler
	 */
	irq = platform_get_irq(pdev, 0);
	if (irq <= 0) {
		dev_err(dev, "Failed to get ma35 crypto irq!\n");
		return -ENODEV;
	}
	err = devm_request_irq(dev, irq, ma35_crypto_irq, IRQF_SHARED,
			       "ma35-crypto", crypto_dev);
	if (err) {
		dev_err(dev, "Failed to request IRQ%d: err: %d.\n", irq, err);
		return err;
	}

	err = ma35_prng_probe(dev, crypto_dev->reg_base, &crypto_dev->prng);
	if (err)
		dev_err(dev, "failed to init ma35-prng!\n");

	err = ma35_aes_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-aes!\n");

	err = ma35_sha_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-sha!\n");

	crypto_dev->ecc_ioctl = false;
	if (!of_property_read_string(dev->of_node, "ecc_ioctl", &str)) {
		if (!strcmp("yes", str))
			crypto_dev->ecc_ioctl = true;
	}
	err = ma35_ecc_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-ecc!\n");

	crypto_dev->rsa_ioctl = false;
	if (!of_property_read_string(dev->of_node, "rsa_ioctl", &str)) {
		if (!strcmp("yes", str))
			crypto_dev->rsa_ioctl = true;
	}
	err = ma35_rsa_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-rsa!\n");

	return 0;
}

static int ma35_crypto_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nu_crypto_dev *crypto_dev;

	crypto_dev = dev_get_drvdata(dev);
	if (!crypto_dev)
		return -ENODEV;

	ma35_prng_remove(&pdev->dev, crypto_dev->prng);
	ma35_aes_remove(&pdev->dev, crypto_dev);
	ma35_sha_remove(&pdev->dev, crypto_dev);
	ma35_ecc_remove(&pdev->dev, crypto_dev);
	ma35_rsa_remove(&pdev->dev, crypto_dev);

	devm_kfree(dev, crypto_dev);

	return 0;
}

static const struct of_device_id ma35_crypto_of_match[] = {
	{ .compatible = "nuvoton,ma35d0-crypto" },
	{ .compatible = "nuvoton,ma35h0-crypto" },
	{},
};
MODULE_DEVICE_TABLE(of, ma35_crypto_of_match);


static struct platform_driver ma35_crypto_driver = {
	.probe  = ma35_crypto_probe,
	.remove = ma35_crypto_remove,
	.driver = {
		.name = "ma35-crypto",
		.owner = THIS_MODULE,
		.of_match_table = ma35_crypto_of_match,
	},
};

module_platform_driver(ma35_crypto_driver);

MODULE_AUTHOR("Nuvoton Technology Corporation");
MODULE_DESCRIPTION("Nuvoton Cryptographic Accelerator");
MODULE_LICENSE("GPL");
