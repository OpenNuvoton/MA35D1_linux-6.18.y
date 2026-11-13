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
#include "vc_codec_abi.h"

#include "vc_legacy_types.h"
#include "vc_dwl_abi.h"
#include "vc_pp_abi.h"

static vc_codec_handle_t  jpegInst;
static struct vc_jpeg_input jpegIn;
static struct vc_jpeg_output jpegOut;

static u32  streamTotalLen;
static u32  streamInFile;
static u32  streamSeekLen;
static u32  imageInfoLength;

static u32  thumbInStream;
static u32  onlyFullResolution = 1;
static u32  mode;    /* 1: THUMBNAIL; 0: FULL RESOLUTION */
static u32  amountOfMCUs;
static u32  mcuInRow;
static u32  mcuSizeDivider;
static u32  progressive;
static u32  nonInterleaved;
static u32  ThumbDone;
static u32  slicedOutputUsed;
static int  fullSliceCounter;
static u32  frameReady;
static u32  sizeLuma;
static u32  sizeChroma;
static u32  sliceToUser;
static u32  sliceSize;
static u32  nbrOfImagesToOut;
static u32  scanCounter;
static struct vc_jpeg_linear_mem outputAddressY;
static struct vc_jpeg_linear_mem outputAddressCbCr;
static u8   *OutImagePtr_Y, *OutImagePtr_U, *OutImagePtr_V;
static int  WriteOutImageBytesCnt;

/*
 *  For monitoring frame rate
 */
static u64 _fps_check_jiffy;
static int _decode_cnt, _collect_cnt, _report_times;

static vc_codec_handle_t	    ppInst;
static struct vc_pp_config	    ppConfig;

static int jpeg_pp_init(struct hantro_ctx *ctx);
static int jpeg_pp_exit(struct hantro_ctx *ctx);
static int jpeg_pp_in_config(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo);
static int jpeg_pp_out_config(struct hantro_ctx *ctx);
static void WriteOutput(u8 *dataLuma, u32 picSizeLuma, u8 *dataChroma,
			u32 picSizeChroma, u32 picMode);
static void WriteProgressiveOutput(u32 sizeLuma, u32 sizeChroma, u32 mode,
				   u8 *dataLuma, u8 *dataCb, u8 *dataCr);
static u32 FindImageInfoEnd(u8 *pStream, u32 stream_length, u32 *pOffset);
static void calcSize(struct vc_jpeg_image_info *imageInfo, u32 picMode);
static void handleSlicedOutput(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo,
			       struct vc_jpeg_input *jpegIn, struct vc_jpeg_output *jpegOut);
static void PrintJpegRet(struct hantro_dev *vpu, vc_s32 *pJpegRet);

static void jpeg_write_progressive(struct vc_jpeg_output *out, u32 pic_mode)
{
	WriteProgressiveOutput(sizeLuma, sizeChroma, pic_mode,
			       (u8 *)out->output_picture_y.virtual_address,
			       (u8 *)out->output_picture_cbcr.virtual_address,
			       (u8 *)out->output_picture_cr.virtual_address);
}

int ma35d1_jpeg_dec_init(struct hantro_ctx *ctx)
{
	vc_s32  jpegRet;

	/* Initialize variables */
	thumbInStream = 0;
	onlyFullResolution = 1;
	mode = 0;
	amountOfMCUs = 0;
	mcuInRow = 0;
	mcuSizeDivider = 0;
	progressive = 0;
	nonInterleaved = 0;
	ThumbDone = 0;
	slicedOutputUsed = 0;
	fullSliceCounter = -1;
	frameReady = 0;
	sizeLuma = 0;
	sizeChroma = 0;
	sliceToUser = 0;
	nbrOfImagesToOut = 0;
	scanCounter = 0;

	memset(&jpegInst, 0, sizeof(jpegInst));
	memset(&jpegIn, 0, sizeof(jpegIn));
	memset(&jpegOut, 0, sizeof(jpegOut));

	jpegRet = vc_codec_jpeg_create(&jpegInst);
	if (jpegRet != VC_JPEG_OK) {
		/* Handle here the error situation */
		return -1;
	}

	vc_codec_jpeg_apply_hw_defaults(jpegInst);

	_fps_check_jiffy = jiffies;
	_report_times = 0;
	_collect_cnt = 0;
	_decode_cnt = 0;

	return 0;
}
EXPORT_SYMBOL(ma35d1_jpeg_dec_init);

