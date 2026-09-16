// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU codec driver for Nuvoton MA35D1 VC8K
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright (C) 2020, Nuvoton Technology Corporation
 *
 */

#include <linux/clk.h>
#include <linux/smp.h>

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_jpeg.h"
#include "hantro_g1_regs.h"
#include "hantro_h1_regs.h"

/*
 * Supported formats.
 */

static const struct hantro_fmt ma35d1_vpu_dec_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 16000,
			.step_width = H264_MB_DIM,
			.min_height = 48,
			.max_height = 16000,
			.step_height = H264_MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV420M,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 16000,
			.step_width = H264_MB_DIM,
			.min_height = 48,
			.max_height = 16000,
			.step_height = H264_MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV422M,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 16000,
			.step_width = H264_MB_DIM,
			.min_height = 48,
			.max_height = 16000,
			.step_height = H264_MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_ARGB32,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = VC8K_MAX_WIDTH,
			.step_width = H264_MB_DIM,
			.min_height = 48,
			.max_height = VC8K_MAX_HEIGHT,
			.step_height = H264_MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_RGB565,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = VC8K_MAX_WIDTH,
			.step_width = H264_MB_DIM,
			.min_height = 48,
			.max_height = VC8K_MAX_HEIGHT,
			.step_height = H264_MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_YUV420,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 16000,
			.step_width = H264_MB_DIM,
			.min_height = 48,
			.max_height = 16000,
			.step_height = H264_MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_H264,
		.codec_mode = HANTRO_MODE_H264_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 48,
			.max_width = VC8K_MAX_WIDTH,
			.step_width = 4,
			.min_height = 48,
			.max_height = VC8K_MAX_HEIGHT,
			.step_height = 4,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.codec_mode = HANTRO_MODE_H264_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = 48,
			.max_width = VC8K_MAX_WIDTH,
			.step_width = 4,
			.min_height = 48,
			.max_height = VC8K_MAX_HEIGHT,
			.step_height = 4,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_JPEG,
		.codec_mode = HANTRO_MODE_JPEG_DEC,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 16000,
			.step_width = 4,
			.min_height = 48,
			.max_height = 16000,
			.step_height = 4,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 16000,
			.step_width = 8,
			.min_height = 48,
			.max_height = 16000,
			.step_height = 8,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_ABGR32,
		.codec_mode = HANTRO_MODE_NONE,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = VC8K_MAX_WIDTH,
			.step_width = 8,
			.min_height = 48,
			.max_height = VC8K_MAX_HEIGHT,
			.step_height =8,
		},
	},
#if 0
	{
		.fourcc = V4L2_PIX_FMT_MJPEG,
		.codec_mode = HANTRO_MODE_JPEG_DEC,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 16000,
			.step_width = 4,
			.min_height = 48,
			.max_height = 16000,
			.step_height = 4,
		},
	},
#endif
#if 0
	{
		.fourcc = V4L2_PIX_FMT_MA35D1_PP,
		.codec_mode = HANTRO_MODE_PP,
		.max_depth = 1,
		.frmsize = {
			.min_width = 48,
			.max_width = 8192,
			.step_width = 8,
			.min_height = 48,
			.max_height = 8192,
			.step_height = 8,
		},
	},
#endif
};


static irqreturn_t ma35d1_vdpu_irq(int irq, void *dev_id)
{
	struct hantro_dev *vpu = dev_id;
	u32 status;

	status = vdpu_read(vpu, G1_REG_INTERRUPT);
	// printk("[%d] - vdec_irq, 0x%x\n", smp_processor_id(), status);
	if (status & 0x100) {
		vpu->dec_state = (status & G1_REG_INTERRUPT_DEC_RDY_INT) ?
				VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;
		if (vpu->dec_state != VB2_BUF_STATE_DONE)
			dev_err(vpu->dev, "%d DECODE ERROR !!  0x%x\n", __LINE__, status);

		//if (status & (1 << 24))
		//	vpu->B_slice_detected = true;
		vc_hw_engine_irq();
		vdpu_write(vpu, 0, G1_REG_INTERRUPT);
	}

	status = vdpu_read(vpu, 60 * 4);     // swreg60 for PP
	if (status & 0x100) {
		vpu->dec_state = (status & G1_REG_INTERRUPT_DEC_RDY_INT) ?
				VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;
		if (vpu->dec_state != VB2_BUF_STATE_DONE)
			dev_err(vpu->dev, "%d DECODE ERROR !!  0x%x\n", __LINE__, status);
		vc_hw_engine_irq();
		vdpu_write(vpu, 0, 60 * 4);  // clear swreg60
		if (vpu->dcultra_base)
			writel_relaxed(readl_relaxed(vpu->dcultra_base + 0x1518) | 0x11,
					(vpu->dcultra_base + 0x1518));  // trigger display
	}
	return IRQ_HANDLED;
}

