// SPDX-License-Identifier: GPL-2.0
/*
 * linux/driver/crypto/nuvoton/nuvoton-sha-optee.c
 *
 * Copyright (c) 2025 Nuvoton technology corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation;version 2 of the License.
 *
 * Some ideas are from oamp-sha.c and mtk-sha.c drivers.
 */
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <crypto/scatterwalk.h>
#include <linux/tee_drv.h>
#include <linux/crypto.h>
#include <crypto/algapi.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/sha3.h>
#include <crypto/sm3.h>
#include <crypto/md5.h>
#include <crypto/internal/hash.h>
#include <linux/unaligned.h>

#include "ma35-crypto.h"
#include "ma35-crypto-optee.h"

/* SHA device flags */
#define DD_FLAGS_BUSY		BIT(0)
#define DD_FLAGS_DO_KEY		BIT(1)

/* SHA context flags */
#define SHA_FLAGS_FIRST		BIT(0)
#define SHA_FLAGS_KEY_BLK	BIT(1)
#define	SHA_FLAGS_FINUP		BIT(2)  /* is a final update request */
#define	SHA_FLAGS_FINAL		BIT(3)  /* is the final request */
#define	SHA_FLAGS_FINAL_DMA	BIT(4)  /* is last DMA of the final request */
#define SHA_FLAGS_TEE_SESSION	BIT(5)  /* owned by this initialized stream */

struct nu_sha_drv {
	struct list_head dev_list;
	/* Device list lock */
	spinlock_t lock;
};

static struct nu_sha_drv nu_sha = {
	.dev_list = LIST_HEAD_INIT(nu_sha.dev_list),
	.lock = __SPIN_LOCK_UNLOCKED(nu_sha.lock),
};

static struct nu_sha_dev *ma35_sha_find_dev(struct nu_sha_ctx *tctx)
{
	struct nu_sha_dev *dd = NULL;
	struct nu_sha_dev *tmp;

	spin_lock_bh(&nu_sha.lock);

	if (!tctx->dd) {
		list_for_each_entry(tmp, &nu_sha.dev_list, list) {
			dd = tmp;
			break;
		}
		tctx->dd = dd;
	} else {
		dd = tctx->dd;
	}

	spin_unlock_bh(&nu_sha.lock);

	return dd;
}

static inline void ma35_write_reg(struct nu_sha_dev *sha_dd, u32 val, u32 reg)
{
	sha_dd->va_shm[reg/4] = val;
}

static inline u32 ma35_read_reg(struct nu_sha_dev *sha_dd, u32 reg)
{
	return sha_dd->va_shm[reg/4];
}

struct ma35_sha_tee_session {
	struct list_head list;
	u32 sid;
	struct nu_sha_reqctx *ctx;
	bool close_failed;
};

struct ma35_sha_optee_state {
	struct nu_sha_dev *dd;
	struct workqueue_struct *wq;
	struct work_struct queue_work;
	struct work_struct done_work;
	struct list_head sessions;
	bool stopping;
	bool registered;
	bool dma_mapped;
};

static struct ma35_sha_optee_state sha_tee;

static bool ma35_sha_tee_session_open(struct nu_sha_dev *dd,
				       struct nu_sha_reqctx *ctx)
{
	struct ma35_sha_tee_session *session;

	if (!(ctx->flags & SHA_FLAGS_TEE_SESSION))
		return false;
	list_for_each_entry(session, &sha_tee.sessions, list) {
		if (session->ctx == ctx && session->sid == ctx->tsi_sid &&
		    !session->close_failed)
			return true;
	}
	return false;
}

static int ma35_sha_tee_close(struct nu_sha_dev *dd, u32 sid)
{
	struct tee_ioctl_invoke_arg arg = { };
	struct tee_param param[4] = { };
	struct ma35_sha_tee_session *session, *tmp;
	int err;

	list_for_each_entry(session, &sha_tee.sessions, list) {
		if (session->sid == sid && session->close_failed)
			return -EIO;
	}
	arg.func = PTA_CMD_CRYPTO_CLOSE_SESSION;
	arg.session = dd->session_id;
	arg.num_params = ARRAY_SIZE(param);
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[0].u.value.a = C_CODE_SHA;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].u.value.a = sid;
	err = tee_client_invoke_func(dd->octx, &arg, param);
	if (err < 0 || arg.ret)
		dev_err(dd->dev, "SHA close sid=%u: transport=%d PTA=%#x\n",
			sid, err, arg.ret);
	list_for_each_entry_safe(session, tmp, &sha_tee.sessions, list) {
		if (session->sid != sid)
			continue;
		if (err < 0 || arg.ret) {
			session->close_failed = true;
			WRITE_ONCE(ma35_crypto_optee_faulted, true);
			break;
		}
		list_del(&session->list);
		kfree(session);
		break;
	}
	return err < 0 ? err : (arg.ret ? -EIO : 0);
}

static int ma35_sha_tee_retire(struct nu_sha_dev *dd,
				struct nu_sha_reqctx *ctx)
{
	struct ma35_sha_tee_session *session, *tmp;
	int err;

	list_for_each_entry_safe(session, tmp, &sha_tee.sessions, list) {
		if (session->ctx != ctx)
			continue;
		err = ma35_sha_tee_close(dd, session->sid);
		if (err)
			return err;
	}
	return 0;
}