void ma35d1_jpeg_dec_exit(struct hantro_ctx *ctx)
{
	if (ctx->pp_ctx.enable_pp)
		jpeg_pp_exit(ctx);
	vc_codec_jpeg_destroy(jpegInst);
}
EXPORT_SYMBOL(ma35d1_jpeg_dec_exit);

int ma35d1_jpeg_dec_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	g1_addr_t src_dma;
	u8 *byteStrmStart;
	struct vc_jpeg_image_info imageInfo;
	u32 data_len, len;
	int jpegRet;
	u64 t0;

	src_buf = hantro_get_src_buf(ctx);
	dst_buf = hantro_get_dst_buf(ctx);

	src_dma = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	len = vb2_get_plane_payload(&src_buf->vb2_buf, 0);
	data_len = len;

	if (vpu->vc8k_cfg.use_dev_coherent) {
		byteStrmStart = vpu->vc8k_cfg.res_mem_virt +
				(src_dma - vpu->vc8k_cfg.res_mem_base);
	} else {
		byteStrmStart = phys_to_virt(src_dma);
		dma_sync_single_for_cpu(vpu->dev, src_dma, data_len, DMA_FROM_DEVICE);
	}

	/*-----------------------------------------------------------------*/
	/*  PHASE 2: OPEN/READ FILE					   */
	/*-----------------------------------------------------------------*/

	jpegIn.buffer_size = 0;
	streamTotalLen = len;
	streamInFile = streamTotalLen;
	streamSeekLen = 0;

	/* initialize vc_codec_jpeg_decode input structure */
	jpegIn.stream_buffer.bus_address = src_dma;
	jpegIn.stream_buffer.virtual_address = (u32 *)byteStrmStart;
	jpegIn.stream_length = len;

	/*-----------------------------------------------------------------*/
	/*  PHASE 3: GET IMAGE INFO					   */
	/*-----------------------------------------------------------------*/
	jpegRet = FindImageInfoEnd(byteStrmStart, len, &imageInfoLength);
	if (jpegRet != 0) {
		dev_err(vpu->dev, "%s - FindImageInfoEnd failed!\n", __func__);
		return -1;
	}

	/* Get image information of the JFIF and decode JFIF header */
	jpegRet = vc_codec_jpeg_get_image_info(jpegInst, &jpegIn, &imageInfo);
	if (jpegRet != VC_JPEG_OK) {
		dev_err(vpu->dev, "%s - vc_codec_jpeg_get_image_info failed!\n", __func__);
		return -1;
	}

	if (ctx->pp_changed == -1) {
		/* PP of this context was not configured. Use global setting. */
		memcpy(&ctx->pp_ctx, &vpu->vc8k_cfg.ppc, sizeof(struct vc8k_pp_params));
	}

	if (!ctx->is_pp_inited && ctx->pp_ctx.enable_pp) {
		if (jpeg_pp_init(ctx) != 0)
			return -1;
		ctx->is_pp_inited = 1;
		jpeg_pp_in_config(ctx, &imageInfo);
	}

	if (!ctx->pp_ctx.enable_pp) {
		struct vb2_buffer *vb = &dst_buf->vb2_buf;

		if (dst_buf->vb2_buf.num_planes < 3) {
			dev_err(vpu->dev, "%s - output num_planes=%d !\n", __func__,
				dst_buf->vb2_buf.num_planes);
			return -EINVAL;
		}
		OutImagePtr_Y = phys_to_virt(vb2_dma_contig_plane_dma_addr(vb, 0));
		OutImagePtr_U = phys_to_virt(vb2_dma_contig_plane_dma_addr(vb, 1));
		OutImagePtr_V = phys_to_virt(vb2_dma_contig_plane_dma_addr(vb, 2));
		WriteOutImageBytesCnt = 0;
	}

	/*  ******************** THUMBNAIL **************************** */
	/* Select if Thumbnail or full resolution image will be decoded */
	if (imageInfo.thumbnail_type == VC_JPEG_THUMBNAIL_JPEG) {
		/* if all thumbnails processed (MJPEG) */
		if (!ThumbDone)
			jpegIn.image_type = VC_JPEG_THUMBNAIL;
		else
			jpegIn.image_type = VC_JPEG_IMAGE;

		thumbInStream = 1;
	} else if (imageInfo.thumbnail_type == VC_JPEG_NO_THUMBNAIL) {
		jpegIn.image_type = VC_JPEG_IMAGE;
	} else if (imageInfo.thumbnail_type == VC_JPEG_THUMBNAIL_NOT_SUPPORTED_FORMAT) {
		jpegIn.image_type = VC_JPEG_IMAGE;
	}

	/* check if forced to decode only full resolution images  ==> discard thumbnail */
	if (onlyFullResolution)
		jpegIn.image_type = VC_JPEG_IMAGE;

	/*-----------------------------------------------------------------*/
	/*  Decode FRAME						   */
	/*-----------------------------------------------------------------*/
	/* TB SPECIFIC == LOOP IF THUMBNAIL IN JFIF */
	/* Decode JFIF */
	if (jpegIn.image_type == VC_JPEG_THUMBNAIL)
		mode = 1; /* TODO KIMA */
	else
		mode = 0;

	/* no slice mode supported in progressive || non-interleaved ==> force to full mode */
	if ((jpegIn.image_type == VC_JPEG_THUMBNAIL &&
	     imageInfo.coding_mode_thumb == VC_JPEG_PROGRESSIVE) ||
	    (jpegIn.image_type == VC_JPEG_IMAGE &&
	     imageInfo.coding_mode == VC_JPEG_PROGRESSIVE))
		jpegIn.slice_mb_set = 0;

	/* if input (only full, not tn) > 4096 MCU */
	/* ==> force to slice mode */
	if (mode == 0) {
		/* calculate MCU's */
		if (imageInfo.output_format == VC_JPEG_YCBCR400 ||
		    imageInfo.output_format == VC_JPEG_YCBCR444_SEMIPLANAR) {
			amountOfMCUs =
				((imageInfo.output_width * imageInfo.output_height) / 64);
			mcuInRow = (imageInfo.output_width / 8);
		} else if (imageInfo.output_format == VC_JPEG_YCBCR420_SEMIPLANAR) {
			/* 265 is the amount of luma samples in MB for 4:2:0 */
			amountOfMCUs =
				((imageInfo.output_width * imageInfo.output_height) / 256);
			mcuInRow = (imageInfo.output_width / 16);
		} else if (imageInfo.output_format == VC_JPEG_YCBCR422_SEMIPLANAR) {
			/* 128 is the amount of luma samples in MB for 4:2:2 */
			amountOfMCUs =
				((imageInfo.output_width * imageInfo.output_height) / 128);
			mcuInRow = (imageInfo.output_width / 16);
		} else if (imageInfo.output_format == VC_JPEG_YCBCR440) {
			/* 128 is the amount of luma samples in MB for 4:4:0 */
			amountOfMCUs =
				((imageInfo.output_width * imageInfo.output_height) / 128);
			mcuInRow = (imageInfo.output_width / 8);
		} else if (imageInfo.output_format == VC_JPEG_YCBCR411_SEMIPLANAR) {
			amountOfMCUs =
				((imageInfo.output_width * imageInfo.output_height) / 256);
			mcuInRow = (imageInfo.output_width / 32);
		}

		/* set mcuSizeDivider for slice size count */
		if (imageInfo.output_format == VC_JPEG_YCBCR400 ||
		    imageInfo.output_format == VC_JPEG_YCBCR440 ||
		    imageInfo.output_format == VC_JPEG_YCBCR444_SEMIPLANAR)
			mcuSizeDivider = 2;
		else
			mcuSizeDivider = 1;

		/* 8190 and over 16M ==> force to slice mode */
		if (jpegIn.slice_mb_set == 0 &&
		    ((imageInfo.output_width * imageInfo.output_height) >
		     VC_CODEC_JPEG_MAX_PIXEL_AMOUNT)) {
			do {
				jpegIn.slice_mb_set++;
			} while (((jpegIn.slice_mb_set * (mcuInRow / mcuSizeDivider)) +
				  (mcuInRow / mcuSizeDivider)) <
				 VC_CODEC_JPEG_MAX_SLICE_SIZE_8190);
		}
	}

	if (ctx->pp_ctx.enable_pp && vpu->vc8k_cfg.pp_wait_vsync != 0) {
		t0 = jiffies;
		while (jiffies - t0 < vpu->vc8k_cfg.pp_wait_vsync) {
			if (readl_relaxed(vpu->dcultra_base + 0x147C))
				break;
		}
	}

	/* decode */
	do {
		jpegRet = vc_codec_jpeg_decode(jpegInst, &jpegIn, &jpegOut);
		if (jpegRet == VC_JPEG_FRAME_READY) {
			/* check if progressive ==> planar output */
			if ((imageInfo.coding_mode == VC_JPEG_PROGRESSIVE && mode == 0) ||
			    (imageInfo.coding_mode_thumb == VC_JPEG_PROGRESSIVE &&
			     mode == 1))
				progressive = 1;

			if ((imageInfo.coding_mode == VC_JPEG_NONINTERLEAVED && mode == 0) ||
			    (imageInfo.coding_mode_thumb == VC_JPEG_NONINTERLEAVED &&
			     mode == 1))
				nonInterleaved = 1;
			else
				nonInterleaved = 0;

			if (jpegIn.slice_mb_set && fullSliceCounter == -1)
				slicedOutputUsed = 1;

			/* info to handleSlicedOutput */
			frameReady = 1;
			if (!mode)
				nbrOfImagesToOut++;
		} else if (jpegRet == VC_JPEG_SCAN_PROCESSED) {
			/* TODO! Progressive scan ready... */

			/* progressive ==> planar output */
			if (imageInfo.coding_mode == VC_JPEG_PROGRESSIVE)
				progressive = 1;

			/* info to handleSlicedOutput */

			if (imageInfo.coding_mode == VC_JPEG_PROGRESSIVE) {
				/* calculate size for output */
				calcSize(&imageInfo, mode);
				jpeg_write_progressive(&jpegOut, mode);

				scanCounter++;
			}
			/* update/reset */
			progressive = 0;
		} else if (jpegRet == VC_JPEG_STRM_PROCESSED) {
			dev_err(vpu->dev, "%s - VC_JPEG_STRM_PROCESSED not a complete JPEG image!\n",
				__func__);
			return -EINVAL;
		} else if (jpegRet == VC_JPEG_STRM_ERROR) {
			dev_err(vpu->dev, "%s %d - VC_JPEG_STRM_ERROR!\n", __func__, __LINE__);
			if (jpegIn.slice_mb_set && (fullSliceCounter == -1))
				slicedOutputUsed = 1;

			/*
			 * calculate/write output of slice and update output
			 * buffer in case of user allocated memory
			 */
			if (slicedOutputUsed && jpegOut.output_picture_y.virtual_address)
				handleSlicedOutput(ctx, &imageInfo, &jpegIn, &jpegOut);

			/* info to handleSlicedOutput */
			frameReady = 1;
			slicedOutputUsed = 0;

			/* Handle here the error situation */
			PrintJpegRet(vpu, &jpegRet);
			if (mode == 1)
				break;
			return -EIO;
		} else if (jpegRet == VC_JPEG_SLICE_READY) {
			slicedOutputUsed = 1;
			/*
			 * calculate/write output of slice and update output
			 * buffer in case of user allocated memory
			 */
			if (jpegOut.output_picture_y.virtual_address)
				handleSlicedOutput(ctx, &imageInfo, &jpegIn, &jpegOut);
			scanCounter++;
		} else {
			/* Handle here the error situation */
			PrintJpegRet(vpu, &jpegRet);
			dev_info(vpu->dev, "%s %d - unhandled jpegRet code 0x%x!\n", __func__,
				 __LINE__, jpegRet);
			return -EIO;
		}
	} while (jpegRet != VC_JPEG_FRAME_READY);

	/* calculate/write output of slice */
	if (slicedOutputUsed && jpegOut.output_picture_y.virtual_address) {
		handleSlicedOutput(ctx, &imageInfo, &jpegIn, &jpegOut);
		slicedOutputUsed = 0;
	}

	if (jpegOut.output_picture_y.virtual_address) {
		/* calculate size for output */
		calcSize(&imageInfo, mode);

		/* Thumbnail || full resolution */
	}

	_decode_cnt++;
	if (jiffies - _fps_check_jiffy >= 1000) {
		_report_times++;
		_collect_cnt += _decode_cnt;
		if (_report_times == 10) {
			dev_info(vpu->dev, "FPS: %d, Average %d.%d\n", _decode_cnt,
				 _collect_cnt / 10, _collect_cnt % 10);
			_report_times = 0;
			_collect_cnt = 0;
		} else {
			dev_info(vpu->dev, "FPS: %d\n", _decode_cnt);
		}
		_decode_cnt = 0;
		_fps_check_jiffy = jiffies;
	}

	if (ctx->pp_ctx.enable_pp)
		return 0;

	if (imageInfo.coding_mode == VC_JPEG_PROGRESSIVE)
		progressive = 1;

	/* write output */
	if (jpegIn.slice_mb_set) {
		if (imageInfo.output_format != VC_JPEG_YCBCR400)
			dev_err(vpu->dev, "To do: WriteFullOutput!!\n");
	} else {
		if (imageInfo.coding_mode != VC_JPEG_PROGRESSIVE) {
			WriteOutput((u8 *)jpegOut.output_picture_y.virtual_address,
				    sizeLuma,
				    (u8 *)jpegOut.output_picture_cbcr.virtual_address,
				    sizeChroma, mode);
		} else {
			/* calculate size for output */
			calcSize(&imageInfo, mode);

			jpeg_write_progressive(&jpegOut, mode);
		}
	}
	progressive = 0;

	return 0;
}
EXPORT_SYMBOL(ma35d1_jpeg_dec_run);

