// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton MA35D1 VC8000 H264 glue driver
 *
 * Copyright (C) 2020, Nuvoton Technology Corporation
 *
 */

#include <linux/types.h>
#include <linux/sort.h>
#include <media/v4l2-mem2mem.h>

#include "hantro.h"
#include "hantro_hw.h"

#include "vc_legacy_types.h"
#include "vc_os_linux.h"
#include "vc_dwl_abi.h"
#include "vc_codec_abi.h"


typedef enum {
	PP_FORMAT_NV12,
	PP_FORMAT_YUV420P,
	PP_FORMAT_YUV444,
} pp_format_t;


struct vc8k_pp_ctx {
	int		width;	
	int		height;
	pp_format_t	out_format;
};

// static struct vc8k_pp_ctx  _vc8k_ctx;


int ma35d1_pp_init(struct hantro_ctx *ctx)
{
	return 0;
}
EXPORT_SYMBOL(ma35d1_pp_init);


int ma35d1_pp_run(struct hantro_ctx *ctx)
{
	// struct vc8k_pp_ctx   *pctx = &_vc8k_ctx;
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct v4l2_pix_format_mplane  *src_pix_mp, *dst_pix_mp;
	dma_addr_t	src_dma_y, dst_dma_y;
	u8	*src_va_y, *dst_va_y;
	u32	src_len, dst_len, reg_val;

	src_buf = hantro_get_src_buf(ctx);
	dst_buf = hantro_get_dst_buf(ctx);
	src_pix_mp = &ctx->src_fmt;
	dst_pix_mp = &ctx->dst_fmt;

	src_dma_y = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	src_va_y = phys_to_virt(src_dma_y);
	src_len = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	
	dev_info(vpu->dev, "[%s] - src: %dx%d, %c%c%c%c, len=%d\n", __func__, src_pix_mp->width, src_pix_mp->height,
                  (src_pix_mp->pixelformat & 0x7f),
		  (src_pix_mp->pixelformat >> 8) & 0x7f,
		  (src_pix_mp->pixelformat >> 16) & 0x7f,
		  (src_pix_mp->pixelformat >> 24) & 0x7f,
		  src_len);
		
	dst_dma_y = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	dst_va_y = phys_to_virt(dst_dma_y);
	dst_len = vb2_get_plane_payload(&dst_buf->vb2_buf, 0);
	
	dev_info(vpu->dev, "[%s] - dst: %dx%d, %c%c%c%c, len=%d\n", __func__, dst_pix_mp->width, dst_pix_mp->height,
                  (dst_pix_mp->pixelformat & 0x7f),
		  (dst_pix_mp->pixelformat >> 8) & 0x7f,
		  (dst_pix_mp->pixelformat >> 16) & 0x7f,
		  (dst_pix_mp->pixelformat >> 24) & 0x7f,
		  dst_len); 
	
	dma_sync_single_for_cpu(ctx->dev->dev, src_dma_y, 
					src_len , DMA_FROM_DEVICE);

	/*-------------------------------------------------------------*/
	/*  write PP swreg61 ~ swreg72                                 */
	/*-------------------------------------------------------------*/
	vc_os_mmio_write32(61, 0x3cf0);
	vc_os_mmio_write32(63, src_dma_y);
	vc_os_mmio_write32(64, src_dma_y + src_pix_mp->width * src_pix_mp->height);
	vc_os_mmio_write32(66, dst_dma_y);
	vc_os_mmio_write32(67, dst_dma_y + dst_pix_mp->width * dst_pix_mp->height);
	vc_os_mmio_write32(72, ((src_pix_mp->width+15)/16) | (((src_pix_mp->height+15)/16)<<9));

	// dev_info(vpu->dev, "PP dma_src: 0x%llx, dma_dst: 0x%llx\n", src_dma_y, dst_dma_y);
	vc_os_mmio_write32(73, 0xffffffff);
	vc_os_mmio_write32(74, 0xffffffff);

	/*-------------------------------------------------------------*/
	/*  write PP swreg79                                           */
	/*-------------------------------------------------------------*/
	reg_val = VC_CODEC_PP_FIXED_DIV(VC_CODEC_PP_TO_FIXED((dst_pix_mp->width - 1), 16), (src_pix_mp->width - 1));
	vc_os_mmio_write32(79, reg_val);

	/*-------------------------------------------------------------*/
	/*  write PP swreg80                                           */
	/*-------------------------------------------------------------*/
	reg_val = VC_CODEC_PP_FIXED_DIV(VC_CODEC_PP_TO_FIXED((dst_pix_mp->height - 1), 16), (src_pix_mp->height - 1));

	if (dst_pix_mp->height > src_pix_mp->height)
		reg_val |= (0x1 << 23);		/* vertical upscale */
	else if (dst_pix_mp->height < src_pix_mp->height)
		reg_val |= (0x2 << 23);		/* vertical downscale */

	if (dst_pix_mp->width > src_pix_mp->width)
		reg_val |= (0x1 << 25);		/* horizontal upscale */
	else if (dst_pix_mp->width < src_pix_mp->width)
		reg_val |= (0x2 << 25);		/* horizontal downscale */

	vc_os_mmio_write32(80, reg_val);

	/*-------------------------------------------------------------*/
	/*  write PP swreg81                                           */
	/*-------------------------------------------------------------*/
	reg_val = VC_CODEC_PP_FIXED_DIV(VC_CODEC_PP_TO_FIXED((src_pix_mp->width - 1), 16), (dst_pix_mp->width - 1));
	reg_val = (reg_val << 16) | 
		  (VC_CODEC_PP_FIXED_DIV(VC_CODEC_PP_TO_FIXED((src_pix_mp->height - 1), 16), (dst_pix_mp->height - 1)));
	vc_os_mmio_write32(81, reg_val);
	
	/*-------------------------------------------------------------*/
	/*  write PP swreg85                                           */
	/*-------------------------------------------------------------*/
	reg_val = 0;
	reg_val |= (VC_CODEC_PP_ASIC_IN_FORMAT_420_SEMIPLANAR << 29);
	
	if (src_pix_mp->pixelformat == V4L2_PIX_FMT_NV12)
		reg_val |= (VC_CODEC_PP_ASIC_OUT_FORMAT_420 << 26);
	else if (src_pix_mp->pixelformat == V4L2_PIX_FMT_YUV420)
		reg_val |= (VC_CODEC_PP_ASIC_OUT_FORMAT_420 << 26);
	
	reg_val |= (dst_pix_mp->height << 15);
	reg_val |= (dst_pix_mp->width << 4);
	vc_os_mmio_write32(85, reg_val);

	/*-------------------------------------------------------------*/
	/*  write PP swreg88                                           */
	/*-------------------------------------------------------------*/
	reg_val = ((src_pix_mp->width+15)/16) << 23;
	vc_os_mmio_write32(88, reg_val);

	/*-------------------------------------------------------------*/
	/*  write PP swreg92                                           */
	/*-------------------------------------------------------------*/
	reg_val = (dst_pix_mp->width << 0);
	vc_os_mmio_write32(92, reg_val);

#if 0
	for (i = 60; i <= 92; i++)
		dev_info(vpu->dev, "swreg%d = 0x%x\n", i, vc_os_mmio_read32(i));
#endif

	/*  
	 *  swreg60[0] - sw_pp_e: External mode post-processing enable.
	 *  swreg60[4] - sw_pp_irq_dis: Post-processor IRQ disabled. 
	 */
	vc_os_mmio_write32(60, 0x11);	
	
	/* 
	 *  waiting for swreg60[12] sw_pp_rdy_int: Interrupt status bit post processor
	 */
	while (!(vc_os_mmio_read32(60) & (1<<12))) ;
	
	// dev_info(vpu->dev, "PP done.\n");

	//dma_sync_single_for_cpu(ctx->dev->dev, dst_dma_y, 
	//		(dst_pix_mp->width * dst_pix_mp->height * 3)/2, DMA_TO_DEVICE);
	return 0;
}
EXPORT_SYMBOL(ma35d1_pp_run);