static void ma35_sha_tee_close_all(struct nu_sha_dev *dd)
{
	struct ma35_sha_tee_session *session;

	while (!list_empty(&sha_tee.sessions)) {
		session = list_first_entry(&sha_tee.sessions,
					   struct ma35_sha_tee_session, list);
		if (session->close_failed ||
		    ma35_sha_tee_close(dd, session->sid)) {
			dev_err(dd->dev,
				"Unreleased SHA session %u; reboot required\n",
				session->sid);
			list_del(&session->list);
			kfree(session);
		}
	}
}

static void ma35_sha_tee_unmap(struct nu_sha_dev *dd)
{
	struct nu_sha_reqctx *ctx = ahash_request_ctx(dd->req);
	int size = ctx->flags & SHA_FLAGS_KEY_BLK ?
		   HMAC_KEY_BUFF_SIZE : SHA_BUFF_SIZE;

	if (!sha_tee.dma_mapped)
		return;
	dma_unmap_single(dd->dev, ctx->dma_fdbck, SHA_FDBCK_SIZE,
			 DMA_BIDIRECTIONAL);
	dma_unmap_single(dd->dev, ctx->dma_buff, size, DMA_TO_DEVICE);
	sha_tee.dma_mapped = false;
}

static int ma35_sha_dma_run(struct nu_sha_dev *dd, int is_key_block)
{
	struct nu_sha_reqctx *ctx = ahash_request_ctx(dd->req);
	struct nu_sha_ctx *tctx = crypto_tfm_ctx(dd->req->base.tfm);
	struct tee_ioctl_invoke_arg inv_arg;
	struct tee_param param[4];
	struct ma35_sha_tee_session *session;
	int  dma_cnt = 0;
	int  err;

	if (READ_ONCE(ma35_crypto_optee_faulted))
		return -EIO;
	sha_tee.dma_mapped = false;
	dma_cnt = 0;
	ctx->dma_buff = 0;
	if (is_key_block) {
		ctx->dma_buff = dma_map_single(dd->dev, tctx->keybuf,
					       HMAC_KEY_BUFF_SIZE, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(dd->dev, ctx->dma_buff))) {
			dev_err(dd->dev, "SHA keybuf dma map error\n");
			return -EINVAL;
		}

		dma_cnt = tctx->keybufcnt;
	} else {
		ctx->dma_buff = dma_map_single(dd->dev, ctx->buffer, SHA_BUFF_SIZE,
					       DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(dd->dev, ctx->dma_buff))) {
			dev_err(dd->dev, "SHA buffer dma map error\n");
			return -EINVAL;
		}

		dma_cnt = ctx->bufcnt;
	}

	ctx->dma_fdbck = dma_map_single(dd->dev, ctx->fdbck, SHA_FDBCK_SIZE,
					DMA_BIDIRECTIONAL);
	if (unlikely(dma_mapping_error(dd->dev, ctx->dma_fdbck))) {
		dev_err(dd->dev, "dma map bytes error\n");
		dma_unmap_single(dd->dev, ctx->dma_buff,
				 is_key_block ? HMAC_KEY_BUFF_SIZE : SHA_BUFF_SIZE,
				 DMA_TO_DEVICE);
		return -EINVAL;
	}
	sha_tee.dma_mapped = true;

	ctx->reg_ctl |= HMAC_CTL_INSWAP | HMAC_CTL_OUTSWAP | HMAC_CTL_FBOUT |
			HMAC_CTL_DMACSCAD | HMAC_CTL_DMAEN | HMAC_CTL_START;
	ctx->reg_ctl |= ctx->op; /* HMAC/SHA3/SM3/MD5 */

	if (ctx->flags & SHA_FLAGS_FIRST) {
		err = ma35_sha_tee_retire(dd, ctx);
		if (err)
			goto tee_error;
		session = kzalloc(sizeof(*session), GFP_KERNEL);
		if (!session) {
			err = -ENOMEM;
			goto tee_error;
		}
		ctx->reg_ctl |= HMAC_CTL_DMAFIRST;
	} else {
		ctx->reg_ctl &= ~HMAC_CTL_DMAFIRST;
		ctx->reg_ctl |= HMAC_CTL_FBIN;
	}

	if (ctx->flags & SHA_FLAGS_FINAL_DMA) {
		/* It's the final request and all data have in DMA buffer. */
		ctx->reg_ctl |= HMAC_CTL_DMALAST;
		if (ctx->flags & SHA_FLAGS_FIRST)
			ctx->reg_ctl &= ~HMAC_CTL_DMACSCAD;
	}

	if ((ctx->op & HMAC_CTL_SHA3EN) && (ctx->bufcnt == 0)) {
		/* workaround for MA35D1 SHA3 in case of DMACNT is 0 */
		ctx->reg_ctl |= HMAC_CTL_DMACSCAD;
	}

	pr_debug("Write HMAC_CTL = 0x%x, dma_cnt = %d, key_len = %d/%d\n",
		 ctx->reg_ctl, dma_cnt, tctx->hmac_key_len, ctx->block_size);

	ma35_write_reg(dd, 0, HMAC_KSCTL);

	ma35_write_reg(dd, (INTSTS_HMACIF | INTSTS_HMACEIF), INTSTS);
	ma35_write_reg(dd, ma35_read_reg(dd, INTEN) | (INTEN_HMACIEN | INTEN_HMACEIEN), INTEN);

	ma35_write_reg(dd, tctx->hmac_key_len, HMAC_KEYCNT);
	ma35_write_reg(dd, dma_cnt, HMAC_DMACNT);
	ma35_write_reg(dd, ctx->dma_buff, HMAC_SADDR);
	ma35_write_reg(dd, ctx->dma_fdbck, HMAC_FBADDR);
	ma35_write_reg(dd, ctx->reg_ctl, HMAC_CTL);

	/*--------------------------------------------------------------*/
	/*  Invoke OP-TEE Crypto PTA to run SHA                         */
	/*--------------------------------------------------------------*/

	if (ctx->flags & SHA_FLAGS_FIRST) {
		/*
		 * Open a crypto session
		 */
		memset(&inv_arg, 0, sizeof(inv_arg));
		memset(&param, 0, sizeof(param));

		/* Invoke PTA_CMD_CRYPTO_OPEN_SESSION function of PTA */
		inv_arg.func = PTA_CMD_CRYPTO_OPEN_SESSION;
		inv_arg.session = dd->session_id;
		inv_arg.num_params = 4;

		/* Fill invoke cmd params */
		param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
		param[0].u.value.a = C_CODE_SHA;

		err = tee_client_invoke_func(dd->octx, &inv_arg, param);
		if ((err < 0) || (inv_arg.ret != 0)) {
			dev_err(dd->dev,
				"SHA open failed: transport=%d PTA=%#x\n",
				err, inv_arg.ret);
			if (err < 0)
				WRITE_ONCE(ma35_crypto_optee_faulted, true);
			kfree(session);
			err = err < 0 ? err : -EIO;
			goto tee_error;
		}
		ctx->tsi_sid = param[1].u.value.a;
		session->sid = ctx->tsi_sid;
		session->ctx = ctx;
		list_add_tail(&session->list, &sha_tee.sessions);
		ctx->flags |= SHA_FLAGS_TEE_SESSION;

		/*
		 * Invoke PTA_CMD_CRYPTO_SHA_START
		 */
		memset(&inv_arg, 0, sizeof(inv_arg));
		memset(&param, 0, sizeof(param));

		/* Invoke PTA_CMD_CRYPTO_SHA_START function of PTA */
		inv_arg.func = PTA_CMD_CRYPTO_SHA_START;
		inv_arg.session = dd->session_id;
		inv_arg.num_params = 4;

		/* Fill invoke cmd params */
		param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
		param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
		param[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;

		param[0].u.value.a = ctx->tsi_sid;
		param[1].u.value.a = ctx->reg_ctl;
		param[1].u.value.b = 0;
		param[2].u.value.a = tctx->hmac_key_len;

		err = tee_client_invoke_func(dd->octx, &inv_arg, param);
		if ((err < 0) || (inv_arg.ret != 0)) {
			dev_err(dd->dev,
				"SHA start sid=%u: transport=%d PTA=%#x\n",
				ctx->tsi_sid, err, inv_arg.ret);
			err = err < 0 ? err : -EIO;
			goto tee_error;
		}
	}

	/*
	 * Invoke PTA_CMD_CRYPTO_SHA_UPDATE/FINAL
	 */
	memset(&inv_arg, 0, sizeof(inv_arg));
	memset(&param, 0, sizeof(param));

	/* Invoke PTA_CMD_CRYPTO_SHA_UPDATE/FINAL function of Trusted App */
	if (ctx->flags & SHA_FLAGS_FINAL_DMA)
		inv_arg.func = PTA_CMD_CRYPTO_SHA_FINAL;
	else
		inv_arg.func = PTA_CMD_CRYPTO_SHA_UPDATE;
	inv_arg.session = dd->session_id;
	inv_arg.num_params = 4;

	/* Fill invoke cmd params */
	param[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	param[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;

	param[0].u.value.a = ctx->tsi_sid;
	param[0].u.value.b = ctx->digest_len;
	param[1].u.memref.shm = dd->shm_pool;
	param[1].u.memref.size = CRYPTO_SHM_SIZE;
	param[1].u.memref.shm_offs = 0;

	err = tee_client_invoke_func(dd->octx, &inv_arg, param);
	if ((err < 0) || (inv_arg.ret != 0)) {
		dev_err(dd->dev,
			"SHA run sid=%u: transport=%d PTA=%#x\n",
			ctx->tsi_sid, err, inv_arg.ret);
		err = err < 0 ? err : -EIO;
		goto tee_error;
	}

	if (ctx->flags & SHA_FLAGS_FINAL_DMA) {
		err = ma35_sha_tee_close(dd, ctx->tsi_sid);
		ctx->flags &= ~SHA_FLAGS_TEE_SESSION;
		if (err)
			goto tee_error;
	}

	queue_work(sha_tee.wq, &sha_tee.done_work);

	return -EINPROGRESS;

tee_error:
	ma35_sha_tee_unmap(dd);
	return err;
}

/*
 *  The whole SHA operation is finished. Get the digest result from SHA engine.
 */
static void  ma35_sha_get_result(struct ahash_request *req)
{
	struct nu_sha_reqctx *ctx = ahash_request_ctx(req);
	u8 *result = req->result;
	int i;

	/* Get the hash from the digest buffer */
	for (i = 0; i < ctx->digest_len/4; i++)
		put_unaligned(ma35_read_reg(ctx->dd, HMAC_DGST(i)),
			      (u32 *)(result + i * sizeof(u32)));
}

/*
 *  A request is completed(err is 0) or aborted(err < 0).
 */
static void ma35_sha_finish_req(struct nu_sha_reqctx *ctx, int err)
{
	struct nu_sha_dev *dd = ctx->dd;
	struct ahash_request *req = dd->req;
	unsigned long flags;

	if (err && ma35_sha_tee_session_open(dd, ctx))
		ma35_sha_tee_close(dd, ctx->tsi_sid);
	if (err)
		ctx->flags &= ~SHA_FLAGS_TEE_SESSION;

	/*
	 *  In case of error occurred or it's the completion of final request
	 */
	if (err || (ctx->flags & SHA_FLAGS_FINAL_DMA)) {
		if (!err)
			ma35_sha_get_result(req);
		kfree(ctx->buffer);
		ctx->buffer = NULL;
		ctx->bufcnt = 0;
	}

	spin_lock_irqsave(&dd->lock, flags);
	dd->req = NULL;
	dd->flags &= ~DD_FLAGS_BUSY;
	spin_unlock_irqrestore(&dd->lock, flags);
	ahash_request_complete(req, err);

	queue_work(sha_tee.wq, &sha_tee.queue_work);
}

static int ma35_sha_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct nu_sha_ctx *tctx = crypto_ahash_ctx(tfm);
	struct nu_sha_reqctx *ctx = ahash_request_ctx(req);
	struct nu_sha_dev *dd = ma35_sha_find_dev(tctx);
	struct hash_alg_common *halg = crypto_hash_alg_common(tfm);
	char *cra_name = halg->base.cra_name;
	bool is_sha3 = false;
	gfp_t gfp;

	if (dd && READ_ONCE(ma35_crypto_optee_faulted))
		return -EIO;
	ctx->op = 0;
	if (strncmp(cra_name, "hmac", 4) == 0) {
		ctx->op = HMAC_CTL_HMACEN;
		if (strncmp(cra_name+5, "sha3-", 5) == 0) {
			ctx->op |= HMAC_CTL_SHA3EN;
			is_sha3 = true;
		}
		if (strncmp(cra_name+5, "sm3", 3) == 0)
			ctx->op |= HMAC_CTL_SM3EN;
		if (strncmp(cra_name+5, "md5", 3) == 0)
			ctx->op |= HMAC_CTL_MD5EN;
	} else if (strncmp(cra_name, "sha3-", 5) == 0) {
		is_sha3 = true;
		ctx->op = HMAC_CTL_SHA3EN;
	} else if (strncmp(cra_name, "sm3", 3) == 0) {
		ctx->op = HMAC_CTL_SM3EN;
	} else if (strncmp(cra_name, "md5", 3) == 0) {
		ctx->op = HMAC_CTL_MD5EN;
	} else {
		/* default, SHA mode */
	}

	pr_debug("[ %s ], 0x%x\n", halg->base.cra_name, ctx->op);
	ctx->dd = dd;
	ctx->sg = NULL;
	ctx->sg_off = 0;
	ctx->req_len = 0;
	ctx->tsi_sid = 0;
	ctx->flags = SHA_FLAGS_FIRST;
	ctx->reg_ctl = 0;
	ctx->digest_len = crypto_ahash_digestsize(tfm);

	switch (ctx->digest_len) {
	case SHA1_DIGEST_SIZE:
		ctx->reg_ctl |= SHA_OPMODE_SHA1;
		ctx->block_size = SHA1_BLOCK_SIZE;
		break;
	case SHA224_DIGEST_SIZE:
		ctx->reg_ctl |= SHA_OPMODE_SHA224;
		if (is_sha3 == true)
			ctx->block_size = SHA3_224_BLOCK_SIZE;
		else
			ctx->block_size = SHA224_BLOCK_SIZE;
		break;
	case SHA256_DIGEST_SIZE:
		ctx->reg_ctl |= SHA_OPMODE_SHA256;
		if (is_sha3 == true)
			ctx->block_size = SHA3_256_BLOCK_SIZE;
		else
			ctx->block_size = SHA256_BLOCK_SIZE;
		break;
	case SHA384_DIGEST_SIZE:
		ctx->reg_ctl |= SHA_OPMODE_SHA384;
		if (is_sha3 == true)
			ctx->block_size = SHA3_384_BLOCK_SIZE;
		else
			ctx->block_size = SHA384_BLOCK_SIZE;
		break;
	case SHA512_DIGEST_SIZE:
		ctx->reg_ctl |= SHA_OPMODE_SHA512;
		if (is_sha3 == true)
			ctx->block_size = SHA3_512_BLOCK_SIZE;
		else
			ctx->block_size = SHA512_BLOCK_SIZE;
		break;
	case MD5_DIGEST_SIZE:
		ctx->block_size = MD5_HMAC_BLOCK_SIZE;
		break;
	default:
		return -EINVAL;
	}

	gfp = (ahash_request_flags(req) & CRYPTO_TFM_REQ_MAY_SLEEP) ?
		GFP_KERNEL : GFP_ATOMIC;
	ctx->buffer = kmalloc(SHA_BUFF_SIZE, gfp | GFP_DMA);
	if (!ctx->buffer)
		return -ENOMEM;

	ctx->bufcnt = 0;
	ctx->dma_max_size = (SHA_BUFF_SIZE / ctx->block_size) * ctx->block_size;

	if (!(ctx->op & HMAC_CTL_HMACEN)) {
		ctx->bufcnt = 0;
		return 0;
	}

	/* is HMAC, check key length */
	if (((tctx->hmac_key_len + ctx->block_size - 1) > HMAC_KEY_BUFF_SIZE) ||
	    (tctx->hmac_key_len == 0)) {
		pr_err("HMAC key length %d is not supported!\n", tctx->hmac_key_len);
		kfree(ctx->buffer);
		ctx->buffer = NULL;
		return -EINVAL;
	}

	ctx->flags |= SHA_FLAGS_KEY_BLK;
	return 0;
}

static void ma35_sha_sg_to_dma_buffer(struct ahash_request *req, struct nu_sha_reqctx *ctx)
{
	int copy_len;

	while (ctx->sg && (ctx->req_len > 0) && (ctx->bufcnt < ctx->dma_max_size)) {

		copy_len = min((int)ctx->sg->length - ctx->sg_off, ctx->req_len);
		if (ctx->dma_max_size - ctx->bufcnt < copy_len)
			copy_len = ctx->dma_max_size - ctx->bufcnt;

		memcpy(&ctx->buffer[ctx->bufcnt],
		       (u8 *)sg_virt(ctx->sg) + ctx->sg_off, copy_len);

		ctx->bufcnt += copy_len;
		ctx->req_len -= copy_len;
		ctx->sg_off += copy_len;

		if (ctx->sg_off >= ctx->sg->length) {
			ctx->sg = sg_next(ctx->sg);
			ctx->sg_off = 0;
		}
	}
}

static int ma35_sha_update_start(struct nu_sha_dev *dd)
{
	struct nu_sha_reqctx *ctx = ahash_request_ctx(dd->req);
	int err = 0;

	if (READ_ONCE(ma35_crypto_optee_faulted) ||
	    (!(ctx->flags & SHA_FLAGS_FIRST) &&
	     !ma35_sha_tee_session_open(dd, ctx))) {
		ma35_sha_finish_req(ctx, -EIO);
		return -EIO;
	}
	if ((ctx->req_len > 0) &&  (ctx->bufcnt < ctx->dma_max_size))
		ma35_sha_sg_to_dma_buffer(dd->req, ctx);

	if (ctx->flags & SHA_FLAGS_KEY_BLK) {
		if ((ctx->flags & (SHA_FLAGS_FINUP | SHA_FLAGS_FINAL)) &&
		    (ctx->bufcnt == 0) && (dd->req->nbytes == 0)) {
			pr_err("ma35 hmac does not support 0 data length!\n");
			ma35_sha_finish_req(ctx, -EINVAL);
			return -EINVAL;
		}

		err = ma35_sha_dma_run(dd, 1);
		if (err != -EINPROGRESS)
			ma35_sha_finish_req(ctx, err); /* DMA trigger failed, abort! */

	} else if (ctx->bufcnt == ctx->dma_max_size) {
		/*
		 * DMA buffer is full, start DMA.
		 */

		/* Check if it's the final DMA */
		if ((ctx->flags & (SHA_FLAGS_FINUP | SHA_FLAGS_FINAL)) && (ctx->req_len == 0))
			ctx->flags |= SHA_FLAGS_FINAL_DMA;

		err = ma35_sha_dma_run(dd, 0);
		if (err != -EINPROGRESS)
			ma35_sha_finish_req(ctx, err); /* DMA trigger failed, abort! */

	} else if (ctx->flags & (SHA_FLAGS_FINUP | SHA_FLAGS_FINAL)) {
		/*
		 * This is the last block of the final update, or
		 * is the final request. It should be the last DMA.
		 * If key block was queued, process it first.
		 */

		ctx->flags |= SHA_FLAGS_FINAL_DMA;

		err = ma35_sha_dma_run(dd, 0);
		if (err != -EINPROGRESS)
			ma35_sha_finish_req(ctx, err); /* DMA trigger failed, abort! */

	} else {
		/*
		 * All data of this request were copy to DMA buffer.
		 * We can finish this request.
		 */
		ma35_sha_finish_req(ctx, 0);
		err = 0;
	}
	return err;
}

static int ma35_sha_handle_queue(struct nu_sha_dev *dd, struct ahash_request *req)
{
	struct crypto_async_request *async_req, *backlog;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&dd->lock, flags);

	if (req) {
		if (sha_tee.stopping) {
			spin_unlock_irqrestore(&dd->lock, flags);
			return -ESHUTDOWN;
		}
		if (READ_ONCE(ma35_crypto_optee_faulted)) {
			spin_unlock_irqrestore(&dd->lock, flags);
			return -EIO;
		}
		ret = ahash_enqueue_request(&dd->queue, req);
		queue_work(sha_tee.wq, &sha_tee.queue_work);
		spin_unlock_irqrestore(&dd->lock, flags);
		return ret;
	}

	if ((dd->flags & DD_FLAGS_BUSY)) {
		/* SHA device is busy on a request */
		spin_unlock_irqrestore(&dd->lock, flags);
		return ret;
	}

	backlog = crypto_get_backlog(&dd->queue);

	async_req = crypto_dequeue_request(&dd->queue);
	if (async_req)
		dd->flags |= DD_FLAGS_BUSY;

	spin_unlock_irqrestore(&dd->lock, flags);

	if (!async_req)
		return ret;

	if (backlog)
		backlog->complete(backlog, -EINPROGRESS);

	req = ahash_request_cast(async_req);
	dd->req = req;

	return ma35_sha_update_start(dd);
}