/*
 * Function name: FindImageInfoEnd
 *
 * Purpose:
 * Finds 0xFFC4 from the stream and pOffset includes number of bytes to
 * this marker. In case of an error returns != 0
 * (i.e., the marker not found).
 */
static u32 FindImageInfoEnd(u8 *pStream, u32 stream_length, u32 *pOffset)
{
	u32 i;

	for (i = 0; i < stream_length; ++i) {
		if (pStream[i] == 0xFF) {
			if (((i + 1) < stream_length) && 0xC4 == pStream[i + 1]) {
				*pOffset = i;
				return 0;
			}
		}
	}
	return -1;
}

/*
 * Function name:  calcSize
 *
 * Purpose:
 * Calculate size
 */
void calcSize(struct vc_jpeg_image_info *imageInfo, u32 picMode)
{
	u32 format;

	sizeLuma = 0;
	sizeChroma = 0;

	format = (picMode == 0) ?
		imageInfo->output_format : imageInfo->output_format_thumb;

	/* if slice interrupt not given to user */
	if (!sliceToUser) {
		if (picMode == 0) {    /* full */
			sizeLuma = (imageInfo->output_width * imageInfo->output_height);
		} else {    /* thumbnail */
			sizeLuma = (imageInfo->output_width_thumb * imageInfo->output_height_thumb);
		}
	} else {
		if (picMode == 0) {   /* full */
			sizeLuma = (imageInfo->output_width * sliceSize);
		} else {    /* thumbnail */
			sizeLuma = (imageInfo->output_width_thumb * sliceSize);
		}
	}

	if (format != VC_JPEG_YCBCR400) {
		if (format == VC_JPEG_YCBCR420_SEMIPLANAR ||
		    format == VC_JPEG_YCBCR411_SEMIPLANAR) {
			sizeChroma = (sizeLuma / 2);
		} else if (format == VC_JPEG_YCBCR444_SEMIPLANAR) {
			sizeChroma = sizeLuma * 2;
		} else {
			sizeChroma = sizeLuma;
		}
	}
}