static int ma35d1_vpu_hw_init(struct hantro_dev *vpu)
{
	return 0;
}

void hantro_g1_h264_dec_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	/* Prepare the H264 decoder context. */
	hantro_prepare_run(ctx);

	hantro_finish_run(ctx);

	vpu->dec_state = VB2_BUF_STATE_ERROR;  /* should be updated by decode done */

	ma35d1_h264_dec_run(ctx);

	hantro_irq_done(vpu, 0, vpu->dec_state);
}
EXPORT_SYMBOL(hantro_g1_h264_dec_run);

void hantro_g1_jpeg_dec_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	/* Prepare the JPEG decoder context. */
	hantro_prepare_run(ctx);

	hantro_finish_run(ctx);

	vpu->dec_state = VB2_BUF_STATE_ERROR;  /* should be updated by decode done */

	ma35d1_jpeg_dec_run(ctx);

	hantro_irq_done(vpu, 0, vpu->dec_state);
}
EXPORT_SYMBOL(hantro_g1_jpeg_dec_run);

void hantro_g1_pp_run(struct hantro_ctx *ctx)
{

	hantro_prepare_run(ctx);

	hantro_finish_run(ctx);

	ma35d1_pp_run(ctx);
}
EXPORT_SYMBOL(hantro_g1_pp_run);


static void ma35d1_vpu_dec_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	vdpu_write(vpu, G1_REG_INTERRUPT_DEC_IRQ_DIS, G1_REG_INTERRUPT);
	vdpu_write(vpu, G1_REG_CONFIG_DEC_CLK_GATE_E, G1_REG_CONFIG);
	vdpu_write(vpu, 1, G1_REG_SOFT_RESET);
}

/*
 * Supported codec ops.
 */

static const struct hantro_codec_ops ma35d1_vpu_codec_ops[] = {
	[HANTRO_MODE_H264_DEC] = {
		.run = hantro_g1_h264_dec_run,
		.reset = ma35d1_vpu_dec_reset,
		.init = ma35d1_h264_dec_init,
		.exit = ma35d1_h264_dec_exit,
	},
	[HANTRO_MODE_JPEG_DEC] = {
		.run = hantro_g1_jpeg_dec_run,
		.reset = ma35d1_vpu_dec_reset,
		.init = ma35d1_jpeg_dec_init,
		.exit = ma35d1_jpeg_dec_exit,
	},

	[HANTRO_MODE_PP] = {
		.run = hantro_g1_pp_run,
		.reset = ma35d1_vpu_dec_reset,
		.init = ma35d1_pp_init,
		.exit = NULL,
	},
};

/*
 * VPU variant.
 */

static const struct hantro_irq ma35d1_vpu_irqs[] = {
	{ "vdpu", ma35d1_vdpu_irq },
};

static const char * const ma35d1_vc8k_clk_names[] = {
	"aclk", "hclk"
};

const struct hantro_variant ma35d1_vpu_variant = {
	.dec_offset = 0x0,
	.dec_fmts = ma35d1_vpu_dec_fmts,
	.num_dec_fmts = ARRAY_SIZE(ma35d1_vpu_dec_fmts),
	.num_enc_fmts = 0,
	.codec = HANTRO_JPEG_DECODER | HANTRO_H264_DECODER,
	.codec_ops = ma35d1_vpu_codec_ops,
	.irqs = ma35d1_vpu_irqs,
	.num_irqs = ARRAY_SIZE(ma35d1_vpu_irqs),
	.init = ma35d1_vpu_hw_init,
	.clk_names = ma35d1_vc8k_clk_names,
	.num_clocks = ARRAY_SIZE(ma35d1_vc8k_clk_names)
};
EXPORT_SYMBOL(ma35d1_vpu_variant);