static void ma35_sha_dma_complete(struct nu_sha_reqctx *ctx)
{
	struct nu_sha_dev *dd = ctx->dd;

	ctx->flags &= ~SHA_FLAGS_FIRST;     /* clear FIRST flag anyway     */

	if (ctx->flags & SHA_FLAGS_KEY_BLK) {
		ctx->flags &= ~SHA_FLAGS_KEY_BLK;
		ma35_sha_update_start(dd);
		return;
	}

	ctx->bufcnt = 0; /* reset DMA buffer count */

	if (ctx->req_len == 0) {
		/* the current request H/W processing done */
		ma35_sha_finish_req(ctx, 0);
		return;
	}

	ma35_sha_update_start(dd);
}

static int ma35_sha_update(struct ahash_request *req)
{
	struct nu_sha_reqctx *ctx = ahash_request_ctx(req);

	ctx->sg = req->src;
	ctx->sg_off = 0;
	ctx->req_len = req->nbytes;

	ma35_sha_sg_to_dma_buffer(req, ctx);
	if (ctx->bufcnt + ctx->req_len <= ctx->dma_max_size)
		return 0;
	return ma35_sha_handle_queue(ctx->dd, req);
}

static int ma35_sha_final(struct ahash_request *req)
{
	struct nu_sha_reqctx *ctx = ahash_request_ctx(req);

	ctx->flags |= SHA_FLAGS_FINAL;

	return ma35_sha_handle_queue(ctx->dd, req);
}