/*
 * Function name:  WriteOutput
 *
 * Purpose:
 * Write picture pointed by data to file. Size of the
 * picture in pixels is indicated by picSize.
 */
static void WriteOutput(u8 *dataLuma, u32 picSizeLuma, u8 *dataChroma,
			u32 picSizeChroma, u32 picMode)
{
	if (picMode == 1)
		return;
	if (!dataLuma || !dataChroma)
		return;

	memcpy(OutImagePtr_Y, dataLuma, picSizeLuma);
	OutImagePtr_Y += picSizeLuma;
	WriteOutImageBytesCnt += picSizeLuma;

	if (!nonInterleaved) {
		/* progressive ==> planar */
		if (!progressive) {
			int   i;

			for (i = 0; i < picSizeChroma / 2; i++) {
				*OutImagePtr_U++ = dataChroma[i * 2];
				*OutImagePtr_V++ = dataChroma[i * 2 + 1];
			}
			WriteOutImageBytesCnt += picSizeChroma;
		} else {    //  is progressive
			int   i;

			for (i = 0; i < picSizeChroma / 2; i++) {
				*OutImagePtr_U++ = dataChroma[i * 2];
				*OutImagePtr_V++ = dataChroma[i * 2 + 1];
			}
			WriteOutImageBytesCnt += picSizeChroma;
		}  // if (!progressive)

	} else {   // if (!nonInterleaved)
		memcpy(OutImagePtr_U, dataChroma, picSizeChroma / 2);
		memcpy(OutImagePtr_V, dataChroma + picSizeChroma / 2, picSizeChroma / 2);
		WriteOutImageBytesCnt += picSizeChroma;
	}
}

