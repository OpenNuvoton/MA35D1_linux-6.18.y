// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NUA3500 VC8000 H264 glue driver
 *
 * Copyright (C) 2020, Nuvoton Technology Corporation
 *
 */

#include <linux/types.h>
#include <linux/sort.h>
#include <linux/delay.h>
#include <media/v4l2-mem2mem.h>
#include <asm/io.h>  /* for ioremap and iounmap */

#include "hantro.h"
#include "hantro_hw.h"
#include "vc_codec_abi.h"

#include "vc_legacy_types.h"
#include "vc_dwl_abi.h"
#include "vc_pp_abi.h"


struct h264_ctx {
	struct vc_h264_info	decInfo;
	vc_codec_handle_t	decInst;     // is typdef (void *)
	struct vc_h264_input	decInput;
	struct vc_h264_output   decOutput;
	struct vc_h264_picture  decPicture;
	int		eos;
	int		picDecodeNumber;
	int		picDisplayNumber;
	vc_codec_handle_t		ppInst;
	struct vc_pp_config	ppConfig;
	bool		header_parsed;
};

static void __maybe_unused printDecodeReturn(struct hantro_dev *vpu, i32 retval);
static void __maybe_unused printH264PicCodingType(struct hantro_dev *vpu, u32 *picType);


static int  h264_pp_init(struct hantro_ctx *ctx);
static int  h264_pp_exit(struct hantro_ctx *ctx);
static void h264_pp_in_config(struct hantro_ctx *ctx);
static int  h264_pp_out_config(struct hantro_ctx *ctx);
void dump_buff_hex(u8 *pucBuff, int nBytes);

void  dump_buff_hex(u8 *pucBuff, int nBytes)
{
	int nIdx;

	nIdx = 0;
	while (nBytes >	0)
	{
		printk("0x%04X	%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", nIdx,
			pucBuff[nIdx], pucBuff[nIdx+1], pucBuff[nIdx+2], pucBuff[nIdx+3], pucBuff[nIdx+4], pucBuff[nIdx+5],
			pucBuff[nIdx+6], pucBuff[nIdx+7], pucBuff[nIdx+8], pucBuff[nIdx+9], pucBuff[nIdx+10],
			pucBuff[nIdx+11], pucBuff[nIdx+12], pucBuff[nIdx+13], pucBuff[nIdx+14], pucBuff[nIdx+15]);
		nIdx +=	16;
		nBytes -= 16;
		printk("\n");
	}
	printk("\n");
}


/*
 *  For monitoring frame rate
 */
static uint64_t		_fps_check_jiffy;
static int		_decode_cnt, _collect_cnt, _report_times;

int ma35d1_vc8k_init(struct hantro_dev *vpu)
{
	int ret = 0;

	if (vc_hw_engine_init(vpu) != 0)
	return -EIO;

	vpu->dcultra_base = ioremap(0x40260000, 0x2000);
	return ret;
}
EXPORT_SYMBOL(ma35d1_vc8k_init);

void ma35d1_vc8k_exit(struct hantro_dev *vpu)
{
}
EXPORT_SYMBOL(ma35d1_vc8k_exit);

/*
 *  called from hantro_start_streaming()   ctx->codec_ops->init
 */
int ma35d1_h264_dec_init(struct hantro_ctx *ctx)
{
	struct h264_ctx  *hctx;
	struct hantro_dev *vpu = ctx->dev;
	int err;

	if (vpu->vc8k_cfg.debug_level > 0)
		dev_info(vpu->dev, "h264_dec_init called.\n");
	ctx->vc8k_err = 0;

#if 0
	if (!IS_ERR(vpu->reset)) {
		printk("\n\nDo VC8000 reset.\n\n");
		reset_control_assert(vpu->reset);
		udelay(100);
		reset_control_deassert(vpu->reset);
	} else {
		dev_err(vpu->dev, "VC8K reset not inserted!!\n");
	}
#endif
	hctx = kzalloc(sizeof(*hctx), GFP_KERNEL);
	if (!hctx)
		return -ENOMEM;

	ctx->vc8k_data = hctx;
	hctx->picDecodeNumber = 1;
	hctx->picDisplayNumber = 1;
	hctx->header_parsed = false;

	err = vc_codec_h264_create(&hctx->decInst);

	if (err != VC_H264_OK) {
		dev_err(vpu->dev, "H264 DECODER INITIALIZATION FAILED\n");
		ctx->vc8k_err = -EIO;
		return -EIO;
	}

	vc_codec_h264_apply_hw_defaults(hctx->decInst);

	_fps_check_jiffy = jiffies;
	_report_times = 0;
	_collect_cnt = 0;
	_decode_cnt = 0;
	return 0;
}
EXPORT_SYMBOL(ma35d1_h264_dec_init);