static int ma35_sha_finup(struct ahash_request *req)
{
	struct nu_sha_reqctx *ctx = ahash_request_ctx(req);
	int err1, err2;

	ctx->flags |= SHA_FLAGS_FINUP;

	err1 = ma35_sha_update(req);
	if (err1 == -EINPROGRESS || (err1 == -EBUSY &&
	    (ahash_request_flags(req) & CRYPTO_TFM_REQ_MAY_BACKLOG)))
		return err1;

	/*
	 * final() has to be always called to cleanup resources
	 * even if update() failed, except EINPROGRESS
	 */
	err2 = ma35_sha_final(req);

	return err1 ?: err2;
}

static int ma35_sha_digest(struct ahash_request *req)
{
	return ma35_sha_init(req) ?: ma35_sha_finup(req);
}

static int ma35_sha_setkey(struct crypto_ahash *tfm, const u8 *key, u32 keylen)
{
	struct nu_sha_ctx *tctx = crypto_ahash_ctx(tfm);
	unsigned int block_size = crypto_ahash_blocksize(tfm);
	unsigned int padded_len;

	if (keylen > HMAC_KEY_BUFF_SIZE ||
	    (keylen && keylen + block_size - 1 > HMAC_KEY_BUFF_SIZE))
		return -EINVAL;

	padded_len = round_up(keylen, block_size);
	if (keylen)
		memcpy(tctx->keybuf, key, keylen);
	memset(tctx->keybuf + keylen, 0, padded_len - keylen);

	tctx->hmac_key_len = keylen;
	tctx->keybufcnt = padded_len;
	return 0;
}

