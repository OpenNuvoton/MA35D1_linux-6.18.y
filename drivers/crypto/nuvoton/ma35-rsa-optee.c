// SPDX-License-Identifier: GPL-2.0
/*
 * linux/driver/crypto/nuvoton/ma35-rsa-optee.c
 *
 * Copyright (c) 2025 Nuvoton technology corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;version 2 of the License.
 *
 */
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/tee_drv.h>
#include <linux/crypto.h>
#include <linux/miscdevice.h>
#include <crypto/internal/rsa.h>
#include <crypto/internal/akcipher.h>

#include "ma35-crypto.h"

static struct nu_rsa_dev  *__rsa_dd;

struct nu_rsa_drv {
	struct list_head dev_list;
	/* Device list lock */
	spinlock_t lock;
};

static struct nu_rsa_drv  nu_rsa = {
	.dev_list = LIST_HEAD_INIT(nu_rsa.dev_list),
	.lock = __SPIN_LOCK_UNLOCKED(nu_rsa.lock),
};

static struct nu_rsa_dev *ma35_rsa_find_dev(struct nu_rsa_ctx *ctx)
{
	struct nu_rsa_dev *dd = NULL;
	struct nu_rsa_dev *tmp;

	spin_lock_bh(&nu_rsa.lock);

	if (!ctx->dd) {
		list_for_each_entry(tmp, &nu_rsa.dev_list, list) {
			dd = tmp;
			break;
		}
		ctx->dd = dd;
	} else {
		dd = ctx->dd;
	}

	spin_unlock_bh(&nu_rsa.lock);
	return dd;
}

static inline void ma35_write_reg(struct nu_rsa_dev *rsa_dd, u32 val, u32 reg)
{
	rsa_dd->va_shm[reg/4] = val;
}

static inline u32 ma35_read_reg(struct nu_rsa_dev *rsa_dd, u32 reg)
{
	return rsa_dd->va_shm[reg/4];
}

static int check_key_length(u8 *input, int keysz)
{
	u8 *key;

	for (key = input; (*key == 0x00); key++)
		keysz--;
	return keysz * 8;
}

static int check_ras_length(struct rsa_key *raw_key)
{
	int rsa_len;

	rsa_len = check_key_length((u8 *)raw_key->n, raw_key->n_sz);

	if ((rsa_len != 1024) && (rsa_len != 2048) &&
		(rsa_len != 3072) && (rsa_len != 4096)) {
		pr_err("rsa length %d is not supported!\n", rsa_len);
		return -EINVAL;
	}
	return rsa_len;
}

static int ma35_rsa_hex_to_reg(u8 *input, int keylen, int rsa_bit_len, u32 *reg)
{
	int rsa_byte_len = rsa_bit_len/8;
	u8 buff[NU_RSA_MAX_BYTE_LEN];
	u8 *key;
	int idx;

	memset(buff, 0, sizeof(buff));

	/* remove leading 0x00's */
	for (key = input; ((*key == 0x00) && (keylen > 0)); key++)
		keylen--;

	if (keylen > rsa_byte_len)
		return -EINVAL;

	memcpy(&buff[rsa_byte_len - keylen], key, keylen);

	for (idx = rsa_byte_len - 1; idx > 0; idx -= 4) {
		*reg++ = (buff[idx - 3] << 24) | (buff[idx - 2] << 16) |
				(buff[idx - 1] << 8) | buff[idx];
	}
	return 0;
}

static void ma35_rsa_reg_to_hex(u32 *reg, int rsa_bit_len, u8 *key, int *key_sz)
{
	int i, idx;

	*key_sz = 0;
	for (idx = rsa_bit_len/32 - 1 ; idx >= 0; idx--) {
		for (i = 3; i >= 0; i--) {
			*key++ = (reg[idx] >> (i*8)) & 0xff;
			(*key_sz)++;
			if (*key_sz >= NU_RSA_MAX_BYTE_LEN)
				return;
		}
	}
}

static int ma35_rsa_sg_to_buffer(struct scatterlist *sgl, u8 *buff, int max_len)
{
	int total = 0, copy_len;

	while (sgl && (total < max_len)) {
		copy_len = min((int)sgl->length, max_len - total);
		memcpy(buff + total, (u8 *)sg_virt(sgl), copy_len);
		total += copy_len;
		sgl = sg_next(sgl);
	}
	return total;
}