/*
 * Function name: handleSlicedOutput
 *
 * Purpose:
 * Calculates size for slice and writes sliced output
 */
void
handleSlicedOutput(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo,
		   struct vc_jpeg_input *jpegIn, struct vc_jpeg_output *jpegOut)
{
	/* for output name */
	fullSliceCounter++;

	/* save start pointers for whole output */
	if (fullSliceCounter == 0) {
		/* virtual address */
		outputAddressY.virtual_address =
			jpegOut->output_picture_y.virtual_address;
		outputAddressCbCr.virtual_address =
			jpegOut->output_picture_cbcr.virtual_address;

		/* bus address */
		outputAddressY.bus_address = jpegOut->output_picture_y.bus_address;
		outputAddressCbCr.bus_address = jpegOut->output_picture_cbcr.bus_address;
	}

	/* if not PP direct to fbdev, write output to V4L2 buffer */
	if (!ctx->pp_ctx.enable_pp) {
		/******** PHASE 5 ********/

		if (imageInfo->output_format) {
			if (!frameReady) {
				sliceSize = jpegIn->slice_mb_set * 16;
			} else {
				if (mode == 0)
					sliceSize = (imageInfo->output_height -
						((fullSliceCounter) * (sliceSize)));
				else
					sliceSize = (imageInfo->output_height_thumb -
						((fullSliceCounter) * (sliceSize)));
			}
		}

		/* slice interrupt from decoder */
		sliceToUser = 1;

		/* calculate size for output */
		calcSize(imageInfo, mode);

		/* write slice output */
		WriteOutput(((u8 *)jpegOut->output_picture_y.virtual_address),
			    sizeLuma,
			    ((u8 *)jpegOut->output_picture_cbcr.virtual_address),
			    sizeChroma, mode);
	}