static int ma35_sha_export(struct ahash_request *req, void *out)
{
	return -EOPNOTSUPP;
}

static int ma35_sha_import(struct ahash_request *req, const void *in)
{
	return -EOPNOTSUPP;
}

static int ma35_sha_cra_init_alg(struct crypto_tfm *tfm, const char *alg_base)
{
	struct nu_sha_ctx *tctx = crypto_tfm_ctx(tfm);
	struct nu_sha_dev *dd = ma35_sha_find_dev(tctx);

	dd = ma35_sha_find_dev(tctx);
	if (!dd)
		return -ENODEV;

	crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm), sizeof(struct nu_sha_reqctx));
	return 0;
}

static int ma35_sha_cra_init(struct crypto_tfm *tfm)
{
	// printk("SHA: %s\n", tfm->__crt_alg->cra_driver_name);
	return ma35_sha_cra_init_alg(tfm, NULL);
}

static void ma35_sha_cra_exit(struct crypto_tfm *tfm)
{
}

static struct ahash_alg  ma35_sha_algs[] = {
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA1_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha1",
		.cra_driver_name = "ma35-optee-sha1",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA1_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA224_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha224",
		.cra_driver_name = "ma35-optee-sha224",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA224_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA256_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha256",
		.cra_driver_name = "ma35-optee-sha256",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA256_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA384_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha384",
		.cra_driver_name = "ma35-optee-sha384",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA384_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA512_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha512",
		.cra_driver_name = "ma35-optee-sha512",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA512_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.setkey = ma35_sha_setkey,
	.halg.digestsize = SHA1_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "hmac(sha1)",
		.cra_driver_name = "ma35-optee-hmac-sha1",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA1_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.setkey = ma35_sha_setkey,
	.halg.digestsize = SHA224_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "hmac(sha224)",
		.cra_driver_name = "ma35-optee-hmac-sha224",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA224_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.setkey = ma35_sha_setkey,
	.halg.digestsize = SHA256_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "hmac(sha256)",
		.cra_driver_name = "ma35-optee-hmac-sha256",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA256_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.setkey = ma35_sha_setkey,
	.halg.digestsize = SHA384_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "hmac(sha384)",
		.cra_driver_name = "ma35-optee-hmac-sha384",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA384_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.setkey = ma35_sha_setkey,
	.halg.digestsize = SHA512_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "hmac(sha512)",
		.cra_driver_name = "ma35-optee-hmac-sha512",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA512_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SM3_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sm3",
		.cra_driver_name = "ma35-optee-sm3",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SM3_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = MD5_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "md5",
		.cra_driver_name = "ma35-optee-md5",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = MD5_HMAC_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
};