static int ma35_rsa_buffer_to_sg(u8 *buff, int data_cnt, struct scatterlist *sgl)
{
	int total = 0, copy_len;

	while (sgl && (data_cnt > 0)) {
		copy_len = min_t(int, sgl->length, data_cnt);
		memcpy((u8 *)sg_virt(sgl), buff + total, copy_len);
		total += copy_len;
		data_cnt -= copy_len;
		sgl = sg_next(sgl);
	}
	return total;
}

static int ma35_rsa_enc(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct nu_rsa_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct nu_rsa_dev *dd = ma35_rsa_find_dev(ctx);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	u8 m[NU_RSA_MAX_BYTE_LEN];
	u32 keyleng;
	int len, err;

	if (dd->octx == NULL)
		return -ENODEV;

	ctx->dd = dd;
	keyleng = ctx->rsa_bit_len/1024 - 1;

	memset(&ctx->buffer[E_OFF], 0, NU_RSA_MAX_BYTE_LEN);

	err = ma35_rsa_hex_to_reg((u8 *)ctx->public_key, ctx->public_key_size,
				  ctx->rsa_bit_len, (u32 *)&ctx->buffer[E_OFF]);
	if (err)
		return err;

	len = ma35_rsa_sg_to_buffer(req->src, m,
				    min_t(int, req->src_len, NU_RSA_MAX_BYTE_LEN));

	pr_debug("[%s] - req_src byte length: %d\n", __func__, len);

	err = ma35_rsa_hex_to_reg(m, len, ctx->rsa_bit_len, (u32 *)&ctx->buffer[M_OFF]);
	if (err)
		return err;

	ctx->dma_buff = dma_map_single(dd->dev, ctx->buffer, RSA_BUFF_SIZE, DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(dd->dev, ctx->dma_buff))) {
		dev_err(dd->dev, "RSA buffer dma map error\n");
		return -EINVAL;
	}

	ma35_write_reg(dd, 0, RSA_KSCTL);
	ma35_write_reg(dd, 0, RSA_KSSTS0);
	ma35_write_reg(dd, 0, RSA_KSSTS1);

	ma35_write_reg(dd, ctx->dma_buff + M_OFF, RSA_SADDR0);
	ma35_write_reg(dd, ctx->dma_buff + N_OFF, RSA_SADDR1);
	ma35_write_reg(dd, ctx->dma_buff + E_OFF, RSA_SADDR2);
	ma35_write_reg(dd, ctx->dma_buff + P_OFF, RSA_SADDR3);
	ma35_write_reg(dd, ctx->dma_buff + Q_OFF, RSA_SADDR4);
	ma35_write_reg(dd, ctx->dma_buff + ANS_OFF, RSA_DADDR);
	ma35_write_reg(dd, ctx->dma_buff + MADR0_OFF, RSA_MADDR0);
	ma35_write_reg(dd, ctx->dma_buff + MADR1_OFF, RSA_MADDR1);
	ma35_write_reg(dd, ctx->dma_buff + MADR2_OFF, RSA_MADDR2);
	ma35_write_reg(dd, ctx->dma_buff + MADR3_OFF, RSA_MADDR3);
	ma35_write_reg(dd, ctx->dma_buff + MADR4_OFF, RSA_MADDR4);
	ma35_write_reg(dd, ctx->dma_buff + MADR5_OFF, RSA_MADDR5);
	ma35_write_reg(dd, ctx->dma_buff + MADR6_OFF, RSA_MADDR6);

	ma35_write_reg(dd, (keyleng << RSA_CTL_KEYLENG_OFFSET) | RSA_CTL_START, RSA_CTL);

	/*
	 *  Invoke OP-TEE Crypto PTA to run RSA
	 */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_CRYPTO_RSA_RUN function of Trusted App */
	inv_arg.func = PTA_CMD_CRYPTO_RSA_RUN;
	inv_arg.session = dd->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

	param[1].u.memref.shm = dd->shm_pool;
	param[1].u.memref.size = CRYPTO_SHM_SIZE;
	param[1].u.memref.shm_offs = 0;

	err = tee_client_invoke_func(dd->octx, &inv_arg, param);
	if ((err < 0) || (inv_arg.ret != 0)) {
		pr_err("PTA_CMD_CRYPTO_RSA_RUN enc err: %x\n", inv_arg.ret);
		return -EINVAL;
	}

	dma_unmap_single(dd->dev, ctx->dma_buff, RSA_BUFF_SIZE, DMA_FROM_DEVICE);

	memset(m, 0, sizeof(m));
	ma35_rsa_reg_to_hex((u32 *)&ctx->buffer[ANS_OFF], ctx->rsa_bit_len, m, &len);

	ma35_rsa_buffer_to_sg(m, min_t(int, len, (int)req->dst_len), req->dst);

	return err;
}