void ma35d1_h264_dec_exit(struct hantro_ctx *ctx)
{
	struct h264_ctx  *hctx = ctx->vc8k_data;

	if (ctx->pp_ctx.enable_pp == true) {
		h264_pp_exit(ctx);
	}

	vc_codec_h264_destroy(hctx->decInst);

	if (ctx->vc8k_data != NULL) {
		kfree(ctx->vc8k_data);
		ctx->vc8k_data = NULL;
	}
}
EXPORT_SYMBOL(ma35d1_h264_dec_exit);

int ma35d1_h264_dec_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct h264_ctx  *hctx = ctx->vc8k_data;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	u64  t0;
	int  ret;

	if (!hctx->decInst) {
		dev_err(vpu->dev, "h264_dec_run - decInst NULL, H264 decoder not inited or aborted!\n");
		return -EIO;
	}

	if (ctx->vc8k_err)
		return ctx->vc8k_err;

	src_buf = hantro_get_src_buf(ctx);
	dst_buf = hantro_get_dst_buf(ctx);

	hctx->decInput.data_length = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	if (hctx->decInput.data_length >= 0x200000) {
		dev_dbg(vpu->dev, "Input len = %d, should be EOS.\n", hctx->decInput.data_length);
		dev_dbg(vpu->dev, "src last flag is %d\n", (src_buf->flags & V4L2_BUF_FLAG_LAST) ? 1 : 0);
		src_buf->flags |= V4L2_BUF_FLAG_LAST;
		dst_buf->flags |= V4L2_BUF_FLAG_LAST;
		return 0;
	}

	hctx->decInput.stream_bus_address = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	if (vpu->vc8k_cfg.use_dev_coherent) {
		hctx->decInput.stream = vpu->vc8k_cfg.res_mem_virt +
					 (hctx->decInput.stream_bus_address -
					 vpu->vc8k_cfg.res_mem_base);
	} else {
		hctx->decInput.stream = phys_to_virt(hctx->decInput.stream_bus_address);
		dma_sync_single_for_cpu(ctx->dev->dev, hctx->decInput.stream_bus_address,
				hctx->decInput.data_length , DMA_FROM_DEVICE);
	}

	// dev_info(vpu->dev, " h264_dec_run - %llx %llx %d, picture_id = %d\n", (u64)hctx->decInput.stream,
	//         (u64)hctx->decInput.stream_bus_address, hctx->decInput.data_length, hctx->decInput.picture_id);

	// dev_info(vpu->dev, "PIC:%d, %d\n", hctx->picDecodeNumber, hctx->decInput.data_length);

	hctx->decInput.picture_id = hctx->picDecodeNumber;

	mutex_lock(&vpu->lock);

	if ((ctx->pp_ctx.enable_pp == true) && (vpu->vc8k_cfg.pp_wait_vsync != 0)) {
		t0 = jiffies;
		while (jiffies - t0 < vpu->vc8k_cfg.pp_wait_vsync) {
			if (readl_relaxed(vpu->dcultra_base + 0x147C)) {
				// printk("%d", jiffies - t0);
				break;
			}
		}
	}

	if ((hctx->header_parsed == true) && ctx->pp_changed) {
		// dev_info(vpu->dev, "run-time pp changed.\n");
		ctx->pp_changed = 0;
		h264_pp_out_config(ctx);
		vc_codec_pp_set_config(hctx->ppInst, &hctx->ppConfig);
	}

	ret = vc_codec_h264_decode(hctx->decInst, &hctx->decInput, &hctx->decOutput);
	// printDecodeReturn(vpu, ret);

	if (ret == VC_H264_HDRS_RDY) {
		//dev_info(vpu->dev, "H264 headers were successfully decoded.\n");
		vc_codec_h264_get_info(hctx->decInst, &hctx->decInfo);

		dev_dbg(vpu->dev, "Width %d Height %d\n",
				 hctx->decInfo.width, hctx->decInfo.height);

		//dev_info(vpu->dev, "Cropping params: (%d, %d) %dx%d\n",
		//		 hctx->decInfo.crop.left_offset,
		//		 hctx->decInfo.crop.top_offset,
		//		 hctx->decInfo.crop.output_width,
		//		 hctx->decInfo.crop.output_height);

		//dev_info(vpu->dev, "MonoChrome = %d\n", hctx->decInfo.monochrome);
		//dev_info(vpu->dev, "Interlaced = %d\n", hctx->decInfo.interlaced_sequence);
		//dev_info(vpu->dev, "DPB mode   = %d\n", hctx->decInfo.dpb_mode);
		//dev_info(vpu->dev, "Pictures in DPB = %d\n", hctx->decInfo.picture_buffer_size);
		//dev_info(vpu->dev, "Pictures in Multibuffer PP = %d\n", hctx->decInfo.pp_multibuffer_size);
		if (hctx->decInfo.output_format == VC_H264_TILED_YUV420)
			dev_dbg(vpu->dev, "Output format = VC_H264_TILED_YUV420\n");
		else if (hctx->decInfo.output_format == VC_H264_YUV400)
			dev_dbg(vpu->dev, "Output format = VC_H264_YUV400\n");
		else
			dev_dbg(vpu->dev, "Output format = VC_H264_SEMIPLANAR_YUV420\n");

		if (ctx->pp_changed == -1) {
			/* PP of this context was not configured. Use global setting. */
			memcpy(&ctx->pp_ctx, &(vpu->vc8k_cfg.ppc), sizeof(struct vc8k_pp_params));
		}

		if (ctx->pp_ctx.enable_pp == true) {
			if (vpu->vc8k_cfg.debug_level > 0)
				dev_info(vpu->dev, "Init PP (h264_dec_run) %d x %d ==> PP output %d x %d\n",
					 hctx->decInfo.width, hctx->decInfo.height,
					 ctx->pp_ctx.img_out_w, ctx->pp_ctx.img_out_h);
			ctx->pp_changed = 0;
			ret = h264_pp_init(ctx);
			if (ret != 0) {
				dev_err(vpu->dev, "h264_dec_run %d - h264_pp_init failed! 0x%x\n", __LINE__, ret);
				mutex_unlock(&vpu->lock);
				ctx->vc8k_err = -EINVAL;
				return -EINVAL;
			}
			h264_pp_in_config(ctx);
			ret = vc_codec_pp_set_config(hctx->ppInst, &hctx->ppConfig);
			if (ret != VC_CODEC_PP_OK) {
				dev_err(vpu->dev, "h264_dec_run %d - PPSetConfig failed! 0x%x\n", __LINE__, ret);
				mutex_unlock(&vpu->lock);
				ctx->vc8k_err = -EINVAL;
				return -EINVAL;
			}
		}
		else
			dev_info(vpu->dev, "PP is not enabled!!\n");
		hctx->header_parsed = true;
		ret = vc_codec_h264_decode(hctx->decInst, &hctx->decInput, &hctx->decOutput);
		// printDecodeReturn(vpu, ret);
	}

	switch (ret) {

	case VC_H264_PENDING_FLUSH:
	//eos = 1;

	case VC_H264_PIC_DECODED:
	hctx->picDecodeNumber++;

	/* if output in display order is preferred, the decoder shall be forced
	 * to output pictures remaining in decoded picture buffer. Use function
	 * vc_codec_h264_next_picture() to obtain next picture in display order. Function
	 * is called until no more images are ready for display. Second parameter
	 * for the function is set to '1' to indicate that this is end of the
	 * stream and all pictures shall be output
	 */
	while (vc_codec_h264_next_picture(hctx->decInst, &hctx->decPicture, hctx->eos) == VC_H264_PIC_RDY) {
		//dev_info(vpu->dev, "PIC %d, view %d, type [%s:%s], ",
		//	 hctx->picDisplayNumber,
		//	 hctx->decPicture.view_id,
		//	 hctx->decPicture.is_idr[0] ? "IDR" : "NON-IDR",
		//	 hctx->decPicture.is_idr[1] ? "IDR" : "NON-IDR");
		/* pic coding type */
		// printH264PicCodingType(vpu, hctx->decPicture.coding_type);
		if (hctx->picDisplayNumber != hctx->decPicture.picture_id) {
			// dev_info(vpu->dev, ", decoded pic %d", hctx->decPicture.picture_id);
		}

		if (hctx->decPicture.error_macroblocks) {
			// dev_info(vpu->dev, ", concealed %d", hctx->decPicture.error_macroblocks);
		}

		if (hctx->decPicture.interlaced) {
			// dev_info(vpu->dev, ", INTERLACED ");
			if (hctx->decPicture.field_picture) {
				//dev_info(vpu->dev, "FIELD %s",
				//		hctx->decPicture.top_field ? "TOP" : "BOTTOM");

				//SetMissingField2Const((u8*)decPicture.output_picture,
				//                      decInfo.width,
				//                      decInfo.height,
				//                      decInfo.monochrome,
				//                      !decPicture.top_field );
			} else	{
				//dev_info(vpu->dev, "FRAME");
			}
		}

		//dev_info(vpu->dev, ", Crop: (%d, %d), %dx%d\n",
		//        decPicture.crop.left_offset,
		//        decPicture.crop.top_offset,
		//        decPicture.crop.output_width,
		//        decPicture.crop.output_height);

		// numErrors += decPicture.error_macroblocks;

		/*lint -esym(644, decInfo) always initialized if pictures
		 * available for display */

		/* Write output picture to file */
		// imageData = (u8 *) decPicture.output_picture;

		if (ctx->pp_ctx.enable_pp == false) {
			u8	*dst_data, *out_pic, *u_plane, *v_plane;
			u32	i, sizeimage = 0;
			dma_addr_t  dst_dma_addr;

			for (i = 0; i < dst_buf->vb2_buf.num_planes; i++) {
				sizeimage += ctx->dst_fmt.plane_fmt[i].sizeimage;
			}
			out_pic = (u8 *)hctx->decPicture.output_picture;

			//if (!vpu->vc8k_cfg.use_dev_coherent)
			//	dma_sync_single_for_cpu(ctx->dev->dev, hctx->decPicture.output_picture_bus_address,
			//				sizeimage, DMA_TO_DEVICE);

			/* Copy Y-plane */
			dst_dma_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
			dst_data = (u8 *)phys_to_virt(dst_dma_addr);
			memcpy(dst_data, out_pic, ctx->dst_fmt.plane_fmt[0].sizeimage);

			/* Copy UV-plane */
			if (dst_buf->vb2_buf.num_planes >= 3) {
				dst_dma_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 1);
				u_plane = (u8 *)phys_to_virt(dst_dma_addr);
				dst_dma_addr = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 2);
				v_plane = (u8 *)phys_to_virt(dst_dma_addr);
				out_pic += ctx->dst_fmt.plane_fmt[0].sizeimage;
				for (i = 0; i < ctx->dst_fmt.plane_fmt[1].sizeimage; i++) {
					u_plane[i] = out_pic[i * 2];
					v_plane[i] = out_pic[i * 2 + 1];
				}
			}
		} else {
			/* PP directly output to frame buffer. Do nothing. */
		}
		_decode_cnt++;
		if (jiffies - _fps_check_jiffy >= 1000) {
			_report_times++;
			_collect_cnt += _decode_cnt;
			if (_report_times == 10) {
				if (vpu->vc8k_cfg.debug_level > 0)
					dev_info(vpu->dev, "FPS: %d, Average %d.%d\n", _decode_cnt, _collect_cnt/10, _collect_cnt%10);
				_report_times = 0;
				_collect_cnt = 0;
			} else {
				if (vpu->vc8k_cfg.debug_level > 0)
					dev_info(vpu->dev, "FPS: %d\n", _decode_cnt);
			}
			_decode_cnt = 0;
			_fps_check_jiffy = jiffies;
		}

		/* Increment display number for every displayed picture */
		hctx->picDisplayNumber++;
	}
	break;

	case VC_H264_STRM_PROCESSED:
	case VC_H264_BUF_EMPTY:
	case VC_H264_NONREF_PIC_SKIPPED:
	case VC_H264_STRM_ERROR:
	/* Used to indicate that picture decoding needs to finalized prior to corrupting next picture
	 * picRdy = 0;
	 */
	break;

	case VC_H264_OK:
	/* nothing to do, just call again */
	break;
	case VC_H264_HW_TIMEOUT:
	dev_err(vpu->dev, "H264 HW Timeout\n");
	break;

	default:
	dev_err(vpu->dev, " h264_dec_run - unhandled decode return code 0x%x !\n", ret);
	ret = -EIO;
	ctx->vc8k_err = ret;
	mutex_unlock(&vpu->lock);
	return ret;
	}

	if (hctx->decOutput.data_left != 0)
	  dev_warn(vpu->dev, " decOutput.data_left = %d\n", hctx->decOutput.data_left);

	mutex_unlock(&vpu->lock);
	return 0;
}
EXPORT_SYMBOL(ma35d1_h264_dec_run);