static struct ahash_alg  ma35_sha3_algs[] = {
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA3_224_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha3-224",
		.cra_driver_name = "ma35-optee-sha3-224",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA3_224_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA3_256_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha3-256",
		.cra_driver_name = "ma35-sha3-256",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA3_256_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA3_384_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha3-384",
		.cra_driver_name = "ma35-optee-sha3-384",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA3_384_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
{
	.init   = ma35_sha_init,
	.update = ma35_sha_update,
	.final  = ma35_sha_final,
	.finup  = ma35_sha_finup,
	.digest = ma35_sha_digest,
	.export = ma35_sha_export,
	.import = ma35_sha_import,
	.halg.digestsize = SHA3_512_DIGEST_SIZE,
	.halg.statesize = sizeof(struct nu_sha_reqctx),
	.halg.base = {
		.cra_name        = "sha3-512",
		.cra_driver_name = "ma35-sha3-512",
		.cra_priority    = 400,
		.cra_flags       = CRYPTO_ALG_ASYNC,
		.cra_blocksize   = SHA3_512_BLOCK_SIZE,
		.cra_ctxsize     = sizeof(struct nu_sha_ctx),
		.cra_alignmask   = 0,
		.cra_module      = THIS_MODULE,
		.cra_init        = ma35_sha_cra_init,
		.cra_exit        = ma35_sha_cra_exit,
	}
},
};