static int ma35_rsa_dec(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct nu_rsa_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct nu_rsa_dev *dd = ma35_rsa_find_dev(ctx);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	u8 m[NU_RSA_MAX_BYTE_LEN];
	u32 keyleng;
	int err, len;

	if (dd->octx == NULL)
		return -ENODEV;

	ctx->dd = dd;
	keyleng = (ctx->rsa_bit_len - 1024) / 1024;

	memset(&ctx->buffer[D_OFF], 0, NU_RSA_MAX_BYTE_LEN);

	err = ma35_rsa_hex_to_reg((u8 *)ctx->private_key, ctx->private_key_size,
				  ctx->rsa_bit_len, (u32 *)&ctx->buffer[D_OFF]);
	if (err)
		return err;

	len = ma35_rsa_sg_to_buffer(req->src, m,
				    min_t(int, req->src_len, NU_RSA_MAX_BYTE_LEN));

	pr_debug("[%s] - req_src byte length: %d\n", __func__, len);

	memset(&ctx->buffer[M_OFF], 0, NU_RSA_MAX_BYTE_LEN);
	memset(&ctx->buffer[ANS_OFF], 0, NU_RSA_MAX_BYTE_LEN);

	err = ma35_rsa_hex_to_reg(m, len, ctx->rsa_bit_len, (u32 *)&ctx->buffer[M_OFF]);
	if (err)
		return err;

	ctx->dma_buff = dma_map_single(dd->dev, ctx->buffer, RSA_BUFF_SIZE, DMA_BIDIRECTIONAL);
	if (unlikely(dma_mapping_error(dd->dev, ctx->dma_buff))) {
		dev_err(dd->dev, "RSA buffer dma map error\n");
		return -EINVAL;
	}

	ma35_write_reg(dd, ctx->dma_buff + M_OFF, RSA_SADDR0);
	ma35_write_reg(dd, ctx->dma_buff + N_OFF, RSA_SADDR1);
	ma35_write_reg(dd, ctx->dma_buff + E_OFF, RSA_SADDR2);
	ma35_write_reg(dd, ctx->dma_buff + P_OFF, RSA_SADDR3);
	ma35_write_reg(dd, ctx->dma_buff + Q_OFF, RSA_SADDR4);
	ma35_write_reg(dd, ctx->dma_buff + ANS_OFF, RSA_DADDR);
	ma35_write_reg(dd, ctx->dma_buff + MADR0_OFF, RSA_MADDR0);
	ma35_write_reg(dd, ctx->dma_buff + MADR1_OFF, RSA_MADDR1);
	ma35_write_reg(dd, ctx->dma_buff + MADR2_OFF, RSA_MADDR2);
	ma35_write_reg(dd, ctx->dma_buff + MADR3_OFF, RSA_MADDR3);
	ma35_write_reg(dd, ctx->dma_buff + MADR4_OFF, RSA_MADDR4);
	ma35_write_reg(dd, ctx->dma_buff + MADR5_OFF, RSA_MADDR5);
	ma35_write_reg(dd, ctx->dma_buff + MADR6_OFF, RSA_MADDR6);

	ma35_write_reg(dd, (keyleng << RSA_CTL_KEYLENG_OFFSET) | RSA_CTL_START, RSA_CTL);

	/*
	 *  Invoke OP-TEE Crypto PTA to run RSA
	 */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_CRYPTO_RSA_RUN function of Trusted App */
	inv_arg.func = PTA_CMD_CRYPTO_RSA_RUN;
	inv_arg.session = dd->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

	param[1].u.memref.shm = dd->shm_pool;
	param[1].u.memref.size = CRYPTO_SHM_SIZE;
	param[1].u.memref.shm_offs = 0;

	err = tee_client_invoke_func(dd->octx, &inv_arg, param);
	if ((err < 0) || (inv_arg.ret != 0)) {
		pr_err("PTA_CMD_CRYPTO_RSA_RUN dec err: %x\n", inv_arg.ret);
		return -EINVAL;
	}

	dma_unmap_single(dd->dev, ctx->dma_buff, RSA_BUFF_SIZE, DMA_BIDIRECTIONAL);

	memset(m, 0, sizeof(m));

	ma35_rsa_reg_to_hex((u32 *)&ctx->buffer[ANS_OFF], ctx->rsa_bit_len, m, &len);

	ma35_rsa_buffer_to_sg(m, min_t(int, len, (int)req->dst_len), req->dst);

	return err;
}