/*------------------------------------------------------------------------------

	Function name:  bsdDecodeReturn

	Purpose: Print out decoder return value

------------------------------------------------------------------------------*/
static void __maybe_unused printDecodeReturn(struct hantro_dev *vpu, i32 retval)
{

	dev_dbg(vpu->dev, " >>> vc_codec_h264_decode returned: ");
	switch (retval)
	{

	case VC_H264_OK:
	dev_dbg(vpu->dev, "VC_H264_OK\n");
	break;
	case VC_H264_NONREF_PIC_SKIPPED:
	dev_dbg(vpu->dev, "VC_H264_NONREF_PIC_SKIPPED\n");
	break;
	case VC_H264_STRM_PROCESSED:
	dev_dbg(vpu->dev, "VC_H264_STRM_PROCESSED\n");
	break;
	case VC_H264_BUF_EMPTY:
	dev_dbg(vpu->dev, "VC_H264_BUF_EMPTY\n");
	break;
	case VC_H264_PIC_RDY:
	dev_dbg(vpu->dev, "VC_H264_PIC_RDY\n");
	break;
	case VC_H264_PIC_DECODED:
	dev_dbg(vpu->dev, "VC_H264_PIC_DECODED\n");
	break;
	case VC_H264_ADVANCED_TOOLS:
	dev_dbg(vpu->dev, "VC_H264_ADVANCED_TOOLS\n");
	break;
	case VC_H264_HDRS_RDY:
	dev_dbg(vpu->dev, "VC_H264_HDRS_RDY\n");
	break;
	case VC_H264_STREAM_NOT_SUPPORTED:
	dev_dbg(vpu->dev, "VC_H264_STREAM_NOT_SUPPORTED\n");
	break;
	case VC_H264_DWL_ERROR:
	dev_dbg(vpu->dev, "VC_H264_DWL_ERROR\n");
	break;
	case VC_H264_HW_TIMEOUT:
	dev_dbg(vpu->dev, "VC_H264_HW_TIMEOUT\n");
	break;
	case VC_H264_PENDING_FLUSH:
	dev_dbg(vpu->dev, "VC_H264_PENDING_FLUSH\n");
	break;
	default:
	dev_dbg(vpu->dev, "Other %d\n", retval);
	break;
	}
}