	if (frameReady) {
		/* give start pointers for whole output write */

		/* virtual address */
		jpegOut->output_picture_y.virtual_address =
			outputAddressY.virtual_address;
		jpegOut->output_picture_cbcr.virtual_address =
			outputAddressCbCr.virtual_address;

		/* bus address */
		jpegOut->output_picture_y.bus_address = outputAddressY.bus_address;
		jpegOut->output_picture_cbcr.bus_address = outputAddressCbCr.bus_address;
	}

	if (frameReady) {
		frameReady = 0;
		sliceToUser = 0;
		fullSliceCounter = -1;
	}
}

void WriteProgressiveOutput(u32 sizeLuma, u32 sizeChroma, u32 mode,
			    u8 *dataLuma, u8 *dataCb, u8 *dataCr)
{
	memcpy(OutImagePtr_Y, dataLuma, sizeLuma);
	OutImagePtr_Y += sizeLuma;
	memcpy(OutImagePtr_U, dataCb, sizeChroma / 2);
	OutImagePtr_U += sizeChroma / 2;
	memcpy(OutImagePtr_V, dataCr, sizeChroma / 2);
	OutImagePtr_V += sizeChroma / 2;
	WriteOutImageBytesCnt += sizeLuma + sizeChroma;
}

/*
 * Print JPEG api return value
 */