static int ma35_rsa_set_pub_key(struct crypto_akcipher *tfm, const void *key,
				unsigned int keylen)
{
	struct nu_rsa_ctx  *ctx = akcipher_tfm_ctx(tfm);
	struct rsa_key raw_key = {0};
	int rsa_bit_len;
	int err;

	err = rsa_parse_pub_key(&raw_key, key, keylen);
	if (err)
		return err;

	pr_debug("Key size n,e,d,p,q: %d %d %d %d %d\n", (int)raw_key.n_sz,
		(int)raw_key.e_sz, (int)raw_key.d_sz, (int)raw_key.p_sz,
		(int)raw_key.q_sz);

	rsa_bit_len = check_ras_length(&raw_key);
	if (rsa_bit_len < 0)
		return rsa_bit_len;

	ctx->rsa_bit_len = rsa_bit_len;

	memset(&ctx->buffer[N_OFF], 0, NU_RSA_MAX_BYTE_LEN);

	err = ma35_rsa_hex_to_reg((u8 *)raw_key.n, raw_key.n_sz, rsa_bit_len,
					(u32 *)&ctx->buffer[N_OFF]);
	if (err)
		return err;

	memcpy(ctx->public_key, (u8 *)raw_key.e, raw_key.e_sz);
	ctx->public_key_size = raw_key.e_sz;

	return 0;
}

static int ma35_rsa_set_priv_key(struct crypto_akcipher *tfm, const void *key,
				 unsigned int keylen)
{
	struct nu_rsa_ctx  *ctx = akcipher_tfm_ctx(tfm);
	struct rsa_key raw_key = {0};
	int rsa_bit_len;
	int err;

	err = rsa_parse_priv_key(&raw_key, key, keylen);
	if (err)
		return err;

	pr_debug("Key size n,e,d,p,q: %d %d %d %d %d\n", (int)raw_key.n_sz,
		(int)raw_key.e_sz, (int)raw_key.d_sz, (int)raw_key.p_sz,
		(int)raw_key.q_sz);

	rsa_bit_len = check_ras_length(&raw_key);
	if (rsa_bit_len < 0)
		return rsa_bit_len;

	ctx->rsa_bit_len = rsa_bit_len;

	memset(&ctx->buffer[N_OFF], 0, NU_RSA_MAX_BYTE_LEN);

	err = ma35_rsa_hex_to_reg((u8 *)raw_key.n, raw_key.n_sz, rsa_bit_len,
				  (u32 *)&ctx->buffer[N_OFF]);
	if (err)
		return err;

	memcpy(ctx->private_key, (u8 *)raw_key.d, raw_key.d_sz);
	ctx->private_key_size = raw_key.d_sz;

	memcpy(ctx->public_key, (u8 *)raw_key.e, raw_key.e_sz);
	ctx->public_key_size = raw_key.e_sz;

	return 0;
}

static unsigned int ma35_rsa_max_size(struct crypto_akcipher *tfm)
{
	struct nu_rsa_ctx *ctx = akcipher_tfm_ctx(tfm);

	pr_debug("%s: %d\n", __func__, ctx->rsa_bit_len / 8);
	return (ctx->rsa_bit_len / 8);
}

static int ma35_rsa_init_tfm(struct crypto_akcipher *tfm)
{
	struct nu_rsa_ctx *ctx = akcipher_tfm_ctx(tfm);

	memset(ctx->buffer, 0, sizeof(ctx->buffer));
	return 0;
}