static void __maybe_unused printH264PicCodingType(struct hantro_dev *vpu, u32 *picType)
{
	dev_dbg(vpu->dev, "Coding type ");
	switch (picType[0])
	{
	case DEC_PIC_TYPE_I:
	dev_dbg(vpu->dev, "[I:");
	break;
	case DEC_PIC_TYPE_P:
	dev_dbg(vpu->dev, "[P:");
	break;
	case DEC_PIC_TYPE_B:
	dev_dbg(vpu->dev, "[B:");
	break;
	default:
	dev_dbg(vpu->dev, "[Other %d:", picType[0]);
	break;
	}

	switch (picType[1])
	{
	case DEC_PIC_TYPE_I:
	dev_dbg(vpu->dev, "I]");
	break;
	case DEC_PIC_TYPE_P:
	dev_dbg(vpu->dev, "P]");
	break;
	case DEC_PIC_TYPE_B:
	dev_dbg(vpu->dev, "B]");
	break;
	default:
	dev_dbg(vpu->dev, "Other %d]", picType[1]);
	break;
	}
}

static int h264_pp_init(struct hantro_ctx *ctx)
{
	struct h264_ctx  *hctx = ctx->vc8k_data;
	struct hantro_dev *vpu = ctx->dev;
	int   ret;

	if (!hctx->decInst) {
		dev_err(vpu->dev, "%s - H264 not inited!\n", __func__);
		return -1;
	}

	//for (i = 60; i <= 100; i++)
	//	vc8k_write_swreg(0, i);

	ret = vc_codec_pp_create(&hctx->ppInst);
	if (ret != VC_CODEC_PP_OK) {
		dev_err(vpu->dev, "%s - failed to create PP\n", __func__);
		return -1;
	}

	ret = vc_codec_pp_enable_combined(hctx->ppInst, hctx->decInst, VC_CODEC_PP_TYPE_H264);
	if (ret != VC_CODEC_PP_OK) {
		dev_err(vpu->dev, "%s - failed to enable combined mode\n", __func__);
		goto cleanup_pp;
	}

	// get the current default PP config
	memset (&hctx->ppConfig, 0, sizeof(hctx->ppConfig));
	ret = vc_codec_pp_get_config(hctx->ppInst, &hctx->ppConfig);
	if (ret != VC_CODEC_PP_OK) {
		dev_err(vpu->dev, "%s - failed to get default PP config\n", __func__);
		goto cleanup_combined;
	}

//	ret = h264_pp_out_config(ctx);
//	if (ret != 0)
//		goto cleanup_combined;

	return 0;

cleanup_combined:
	vc_codec_pp_disable_combined(hctx->ppInst, hctx->decInst);

cleanup_pp:
	vc_codec_pp_release(hctx->ppInst);
	return ret;
}

