// SPDX-License-Identifier: GPL-2.0
/*
 * linux/driver/crypto/nuvoton/nuvoton-crypto-optee.c
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
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/tee_drv.h>
#include <linux/crypto.h>
#include <linux/spinlock.h>
#include <crypto/algapi.h>

#include "ma35-crypto.h"

static struct nu_crypto_dev *_ma35_crypto_optee_dev;

static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	if (ver->impl_id == TEE_IMPL_ID_OPTEE)
		return 1;
	else
		return 0;
}

static int optee_crypto_probe(struct device *dev)
{
	struct nu_crypto_dev *crypto_dev;
	struct tee_client_device *tee_cdev; // = to_tee_client_device(dev);
	struct tee_context *ctx;
	u32 session_id;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_ioctl_open_session_arg sess_arg;
	struct tee_param param[4];
	int ret;

	crypto_dev = _ma35_crypto_optee_dev;

	tee_cdev = to_tee_client_device(dev);
	crypto_dev->tee_cdev = tee_cdev;

	/*
	 * Open context with TEE driver
	 */
	ctx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(ctx)) {
		dev_err(dev, "tee_client_open_context failed.\n");
		return -ENODEV;
	}

	/*
	 * Open session with Crypto Trusted App
	 */
	memcpy(sess_arg.uuid, tee_cdev->id.uuid.b, TEE_IOCTL_UUID_LEN);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	sess_arg.num_params = 0;
	ret = tee_client_open_session(ctx, &sess_arg, NULL);
	if ((ret < 0) || (sess_arg.ret != 0)) {
		dev_err(dev, "tee_client_open_session failed, err: %x\n",
			sess_arg.ret);
		tee_client_close_context(ctx);
		return -EINVAL;
	}
	session_id = sess_arg.session;

	/*
	 * Invoke PTA_CMD_CRYPTO_INIT function of Trusted App
	 */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));
	inv_arg.func = PTA_CMD_CRYPTO_INIT;
	inv_arg.session = session_id;
	inv_arg.num_params = 4;
	ret = tee_client_invoke_func(ctx, &inv_arg, param);
	if ((ret < 0) || (inv_arg.ret != 0)) {
		dev_err(dev, "PTA_CMD_CRYPTO_INIT invoke err: %x\n",
			inv_arg.ret);
		ret = -EINVAL;
	}
	tee_client_close_session(ctx, session_id);
	tee_client_close_context(ctx);
	return ret;
}

static int optee_crypto_remove(struct device *dev)
{
	return 0;
}

static const struct tee_client_device_id optee_crypto_id_table[] = {
	{ UUID_INIT(0x61d3c750, 0x9e72, 0x46b6,
		    0x85, 0x7c, 0x46, 0xfa, 0x51, 0x27, 0x32, 0xac) },
	{}
};
MODULE_DEVICE_TABLE(tee, optee_crypto_id_table);

static struct tee_client_driver optee_crypto_driver = {
	.id_table	= optee_crypto_id_table,
	.driver		= {
		.name		= "optee-ma35-crypto",
		.bus		= &tee_bus_type,
		.probe		= optee_crypto_probe,
		.remove		= optee_crypto_remove,
	},
};

int ma35_crypto_optee_init(struct nu_crypto_dev *crypto_dev)
{
	int err;

	if (crypto_dev->tee_cdev != NULL)
		return 0; /* already inited */

	pr_info("Register MA35D1 Crypto optee client driver.\n");
	err = driver_register(&optee_crypto_driver.driver);
	if (err) {
		pr_err("Failed to register crypto optee driver!\n");
		return err;
	}
	return 0;
}

static int ma35_crypto_optee_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nu_crypto_dev *crypto_dev;
	int err;

	crypto_dev = devm_kzalloc(dev, sizeof(*crypto_dev), GFP_KERNEL);
	if (!crypto_dev)
		return -ENOMEM;

	dev_set_drvdata(dev, crypto_dev);

	_ma35_crypto_optee_dev = crypto_dev;

	crypto_dev->ecc_ioctl = true;
	crypto_dev->rsa_ioctl = true;

	err = ma35_aes_optee_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-aes-optee!\n");

	err = ma35_sha_optee_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-sha-optee!\n");

	err = ma35_ecc_optee_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-ecc-optee!\n");

	err = ma35_rsa_optee_probe(dev, crypto_dev);
	if (err)
		dev_err(dev, "failed to init ma35-rsa-optee!\n");

	return 0;
}

static int ma35_crypto_optee_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nu_crypto_dev *crypto_dev;

	crypto_dev = dev_get_drvdata(dev);
	if (!crypto_dev)
		return -ENODEV;

	ma35_aes_optee_remove(&pdev->dev, crypto_dev);
	ma35_sha_optee_remove(&pdev->dev, crypto_dev);
	ma35_ecc_optee_remove(&pdev->dev, crypto_dev);
	ma35_rsa_optee_remove(&pdev->dev, crypto_dev);

	return 0;
}

static const struct of_device_id ma35_crypto_optee_of_match[] = {
	{ .compatible = "nuvoton,ma35d1-crypto-optee" },
	{ .compatible = "nuvoton,ma35d0-crypto-optee" },
	{ .compatible = "nuvoton,ma35h0-crypto-optee" },
	{},
};
MODULE_DEVICE_TABLE(of, ma35_crypto_optee_of_match);

static struct platform_driver ma35_crypto_optee_driver = {
	.probe  = ma35_crypto_optee_probe,
	.remove = ma35_crypto_optee_remove,
	.driver = {
		.name = "ma35-crypto-optee",
		.owner = THIS_MODULE,
		.of_match_table = ma35_crypto_optee_of_match,
	},
};

static int __init ma35_crypto_platform_driver_init(void)
{
	return platform_driver_register(&ma35_crypto_optee_driver);
}

static void __exit ma35_crypto_platform_driver_exit(void)
{
	platform_driver_unregister(&ma35_crypto_optee_driver);
}

late_initcall(ma35_crypto_platform_driver_init);
module_exit(ma35_crypto_platform_driver_exit);

MODULE_AUTHOR("Nuvoton Technology Corporation");
MODULE_DESCRIPTION("Nuvoton Cryptographic Accelerator OP-TEE");
MODULE_LICENSE("GPL");