static struct akcipher_alg ma35_rsa_optee_alg = {
	.encrypt = ma35_rsa_enc,
	.decrypt = ma35_rsa_dec,
	.set_priv_key = ma35_rsa_set_priv_key,
	.set_pub_key = ma35_rsa_set_pub_key,
	.max_size = ma35_rsa_max_size,
	.init = ma35_rsa_init_tfm,
	.base = {
		.cra_name = "rsa(ma35-optee)",
		.cra_driver_name = "rsa-ma35d1-optee",
		.cra_priority = 500,
		.cra_module = THIS_MODULE,
		.cra_ctxsize = sizeof(struct nu_rsa_ctx),
	},
};

static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	if (ver->impl_id == TEE_IMPL_ID_OPTEE)
		return 1;
	else
		return 0;
}

static int ma35_rsa_ioctl_set_register(struct nu_rsa_ctx *rsa_ctx, int offs, unsigned long arg)
{
	int ret, sz;
	u8 kbuf[520];

	memset(kbuf, 0, sizeof(kbuf));
	if (copy_from_user(kbuf, (u8 *)arg, 514))
		return -EFAULT;

	sz = (kbuf[0] << 8) | kbuf[1];
	if (sz > 512)
		return -EINVAL;

	memset(&rsa_ctx->buffer[offs], 0, NU_RSA_MAX_BYTE_LEN);
	ret = ma35_rsa_hex_to_reg((u8 *)&kbuf[2], sz, rsa_ctx->rsa_bit_len,
				  (u32 *)&rsa_ctx->buffer[offs]);
	return ret;
}

