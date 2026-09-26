// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35 series key store op-tee driver
 *
 * Copyright (c) 2025 Nuvoton technology corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;version 2 of the License.
 *
 */

#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of_platform.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/compat.h>
#include <linux/highmem.h>
#include <linux/tee_drv.h>

#include <uapi/misc/ma35_kstore.h>

#define MISCDEV_NAME		"ksdev"

#define KS_BUSY_TIMEOUT		1000

static char  ks_miscdev_name[] = MISCDEV_NAME;

static uint16_t au8SRAMCntTbl[21] = {4, 6, 6, 7, 8, 8, 8, 9, 12, 13, 16, 17,
				     18, 0, 0, 0, 32, 48, 64, 96, 128};
static uint16_t au8OTPCntTbl[7] = {4, 6, 6, 7, 8, 8, 8};

#define TEE_ERROR_KS_BUSY		0x00000001
#define TEE_ERROR_KS_FAIL		0x00000002
#define TEE_ERROR_KS_INVALID		0x00000003
#define TEE_ERROR_OTP_INVALID		0x00000011
#define TEE_ERROR_OTP_FAIL		0x00000012

/*
 * PTA_CMD_KS_INIT - Initialize Key Store
 *
 * param[0] unused
 * param[1] unused
 * param[2] unused
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_BAD_PARAMETERS - Incorrect input param
 * TEE_ERROR_KS_FAIL - Initialization failed
 */
#define PTA_CMD_KS_INIT			0x1

/*
 * PTA_CMD_KS_READ - Read a Key Store key
 *
 * param[0] (in value) - value.a: 0: SRAM; 2: OTP
 *                       value.b: key number
 * param[1] (inout memref) - memref.size: word count of the key
 *                           memref.buffer: key buffer
 * param[2] unused
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_KS_INVALID - Incorrect input param
 * TEE_ERROR_KS_FAIL - Read operation failed
 */
#define PTA_CMD_KS_READ			0x2

/*
 * PTA_CMD_KS_WRITE - Write a Key Store key
 *
 * param[0] (in value) - value.a: 0: SRAM; 2: OTP
 *                       value.b: meta data
 * param[1] (inout memref) - memref.size: word count of the key
 *                           memref.buffer: key buffer
 * param[2] (out value) - value.a: key number
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_KS_INVALID - Invalid parameter
 * TEE_ERROR_KS_FAIL - Write operation failed
 */
#define PTA_CMD_KS_WRITE		0x3

/*
 * PTA_CMD_KS_ERASE - Erase a Key Store key
 *
 * param[0] (in value) - value.a: 0: SRAM; 2: OTP
 *                       value.b: key number
 * param[1] unused
 * param[2] unused
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_KS_INVALID - Incorrect input param
 * TEE_ERROR_KS_FAIL - Erase operation failed
 */
#define PTA_CMD_KS_ERASE		0x4

/*
 * PTA_CMD_KS_ERASE_ALL - Erase all Key Store SRAM keys
 *
 * param[0] unused
 * param[1] unused
 * param[2] unused
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_KS_FAIL - Erase all operation failed
 */
#define PTA_CMD_KS_ERASE_ALL		0x5

/*
 * PTA_CMD_KS_REVOKE - Revoke a Key Store key
 *
 * param[0] (in value) - value.a: 0: SRAM; 2: OTP
 *                       value.b: key number
 * param[1] unused
 * param[2] unused
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_KS_INVALID - Incorrect input param
 * TEE_ERROR_KS_FAIL - Revoke operation failed
 */
#define PTA_CMD_KS_REVOKE		0x6

/*
 * PTA_CMD_KS_REMAIN - Get the remaining size of Key Store SRAM
 *
 * param[0] (out value) - value.a: remaining size of SRAM
 * param[1] unused
 * param[2] unused
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_KS_FAIL - Get remain operation failed
 */
#define PTA_CMD_KS_REMAIN		0x7