static void PrintJpegRet(struct hantro_dev *vpu, vc_s32 *pJpegRet)
{
	switch (*pJpegRet) {
	case VC_JPEG_FRAME_READY:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_FRAME_READY\n");
		break;
	case VC_JPEG_OK:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_OK\n");
		break;
	case VC_JPEG_ERROR:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_ERROR\n");
		break;
	case VC_JPEG_DWL_HW_TIMEOUT:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_HW_TIMEOUT\n");
		break;
	case VC_JPEG_UNSUPPORTED:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_UNSUPPORTED\n");
		break;
	case VC_JPEG_PARAM_ERROR:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_PARAM_ERROR\n");
		break;
	case VC_JPEG_MEMFAIL:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_MEMFAIL\n");
		break;
	case VC_JPEG_INITFAIL:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_INITFAIL\n");
		break;
	case VC_JPEG_HW_BUS_ERROR:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_HW_BUS_ERROR\n");
		break;
	case VC_JPEG_SYSTEM_ERROR:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_SYSTEM_ERROR\n");
		break;
	case VC_JPEG_DWL_ERROR:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_DWL_ERROR\n");
		break;
	case VC_JPEG_INVALID_STREAM_LENGTH:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_INVALID_STREAM_LENGTH\n");
		break;
	case VC_JPEG_STRM_ERROR:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_STRM_ERROR\n");
		break;
	case VC_JPEG_INVALID_INPUT_BUFFER_SIZE:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_INVALID_INPUT_BUFFER_SIZE\n");
		break;
	case VC_JPEG_INCREASE_INPUT_BUFFER:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_INCREASE_INPUT_BUFFER\n");
		break;
	case VC_JPEG_SLICE_MODE_UNSUPPORTED:
		dev_info(vpu->dev, "TB: jpeg API	returned : VC_JPEG_SLICE_MODE_UNSUPPORTED\n");
		break;
	default:
		dev_info(vpu->dev, "TB: jpeg API	returned unknown status\n");
		break;
	}
}

static int jpeg_pp_init(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	int   ret;

	if (!jpegInst) {
		dev_info(vpu->dev, "%s - JPEG not inited!\n", __func__);
		return -1;
	}

	ret = vc_codec_pp_create(&ppInst);
	if (ret != VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - failed to create PP\n", __func__);
		return -1;
	}

	ret = vc_codec_pp_enable_combined(ppInst, jpegInst, VC_CODEC_PP_TYPE_JPEG);
	if (ret != VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - failed to enable combined mode\n", __func__);
		goto cleanup_pp;
	}

	// get the current default PP config
	memset(&ppConfig, 0, sizeof(ppConfig));
	ret = vc_codec_pp_get_config(ppInst, &ppConfig);
	if (ret != VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - failed to get default PP config\n", __func__);
		goto cleanup_combined;
	}

	if (ctx->pp_ctx.enable_pp) {
		ret = jpeg_pp_out_config(ctx);
		if (ret != 0)
			goto cleanup_combined;
	}
	return 0;

cleanup_combined:
	vc_codec_pp_disable_combined(ppInst, jpegInst);

cleanup_pp:
	vc_codec_pp_release(ppInst);
	return ret;
}

static int jpeg_pp_exit(struct hantro_ctx *ctx)
{
	vc_codec_pp_disable_combined(ppInst, jpegInst);
	vc_codec_pp_release(ppInst);
	return 0;
}