static long ma35_rsa_optee_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct nu_rsa_ctx *rsa_ctx = filp->private_data;
	struct nu_rsa_dev *dd;
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	u8 m[NU_RSA_MAX_BYTE_LEN];
	int keyleng, len;
	int err;

	if (!rsa_ctx)
		return -EIO;
	dd = rsa_ctx->dd;

	switch (cmd) {
	case RSA_IOC_SET_BITLEN:
		rsa_ctx->rsa_bit_len = arg;
		return 0;

	case RSA_IOC_SET_N:
		memset(&rsa_ctx->buffer[N_OFF], 0, NU_RSA_MAX_BYTE_LEN);
		ma35_rsa_ioctl_set_register(rsa_ctx, N_OFF, arg);
		break;

	case RSA_IOC_SET_E:
		memset(&rsa_ctx->buffer[E_OFF], 0, NU_RSA_MAX_BYTE_LEN);
		ma35_rsa_ioctl_set_register(rsa_ctx, E_OFF, arg);
		break;

	case RSA_IOC_SET_M:
		memset(&rsa_ctx->buffer[M_OFF], 0, NU_RSA_MAX_BYTE_LEN);
		ma35_rsa_ioctl_set_register(rsa_ctx, M_OFF, arg);
		break;

	case RSA_IOC_SET_P:
		memset(&rsa_ctx->buffer[P_OFF], 0, NU_RSA_MAX_BYTE_LEN);
		ma35_rsa_ioctl_set_register(rsa_ctx, P_OFF, arg);
		break;

	case RSA_IOC_SET_Q:
		memset(&rsa_ctx->buffer[Q_OFF], 0, NU_RSA_MAX_BYTE_LEN);
		ma35_rsa_ioctl_set_register(rsa_ctx, Q_OFF, arg);
		break;

	case RSA_IOC_RUN:
		memset(&rsa_ctx->buffer[ANS_OFF], 0, NU_RSA_MAX_BYTE_LEN);
		rsa_ctx->dma_buff = dma_map_single(dd->dev, rsa_ctx->buffer,
				RSA_BUFF_SIZE, DMA_BIDIRECTIONAL);
		if (unlikely(dma_mapping_error(dd->dev, rsa_ctx->dma_buff))) {
			dev_err(dd->dev, "RSA buffer DMA map error\n");
			return -EINVAL;
		}
		ma35_write_reg(dd, rsa_ctx->dma_buff + M_OFF, RSA_SADDR0);
		ma35_write_reg(dd, rsa_ctx->dma_buff + N_OFF, RSA_SADDR1);
		ma35_write_reg(dd, rsa_ctx->dma_buff + E_OFF, RSA_SADDR2);
		ma35_write_reg(dd, rsa_ctx->dma_buff + P_OFF, RSA_SADDR3);
		ma35_write_reg(dd, rsa_ctx->dma_buff + Q_OFF, RSA_SADDR4);
		ma35_write_reg(dd, rsa_ctx->dma_buff + ANS_OFF, RSA_DADDR);
		ma35_write_reg(dd, rsa_ctx->dma_buff + MADR0_OFF, RSA_MADDR0);
		ma35_write_reg(dd, rsa_ctx->dma_buff + MADR1_OFF, RSA_MADDR1);
		ma35_write_reg(dd, rsa_ctx->dma_buff + MADR2_OFF, RSA_MADDR2);
		ma35_write_reg(dd, rsa_ctx->dma_buff + MADR3_OFF, RSA_MADDR3);
		ma35_write_reg(dd, rsa_ctx->dma_buff + MADR4_OFF, RSA_MADDR4);
		ma35_write_reg(dd, rsa_ctx->dma_buff + MADR5_OFF, RSA_MADDR5);
		ma35_write_reg(dd, rsa_ctx->dma_buff + MADR6_OFF, RSA_MADDR6);

		keyleng = rsa_ctx->rsa_bit_len/1024 - 1;
		ma35_write_reg(dd, (keyleng << RSA_CTL_KEYLENG_OFFSET) |
				RSA_CTL_START, RSA_CTL);

		/*
		 *  Invoke OP-TEE Crypto PTA to run RSA
		 */
		memset(&inv_arg, 0, sizeof(inv_arg));
		memset(&param, 0, sizeof(param));

		/* Invoke PTA_CMD_CRYPTO_RSA_RUN function of Trusted App */
		inv_arg.func = PTA_CMD_CRYPTO_RSA_RUN;
		inv_arg.session = dd->session_id;
		inv_arg.num_params = 4;

		/* Fill invoke cmd params */
		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

		param[1].u.memref.shm = dd->shm_pool;
		param[1].u.memref.size = CRYPTO_SHM_SIZE;
		param[1].u.memref.shm_offs = 0;

		memcpy(&(dd->va_shm[0x1000/4]), rsa_ctx->buffer, RSA_BUFF_SIZE);
		memset(&(dd->va_shm[0x3000/4]), 0, 0x200);

		dma_sync_single_for_device(dd->dev, rsa_ctx->dma_buff,
					   RSA_BUFF_SIZE, DMA_TO_DEVICE);

		err = tee_client_invoke_func(dd->octx, &inv_arg, param);
		if ((err < 0) || (inv_arg.ret != 0)) {
			pr_err("PTA_CMD_CRYPTO_RSA_RUN err: %x\n", inv_arg.ret);
			dma_unmap_single(dd->dev, rsa_ctx->dma_buff, RSA_BUFF_SIZE,
					 DMA_BIDIRECTIONAL);
			return -EINVAL;
		}
		memcpy((u32 *)&rsa_ctx->buffer[ANS_OFF], &(dd->va_shm[0x3000/4]), 0x200);

		dma_unmap_single(dd->dev, rsa_ctx->dma_buff, RSA_BUFF_SIZE,
				 DMA_BIDIRECTIONAL);

		ma35_rsa_reg_to_hex((u32 *)&rsa_ctx->buffer[ANS_OFF],
				    rsa_ctx->rsa_bit_len, m, &len);

		if (copy_to_user((char *)arg, m, len))
			return -EFAULT;
		break;

	default:
		return -ENOTTY;
	}
	return 0;
}

static int ma35_rsa_optee_open(struct inode *inode, struct file *file)
{
	struct nu_rsa_ctx  *rsa_ctx;

	rsa_ctx = kzalloc(sizeof(*rsa_ctx), GFP_KERNEL);
	if (!rsa_ctx)
		return -ENOMEM;

	rsa_ctx->dd = __rsa_dd;
	file->private_data = rsa_ctx;

	if (__rsa_dd->octx == NULL)
		return -ENODEV;

	return 0;
}

static int ma35_rsa_optee_close(struct inode *inode, struct file *file)
{
	struct nu_rsa_ctx  *rsa_ctx;

	rsa_ctx = file->private_data;
	if (!rsa_ctx)
		return -EIO;
	kfree(rsa_ctx);
	return 0;
}

static const struct file_operations ma35_rsa_optee_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= ma35_rsa_optee_ioctl,
	.open           = ma35_rsa_optee_open,
	.release        = ma35_rsa_optee_close,
};