/*
 * PTA_CMD_OTP_READ - Read OTP
 *
 * param[0] (in value) - value.a: OTP address
 * param[1] (inout memref) - memref.size: OTP buffer size in bytes
 *                           memref.buffer: key buffer
 * param[2] unused
 * param[3] unused
 *
 * Result:
 * TEE_SUCCESS - Invoke command success
 * TEE_ERROR_OTP_INVALID - Incorrect input param
 * TEE_ERROR_OTP_FAIL - read OTP failed
 */
#define PTA_CMD_OTP_READ		0x12
#define PTA_CMD_OTP_PROGRAM		0x13

#define KS_DATA_SHM_SZ			1024

/**
 * struct optee_ks_private - OP-TEE private data of ma35 key store
 * @dev:		OP-TEE based device.
 * @ctx:		OP-TEE context handler.
 * @session_id:		KS TA session identifier.
 * @ks_shm_pool:	Memory pool shared with ma35 key store op-tee pta
 * @miscdev:		Linux misc device data
 */
struct optee_ks_private {
	struct device *dev;
	struct tee_context *ctx;
	u32 session_id;
	struct tee_shm *ks_shm_pool;
	u8 *va_shm;
	struct miscdevice miscdev;
};

static struct optee_ks_private pvt_data;

#define to_optee_ks_private(r)   container_of(r, struct optee_ks_private, miscdev)

static int optee_ks_read(struct optee_ks_private *ks_priv, void __user *arg)
{
	struct ks_read_args r_args;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int err;

	err = copy_from_user(&r_args, arg, sizeof(r_args));
	if (err)
		return -EFAULT;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_READ function of Trusted App */
	inv_arg.func = PTA_CMD_KS_READ;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

	param[0].u.value.a = r_args.type;
	param[0].u.value.b = r_args.key_idx;
	param[1].u.memref.shm = ks_priv->ks_shm_pool;
	param[1].u.memref.size = r_args.word_cnt;
	param[1].u.memref.shm_offs = 0;

	err = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((err < 0) || (inv_arg.ret != 0))
		return -EINVAL;

	memcpy((u8 *)r_args.key, ks_priv->va_shm, r_args.word_cnt * 4);

	return copy_to_user(arg, &r_args, sizeof(r_args));
}

