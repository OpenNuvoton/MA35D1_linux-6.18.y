// SPDX-License-Identifier: GPL-2.0
/*
 * Hantro VPU driver - Nuvoton MA35D1 VC8000NanoD variant
 *
 * H.264 stateless decode. The decoder currently outputs NV12 directly;
 * the post-processor (format conversion and scaling) is not used yet and
 * is planned as future work.  Register layout is G1-compatible, so the
 * mainline G1 H.264 code is reused unchanged.
 */

#include <linux/module.h>

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_g1_regs.h"

static const struct hantro_fmt ma35d1_vpu_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.codec_mode = HANTRO_MODE_NONE,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_FHD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_FHD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.codec_mode = HANTRO_MODE_H264_DEC,
		.max_depth = 2,
		.frmsize = {
			.min_width = FMT_MIN_WIDTH,
			.max_width = FMT_FHD_WIDTH,
			.step_width = MB_DIM,
			.min_height = FMT_MIN_HEIGHT,
			.max_height = FMT_FHD_HEIGHT,
			.step_height = MB_DIM,
		},
	},
};

static void ma35d1_vpu_reset(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;

	vdpu_write(vpu, G1_REG_INTERRUPT_DEC_IRQ_DIS, G1_REG_INTERRUPT);
	vdpu_write(vpu, G1_REG_CONFIG_DEC_CLK_GATE_E, G1_REG_CONFIG);
}

static const struct hantro_codec_ops ma35d1_vpu_codec_ops[] = {
	[HANTRO_MODE_H264_DEC] = {
		.run = hantro_g1_h264_dec_run,
		.reset = ma35d1_vpu_reset,
		.init = hantro_h264_dec_init,
		.exit = hantro_h264_dec_exit,
	},
};

static const struct hantro_irq ma35d1_vpu_irqs[] = {
	{ "vpu", hantro_g1_irq },
};

static const char * const ma35d1_vpu_clk_names[] = { "vpu" };

const struct hantro_variant ma35d1_vpu_variant = {
	.dec_fmts = ma35d1_vpu_fmts,
	.num_dec_fmts = ARRAY_SIZE(ma35d1_vpu_fmts),
	.codec = HANTRO_H264_DECODER,
	.codec_ops = ma35d1_vpu_codec_ops,
	.irqs = ma35d1_vpu_irqs,
	.num_irqs = ARRAY_SIZE(ma35d1_vpu_irqs),
	.clk_names = ma35d1_vpu_clk_names,
	.num_clocks = ARRAY_SIZE(ma35d1_vpu_clk_names),
};