/*
 *  This task is triggerred by Crypto IRQ when a SHA DMA completed.
 */
static void ma35_sha_done_task(unsigned long data)
{
	struct nu_sha_dev *dd = (struct nu_sha_dev *)data;
	struct nu_sha_reqctx *ctx = ahash_request_ctx(dd->req);

	ma35_sha_tee_unmap(dd);
	ma35_sha_dma_complete(ctx);
}

static void ma35_sha_tee_queue_work(struct work_struct *work)
{
	struct ma35_sha_optee_state *tee =
		container_of(work, struct ma35_sha_optee_state, queue_work);

	ma35_sha_handle_queue(tee->dd, NULL);
}

static void ma35_sha_tee_done_work(struct work_struct *work)
{
	struct ma35_sha_optee_state *tee =
		container_of(work, struct ma35_sha_optee_state, done_work);

	ma35_sha_done_task((unsigned long)tee->dd);
}

static int optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	if (ver->impl_id == TEE_IMPL_ID_OPTEE)
		return 1;
	else
		return 0;
}

static int  ma35_optee_sha_init(struct nu_sha_dev *dd)
{
	struct tee_ioctl_open_session_arg sess_arg = { };
	int err;

	err = ma35_crypto_optee_init(dd->nu_cdev);
	if (err)
		return err;
	/*
	 * Open SHA context with TEE driver
	 */
	dd->octx = tee_client_open_context(NULL, optee_ctx_match, NULL, NULL);
	if (IS_ERR(dd->octx)) {
		err = PTR_ERR(dd->octx);
		dd->octx = NULL;
		return err;
	}

	/*
	 * Open SHA session with Crypto Trusted App
	 */
	memcpy(sess_arg.uuid, dd->nu_cdev->tee_cdev->id.uuid.b, TEE_IOCTL_UUID_LEN);
	sess_arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	sess_arg.num_params = 0;

	err = tee_client_open_session(dd->octx, &sess_arg, NULL);
	if ((err < 0) || (sess_arg.ret != 0)) {
		pr_err("%s open session failed, err: %x\n", __func__, sess_arg.ret);
		err = err < 0 ? err : -EIO;
		goto out_ctx;
	}
	dd->session_id = sess_arg.session;

	/*
	 * Allocate handshake buffer from OP-TEE share memory
	 */
	dd->shm_pool = tee_shm_alloc_kernel_buf(dd->octx, CRYPTO_SHM_SIZE);
	if (IS_ERR(dd->shm_pool)) {
		err = PTR_ERR(dd->shm_pool);
		goto out_sess;
	}

	dd->va_shm = tee_shm_get_va(dd->shm_pool, 0);
	if (IS_ERR(dd->va_shm)) {
		err = PTR_ERR(dd->va_shm);
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

static void ma35_optee_sha_exit(struct nu_sha_dev *dd)
{
	tee_shm_free(dd->shm_pool);
	tee_client_close_session(dd->octx, dd->session_id);
	tee_client_close_context(dd->octx);
	dd->octx = NULL;
}

static void ma35_sha_tee_stop(struct nu_sha_dev *dd)
{
	unsigned long flags;

	spin_lock_irqsave(&dd->lock, flags);
	sha_tee.stopping = true;
	spin_unlock_irqrestore(&dd->lock, flags);
	if (sha_tee.wq) {
		destroy_workqueue(sha_tee.wq);
		sha_tee.wq = NULL;
	}
	ma35_sha_tee_close_all(dd);
}

int ma35_sha_optee_probe(struct device *dev, struct nu_crypto_dev *crypto_dev)
{
	struct nu_sha_dev *sha_dd = &crypto_dev->sha_dd;
	int i, err = 0;
	int sha_registered = 0, sha3_registered = 0;

#ifdef CONFIG_CRYPTO_SELFTESTS
	/* ma35 sha-optee driver cannot pass some corner test of linux run-time test */
	pr_err("Please disable CONFIG_CRYPTO_SELFTESTS to have ma35 sha-optee driver support.\n");
	return -EINVAL;
#endif

	sha_dd->dev = dev;
	sha_dd->nu_cdev = crypto_dev;
	sha_dd->reg_base = crypto_dev->reg_base;
	sha_dd->octx = NULL;
	memset(&sha_tee, 0, sizeof(sha_tee));
	sha_tee.dd = sha_dd;

	err = ma35_optee_sha_init(sha_dd);
	if (err)
		return err;


	INIT_LIST_HEAD(&sha_dd->list);
	INIT_LIST_HEAD(&sha_tee.sessions);
	spin_lock_init(&sha_dd->lock);
	crypto_init_queue(&sha_dd->queue, 32);
	INIT_WORK(&sha_tee.queue_work, ma35_sha_tee_queue_work);
	INIT_WORK(&sha_tee.done_work, ma35_sha_tee_done_work);
	sha_tee.wq = alloc_ordered_workqueue("ma35-sha-optee",
						 WQ_MEM_RECLAIM);
	if (!sha_tee.wq) {
		err = -ENOMEM;
		goto err_optee;
	}

	spin_lock(&nu_sha.lock);
	list_add_tail(&sha_dd->list, &nu_sha.dev_list);
	spin_unlock(&nu_sha.lock);

	for (i = 0; i < ARRAY_SIZE(ma35_sha_algs); i++) {
		err = crypto_register_ahash(&ma35_sha_algs[i]);
		if (err) {
			dev_err(dev, "failed to register %s/%s: %d\n",
				ma35_sha_algs[i].halg.base.cra_name,
				ma35_sha_algs[i].halg.base.cra_driver_name,
				err);
			goto err_register;
		}
		sha_registered++;
	}

	for (i = 0; i < ARRAY_SIZE(ma35_sha3_algs); i++) {
		err = crypto_register_ahash(&ma35_sha3_algs[i]);
		if (err) {
			dev_err(dev, "failed to register %s/%s: %d\n",
				ma35_sha3_algs[i].halg.base.cra_name,
				ma35_sha3_algs[i].halg.base.cra_driver_name,
				err);
			goto err_register;
		}
		sha3_registered++;
	}

	sha_tee.registered = true;
	pr_info("ma35 crypto sha optee initialized.\n");
	return 0;

err_register:
	while (sha3_registered--)
		crypto_unregister_ahash(&ma35_sha3_algs[sha3_registered]);
	while (sha_registered--)
		crypto_unregister_ahash(&ma35_sha_algs[sha_registered]);
	spin_lock(&nu_sha.lock);
	list_del(&sha_dd->list);
	spin_unlock(&nu_sha.lock);
	ma35_sha_tee_stop(sha_dd);
err_optee:
	ma35_optee_sha_exit(sha_dd);
	dev_err(dev, "SHA initialization failed. %d\n", err);

	return err;
}

int ma35_sha_optee_remove(struct device *dev, struct nu_crypto_dev *crypto_dev)
{
	struct nu_sha_dev *sha_dd = &crypto_dev->sha_dd;
	int i;

	if (!sha_tee.registered)
		return 0;

	ma35_sha_tee_stop(sha_dd);

	for (i = 0; i < ARRAY_SIZE(ma35_sha_algs); i++)
		crypto_unregister_ahash(&ma35_sha_algs[i]);

	for (i = 0; i < ARRAY_SIZE(ma35_sha3_algs); i++)
		crypto_unregister_ahash(&ma35_sha3_algs[i]);

	spin_lock(&nu_sha.lock);
	list_del(&sha_dd->list);
	spin_unlock(&nu_sha.lock);

	ma35_optee_sha_exit(sha_dd);
	sha_tee.registered = false;

	return 0;
}