static int optee_ks_write_sram(struct optee_ks_private *ks_priv, void __user *arg)
{
	struct ks_write_args w_args;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int sidx, key_wcnt;
	int err;

	err = copy_from_user(&w_args, arg, sizeof(w_args));
	if (err)
		return -EFAULT;

	/* Get size index */
	sidx = ((w_args.meta_data & KS_METADATA_SIZE_Msk) >> KS_METADATA_SIZE_Pos);

	key_wcnt = au8SRAMCntTbl[sidx];

	memcpy(ks_priv->va_shm, (u8 *)w_args.key, key_wcnt * 4);

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_WRITE function of Trusted App */
	inv_arg.func = PTA_CMD_KS_WRITE;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	param[0].u.value.a = KS_SRAM;
	param[0].u.value.b = w_args.meta_data | KS_TOMETAKEY(w_args.key_idx);
	param[1].u.memref.shm = ks_priv->ks_shm_pool;
	param[1].u.memref.size = key_wcnt;
	param[1].u.memref.shm_offs = 0;

	err = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((err < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_KS_WRITE invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}
	return param[2].u.value.a;
}

static int optee_ks_write_otp(struct optee_ks_private *ks_priv, void __user *arg)
{
	struct ks_write_args w_args;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int sidx, key_wcnt;
	int err;

	err = copy_from_user(&w_args, arg, sizeof(w_args));
	if (err)
		return -EFAULT;

	/* Get size index */
	sidx = ((w_args.meta_data & KS_METADATA_SIZE_Msk) >> KS_METADATA_SIZE_Pos);

	/* OTP only support maximum 256 bits */
	if (sidx >= 7)
		return -EINVAL;

	key_wcnt = au8OTPCntTbl[sidx];

	memcpy(ks_priv->va_shm, (u8 *)w_args.key, key_wcnt * 4);

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_WRITE function of Trusted App */
	inv_arg.func = PTA_CMD_KS_WRITE;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
	param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	param[0].u.value.a = KS_OTP;
	param[0].u.value.b = w_args.meta_data | KS_TOMETAKEY(w_args.key_idx);
	param[1].u.memref.shm = ks_priv->ks_shm_pool;
	param[1].u.memref.size = key_wcnt;
	param[1].u.memref.shm_offs = 0;

	err = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((err < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_KS_WRITE invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}
	return 0;
}

static int optee_ks_erase(struct optee_ks_private *ks_priv, void __user *arg)
{
	struct ks_kidx_args k_args;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret;

	ret = copy_from_user(&k_args, arg, sizeof(k_args));
	if (ret)
		return -EFAULT;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_ERASE function of Trusted App */
	inv_arg.func = PTA_CMD_KS_ERASE;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;

	param[0].u.value.a = k_args.type;
	param[0].u.value.b = k_args.key_idx;

	ret = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((ret < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_KS_ERASE invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}
	return 0;
}

static int optee_ks_erase_all(struct optee_ks_private *ks_priv)
{
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_ERASE_ALL function of Trusted App */
	inv_arg.func = PTA_CMD_KS_ERASE_ALL;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	ret = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((ret < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_KS_ERASE_ALL invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}

	return 0;
}

static int optee_ks_revoke(struct optee_ks_private *ks_priv, void __user *arg)
{
	struct ks_kidx_args k_args;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret;

	ret = copy_from_user(&k_args, arg, sizeof(k_args));
	if (ret)
		return -EFAULT;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_REVOKE function of Trusted App */
	inv_arg.func = PTA_CMD_KS_REVOKE;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;

	param[0].u.value.a = k_args.type;
	param[0].u.value.b = k_args.key_idx;

	ret = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((ret < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_KS_REVOKE invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}

	return 0;
}

static int optee_ks_remain(struct optee_ks_private *ks_priv)
{
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_REMAIN function of Trusted App */
	inv_arg.func = PTA_CMD_KS_REMAIN;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	ret = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((ret < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_KS_REMAIN invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}

	return param[0].u.value.a;
}

static int optee_ks_read_otp(struct optee_ks_private *ks_priv, void __user *arg)
{
	struct ks_read_args r_args;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret;

	ret = copy_from_user(&r_args, arg, sizeof(r_args));
	if (ret)
		return -EFAULT;
	if (r_args.word_cnt <= 0 ||
	    r_args.word_cnt > (int)ARRAY_SIZE(r_args.key) ||
	    r_args.key_idx < 0x100 || r_args.key_idx >= 0x1D0 ||
	    (r_args.key_idx & 3) ||
	    r_args.word_cnt > (0x1D0 - r_args.key_idx) / 4)
		return -EINVAL;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_READ function of Trusted App */
	inv_arg.func = PTA_CMD_OTP_READ;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

	param[0].u.value.a = r_args.key_idx;
	param[1].u.memref.shm = ks_priv->ks_shm_pool;
	param[1].u.memref.size = r_args.word_cnt * sizeof(u32);
	param[1].u.memref.shm_offs = 0;

	ret = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((ret < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_OTP_READ invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}

	memcpy((u8 *)r_args.key, ks_priv->va_shm, r_args.word_cnt * 4);

	return copy_to_user(arg, &r_args, sizeof(r_args));
}

static int optee_ks_program_otp(struct optee_ks_private *ks_priv,
				void __user *arg)
{
	struct ks_read_args p_args;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	u32 bit;
	int ret;

	if (copy_from_user(&p_args, arg, sizeof(p_args)))
		return -EFAULT;

	bit = p_args.key[0];
	if (p_args.word_cnt != 1 || p_args.key_idx < 0 ||
	    (p_args.key_idx & 3) || !bit || (bit & (bit - 1)) ||
	    !((p_args.key_idx >= 0x120 && p_args.key_idx <= 0x148) ||
	      (p_args.key_idx >= 0x1A4 && p_args.key_idx <= 0x1CC)))
		return -EINVAL;

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	inv_arg.func = PTA_CMD_OTP_PROGRAM;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = p_args.key_idx;
	param[0].u.value.b = bit;

	ret = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if (ret < 0 || inv_arg.ret) {
		dev_err(ks_priv->dev, "PTA_CMD_OTP_PROGRAM invoke err: %x\n",
			inv_arg.ret);
		return -EIO;
	}

	return 0;
}

static int optee_ks_dev_open(struct inode *iptr, struct file *fptr)
{
	struct optee_ks_private *ks_priv;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	int ret;

	ks_priv = container_of(fptr->private_data, struct optee_ks_private, miscdev);

	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_KS_INIT function of Trusted App */
	inv_arg.func = PTA_CMD_KS_INIT;
	inv_arg.session = ks_priv->session_id;
	inv_arg.num_params = 4;

	ret = tee_client_invoke_func(ks_priv->ctx, &inv_arg, param);
	if ((ret < 0) || (inv_arg.ret != 0)) {
		dev_err(ks_priv->dev, "PTA_CMD_KS_INIT invoke err: %x\n",
			inv_arg.ret);
		return -EINVAL;
	}
	return 0;
}

static int optee_ks_dev_release(struct inode *iptr, struct file *fptr)
{
	return 0;
}

static long optee_ks_dev_ioctl(struct file *fptr, unsigned int cmd,
				 unsigned long data)
{
	struct optee_ks_private  *ks_priv;
	char __user *argp = (char __user *)data;
	int ret;

	ks_priv = container_of(fptr->private_data, struct optee_ks_private, miscdev);

	if (_IOC_TYPE(cmd) != MA35_KS_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case NU_KS_IOCTL_READ:
		ret = optee_ks_read(ks_priv, argp);
		break;

	case NU_KS_IOCTL_WRITE_SRAM:
		ret = optee_ks_write_sram(ks_priv, argp);
		break;

	case NU_KS_IOCTL_WRITE_OTP:
		ret = optee_ks_write_otp(ks_priv, argp);
		break;

	case NU_KS_IOCTL_ERASE:
		ret = optee_ks_erase(ks_priv, argp);
		break;

	case NU_KS_IOCTL_ERASE_ALL:
		ret = optee_ks_erase_all(ks_priv);
		break;

	case NU_KS_IOCTL_REVOKE:
		ret = optee_ks_revoke(ks_priv, argp);
		break;

	case NU_KS_IOCTL_GET_REMAIN:
		ret = optee_ks_remain(ks_priv);
		break;

	case NU_KS_IOCTL_OTP_READ:
		ret = optee_ks_read_otp(ks_priv, argp);
		break;

	case NU_KS_IOCTL_OTP_WRITE:
		ret = optee_ks_program_otp(ks_priv, argp);
		break;

	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static const struct file_operations ma35_ks_optee_fops = {
	.owner = THIS_MODULE,
	.open = optee_ks_dev_open,
	.release = optee_ks_dev_release,
	.unlocked_ioctl = optee_ks_dev_ioctl,
	.compat_ioctl = optee_ks_dev_ioctl,
};

static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	if (ver->impl_id == TEE_IMPL_ID_OPTEE)
		return 1;
	else
		return 0;
}

static int optee_ks_probe(struct device *dev)
{
	struct tee_client_device *ks_device = to_tee_client_device(dev);
	struct tee_ioctl_open_session_arg sess_arg;
	int ret;

	memset(&sess_arg, 0, sizeof(sess_arg));

	pvt_data.dev = dev;

	/*
	 * Open context with TEE driver
	 */
	pvt_data.ctx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(pvt_data.ctx)) {
		dev_err(dev, "tee_client_open_context failed, err: %x\n",
			sess_arg.ret);
		return -ENODEV;
	}

	/*
	 * Open session with KS Trusted App
	 */
	memcpy(sess_arg.uuid, ks_device->id.uuid.b, TEE_IOCTL_UUID_LEN);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	sess_arg.num_params = 0;

	ret = tee_client_open_session(pvt_data.ctx, &sess_arg, NULL);
	if (ret < 0 || sess_arg.ret != 0) {
		dev_err(dev, "tee_client_open_session failed, err: %x\n", sess_arg.ret);
		ret = -EINVAL;
		goto out_ctx;
	}
	pvt_data.session_id = sess_arg.session;

	/*
	 * Allocate KS buffer from OP-TEE share memory
	 */
	pvt_data.ks_shm_pool = tee_shm_alloc_kernel_buf(pvt_data.ctx, KS_DATA_SHM_SZ);
	if (IS_ERR(pvt_data.ks_shm_pool)) {
		dev_err(dev, "tee_shm_alloc failed\n");
		ret = -ENOMEM;
		goto out_sess;
	}

	pvt_data.va_shm = tee_shm_get_va(pvt_data.ks_shm_pool, 0);
	if (IS_ERR(pvt_data.va_shm)) {
		dev_err(dev, "tee_shm_get_va failed\n");
		tee_shm_free(pvt_data.ks_shm_pool);
		ret = -ENOMEM;
		goto out_sess;
	}

	/*
	 * Register optee KS device as a misc device
	 */
	pvt_data.miscdev.minor = MISC_DYNAMIC_MINOR;
	pvt_data.miscdev.name = ks_miscdev_name;
	pvt_data.miscdev.fops = &ma35_ks_optee_fops;
	pvt_data.miscdev.parent = dev;
	ret = misc_register(&pvt_data.miscdev);
	if (ret) {
		dev_err(dev, "error: %d. Unable to register device", ret);
		goto out_sess;
	}

	printk("ma35 kstore-optee initialized.\n");
	return 0;

out_sess:
	tee_client_close_session(pvt_data.ctx, pvt_data.session_id);
out_ctx:
	tee_client_close_context(pvt_data.ctx);
	return ret;
}

static int optee_ks_remove(struct device *dev)
{
	misc_deregister(&pvt_data.miscdev);
	tee_shm_free(pvt_data.ks_shm_pool);
	tee_client_close_session(pvt_data.ctx, pvt_data.session_id);
	tee_client_close_context(pvt_data.ctx);
	return 0;
}

static const struct tee_client_device_id optee_ks_id_table[] = {
	{ UUID_INIT(0xaac83d50, 0xc303, 0x41ee,
		   0xb8, 0xf2, 0x70, 0x6c, 0x0b, 0x78, 0xe5, 0xad)},
	{}
};

MODULE_DEVICE_TABLE(tee, optee_ks_id_table);

static struct tee_client_driver optee_ks_driver = {
	.id_table	= optee_ks_id_table,
	.driver		= {
		.name		= "ma35-kstore-optee",
		.bus		= &tee_bus_type,
		.probe		= optee_ks_probe,
		.remove		= optee_ks_remove,
	},
};

static int ma35d1_ks_probe(struct platform_device *pdev)
{
	int err;

	err = driver_register(&optee_ks_driver.driver);
	if (err)
		pr_debug("Failed to register ma35-kstore-optee!\n");
	else
		pr_info("ma35-kstore-optee initialized.\n");

	return err;
}

static void ma35d1_ks_remove(struct platform_device *pdev)
{
}

static const struct of_device_id ma35_ks_of_match[] = {
	{
		.compatible = "nuvoton,ma35d1-kstore-optee",
	},
	{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, ma35_ks_of_match);

static struct platform_driver ma35d1_ks_driver = {
	.probe = ma35d1_ks_probe,
	.remove =  ma35d1_ks_remove,
	.driver = {
		.name = "ma35d1-kstore-optee",
		.of_match_table = ma35_ks_of_match,
	},
};

module_platform_driver(ma35d1_ks_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Nuvoton, Inc");
MODULE_DESCRIPTION("Nuvoton MA35D1 Key Store Driver");