static struct miscdevice ma35_rsa_optee_dev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "nuvoton-rsa",
	.fops		= &ma35_rsa_optee_fops,
};

static int ma35_optee_rsa_init(struct nu_rsa_dev *dd)
{
	struct tee_ioctl_open_session_arg sess_arg;
	int   err;

	err = ma35_crypto_optee_init(dd->nu_cdev);
	if (err)
		return err;
	/*
	 * Open RSA context with TEE driver
	 */
	dd->octx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(dd->octx)) {
		pr_err("%s open context failed, err: %x\n", __func__, sess_arg.ret);
		return err;
	}

	/*
	 * Open RSA session with Crypto Trusted App
	 */
	memset(&sess_arg, 0, sizeof(sess_arg));
	memcpy(sess_arg.uuid, dd->nu_cdev->tee_cdev->id.uuid.b, TEE_IOCTL_UUID_LEN);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	sess_arg.num_params = 0;

	err = tee_client_open_session(dd->octx, &sess_arg, NULL);
	if ((err < 0) || (sess_arg.ret != 0)) {
		pr_err("%s open session failed, err: %x\n", __func__, sess_arg.ret);
		err = -EINVAL;
		goto out_ctx;
	}
	dd->session_id = sess_arg.session;

	/*
	 * Allocate handshake buffer from OP-TEE share memory
	 */
	dd->shm_pool = tee_shm_alloc_kernel_buf(dd->octx, CRYPTO_SHM_SIZE);
	if (IS_ERR(dd->shm_pool)) {
		pr_err("%s tee_shm_alloc failed\n", __func__);
		goto out_sess;
	}

	dd->va_shm = tee_shm_get_va(dd->shm_pool, 0);
	if (IS_ERR(dd->va_shm)) {
		tee_shm_free(dd->shm_pool);
		pr_err("%s tee_shm_get_va failed\n", __func__);
		goto out_sess;
	}

	return 0;

out_sess:
	tee_client_close_session(dd->octx, dd->session_id);
out_ctx:
	tee_client_close_context(dd->octx);
	return err;
}

static void ma35_optee_rsa_exit(struct nu_rsa_dev *dd)
{
	tee_shm_free(dd->shm_pool);
	tee_client_close_session(dd->octx, dd->session_id);
	tee_client_close_context(dd->octx);
	dd->octx = NULL;
}

int ma35_rsa_optee_probe(struct device *dev, struct nu_crypto_dev *crypto_dev)
{
	struct nu_rsa_dev *rsa_dd = &crypto_dev->rsa_dd;
	int err = 0;

	__rsa_dd = rsa_dd;
	rsa_dd->dev = dev;
	rsa_dd->nu_cdev = crypto_dev;
	rsa_dd->reg_base = crypto_dev->reg_base;
	rsa_dd->octx = NULL;

	err = ma35_optee_rsa_init(rsa_dd);
	if (err)
		return err;

	spin_lock(&nu_rsa.lock);
	list_add_tail(&rsa_dd->list, &nu_rsa.dev_list);
	spin_unlock(&nu_rsa.lock);

	if (crypto_dev->rsa_ioctl == true) {
		misc_register(&ma35_rsa_optee_dev);
	} else {
		err = crypto_register_akcipher(&ma35_rsa_optee_alg);
		if (err) {
			pr_err("[%s] - failed to register ma35_rsa_optee_alg! %d\n",
				__func__, err);
			goto err_out;
		}
	}
	pr_info("ma35 crypto rsa optee initialized.\n");
	return 0;

err_out:
	spin_lock(&nu_rsa.lock);
	list_del(&rsa_dd->list);
	spin_unlock(&nu_rsa.lock);
	return err;
}

int ma35_rsa_optee_remove(struct device *dev, struct nu_crypto_dev *crypto_dev)
{
	struct nu_rsa_dev *rsa_dd = &crypto_dev->rsa_dd;

	if (rsa_dd == NULL)
		return -ENODEV;

	if (crypto_dev->rsa_ioctl == true)
		misc_deregister(&ma35_rsa_optee_dev);
	else
		crypto_unregister_akcipher(&ma35_rsa_optee_alg);

	spin_lock(&nu_rsa.lock);
	list_del(&rsa_dd->list);
	spin_unlock(&nu_rsa.lock);

	ma35_optee_rsa_exit(rsa_dd);
	return 0;
}