static int jpeg_pp_out_config(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vc8k_pp_params *pp = &ctx->pp_ctx;
	struct vb2_v4l2_buffer *dst_buf;

	dst_buf = hantro_get_dst_buf(ctx);

	/* PP output to frame buffer (ultrafb/overlay) */
	ppConfig.input_rotation = pp->rotation;

	if (pp->pp_out_dst == 0 || pp->pp_out_dst == 1) {
		if (pp->img_out_x != 0 || pp->img_out_y != 0 ||
		    pp->img_out_w != pp->frame_buf_w || pp->img_out_h != pp->frame_buf_h) {
			ppConfig.framebuffer.enable = 1;
			ppConfig.framebuffer.write_origin_x = pp->img_out_x;
			ppConfig.framebuffer.write_origin_y = pp->img_out_y;
			ppConfig.framebuffer.width = pp->frame_buf_w;
			ppConfig.framebuffer.height = pp->frame_buf_h;
		}
	}

	ppConfig.output.width = pp->img_out_w;
	ppConfig.output.height = pp->img_out_h;

	if (pp->img_out_fmt == V4L2_PIX_FMT_NV12) {
		ppConfig.output.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;
	} else if (pp->img_out_fmt == V4L2_PIX_FMT_YUYV) {
		ppConfig.output.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_2_INTERLEAVED;
	} else {
		/*
		 * PP output RGB format
		 */
		ppConfig.rgb.transform = VC_PP_YCBCR2RGB_TRANSFORM_CUSTOM;
		ppConfig.rgb.alpha = 0xFF;
		ppConfig.rgb.coefficients.a = 298;
		ppConfig.rgb.coefficients.b = 409;
		ppConfig.rgb.coefficients.c = 208;
		ppConfig.rgb.coefficients.d = 100;
		ppConfig.rgb.coefficients.e = 516;
		ppConfig.rgb.dithering_enable = 1;

		if (pp->img_out_fmt == V4L2_PIX_FMT_RGB565) {
			ppConfig.output.pix_format = VC_PP_PIX_FMT_RGB16_5_6_5;
		} else {
			/*
			 * should be RGB888, no need to check
			 */
			ppConfig.output.pix_format  = VC_PP_PIX_FMT_RGB32;
		}
	}

	if (pp->pp_out_dst == 0) {
		ppConfig.output.buffer_bus_addr = readl_relaxed(vpu->dcultra_base + 0x1400);
	} else if (pp->pp_out_dst == 1) {
		ppConfig.output.buffer_bus_addr = readl_relaxed(vpu->dcultra_base + 0x15C0);
	} else {
		/* PP output to memory (DMA) buffer */
		if (pp->frame_buf_paddr)
			ppConfig.output.buffer_bus_addr = pp->frame_buf_paddr;
		else
			ppConfig.output.buffer_bus_addr =
				vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	}

	ppConfig.output.buffer_chroma_bus_addr = ppConfig.output.buffer_bus_addr +
			ppConfig.output.width * ppConfig.output.height;

	if (ppConfig.output.buffer_bus_addr < 0x80000000UL ||
	    ppConfig.output.buffer_bus_addr > 0xC0000000UL) {
		dev_info(vpu->dev, "%s - Invalid PP output address! 0x%llx\n",
			 __func__, ppConfig.output.buffer_bus_addr);
		return -1;
	}
	return 0;
}

static int jpeg_pp_in_config(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo)
{
	struct hantro_dev *vpu = ctx->dev;
	int   ret;

	ppConfig.input_crop_enable = 0; /* crop is not supported in current release */

	ppConfig.input.video_range = 1;
	ppConfig.input.width = imageInfo->output_width;
	ppConfig.input.height = imageInfo->output_height;
	ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;

	if (imageInfo->output_format) {
		switch (imageInfo->output_format) {
		case VC_JPEG_YCBCR400:
			ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_0_0;
			break;
		case VC_JPEG_YCBCR420_SEMIPLANAR:
			ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;
			break;
		case VC_JPEG_YCBCR422_SEMIPLANAR:
			ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_2_SEMIPLANAR;
			break;
		case VC_JPEG_YCBCR440:
			ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_4_0;
			break;
		case VC_JPEG_YCBCR411_SEMIPLANAR:
			ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_1_1_SEMIPLANAR;
			break;
		case VC_JPEG_YCBCR444_SEMIPLANAR:
			ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_4_4_SEMIPLANAR;
			break;
		}
	}

	// and finally set the PP config to the post-proc
	ret = vc_codec_pp_set_config(ppInst, &ppConfig);
	if (ret != VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - PPSetConfig failed! %d\n", __func__, ret);
		return -1;
	}
	return 0;
}