static int h264_pp_exit(struct hantro_ctx *ctx)
{
	struct h264_ctx  *hctx = ctx->vc8k_data;

	vc_codec_pp_disable_combined(hctx->ppInst, hctx->decInst);
	vc_codec_pp_release(hctx->ppInst);
	return 0;
}

static int h264_pp_out_config(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct h264_ctx  *hctx = ctx->vc8k_data;
	struct vc8k_pp_params *pp = &(ctx->pp_ctx);

	hctx->ppConfig.input_rotation = pp->rotation;
	hctx->ppConfig.output.width = pp->img_out_w;
	hctx->ppConfig.output.height = pp->img_out_h;

	if ((pp->img_out_x != 0) || (pp->img_out_y != 0) ||
		(pp->img_out_w != pp->frame_buf_w) || (pp->img_out_h != pp->frame_buf_h)) {
		hctx->ppConfig.framebuffer.enable = 1;
		hctx->ppConfig.framebuffer.write_origin_x = pp->img_out_x;
		hctx->ppConfig.framebuffer.write_origin_y = pp->img_out_y;
		hctx->ppConfig.framebuffer.width = pp->frame_buf_w;
		hctx->ppConfig.framebuffer.height = pp->frame_buf_h;
	}

	if (pp->img_out_fmt == V4L2_PIX_FMT_NV12) {
		hctx->ppConfig.output.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;
	} else {
		/*
		 * PP output RGB format
		 */
		hctx->ppConfig.rgb.transform = VC_PP_YCBCR2RGB_TRANSFORM_CUSTOM;
		hctx->ppConfig.rgb.alpha = 0xFF;
		hctx->ppConfig.rgb.coefficients.a = 298;
		hctx->ppConfig.rgb.coefficients.b = 409;
		hctx->ppConfig.rgb.coefficients.c = 208;
		hctx->ppConfig.rgb.coefficients.d = 100;
		hctx->ppConfig.rgb.coefficients.e = 516;
		hctx->ppConfig.rgb.dithering_enable = 1;

		if (pp->img_out_fmt == V4L2_PIX_FMT_RGB565) {
			hctx->ppConfig.output.pix_format = VC_PP_PIX_FMT_RGB16_5_6_5;
		} else {
			/*
			 * should be RGB888, no need to check
			 */
			hctx->ppConfig.output.pix_format  = VC_PP_PIX_FMT_RGB32;
		}
	}

	if (pp->pp_out_dst == 1)
		hctx->ppConfig.output.buffer_bus_addr = readl_relaxed(vpu->dcultra_base + 0x15C0);
	else
		hctx->ppConfig.output.buffer_bus_addr = readl_relaxed(vpu->dcultra_base + 0x1400);
	hctx->ppConfig.output.buffer_chroma_bus_addr = hctx->ppConfig.output.buffer_bus_addr +
			hctx->ppConfig.output.width * hctx->ppConfig.output.height;

	if ((hctx->ppConfig.output.buffer_bus_addr < 0x80000000UL) ||
	    (hctx->ppConfig.output.buffer_bus_addr > 0xC0000000UL)) {
		dev_err(vpu->dev, "%s - Invalid PP output address! 0x%llx\n",
			__func__, hctx->ppConfig.output.buffer_bus_addr);
		return -1;
	}
	return 0;
}

static void h264_pp_in_config(struct hantro_ctx *ctx)
{
	struct h264_ctx  *hctx = ctx->vc8k_data;

	h264_pp_out_config(ctx);

	hctx->ppConfig.input_crop_enable = 0;   /* crop is not supported in current release */
	hctx->ppConfig.input.video_range = 1;
	hctx->ppConfig.input.width = hctx->decInfo.width;
	hctx->ppConfig.input.height = hctx->decInfo.height;
	hctx->ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;
}
