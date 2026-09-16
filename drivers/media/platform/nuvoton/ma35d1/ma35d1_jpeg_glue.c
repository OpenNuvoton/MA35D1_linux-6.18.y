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
static struct vc_jpeg_input	jpegIn;
static struct vc_jpeg_output	jpegOut;

static u32  streamTotalLen;
static u32  streamInFile;
static u32  streamSeekLen;
static u32  imageInfoLength;

static u32  thumbInStream = 0;
static u32  onlyFullResolution = 1;
static u32  mode = 0;    /* 1: THUMBNAIL; 0: FULL RESOLUTION */
static u32  amountOfMCUs = 0;
static u32  mcuInRow = 0;
static u32  mcuSizeDivider = 0;
static u32  progressive	= 0;
static u32  nonInterleaved = 0;
static u32  ThumbDone =	0;
static u32  slicedOutputUsed = 0;
static int  fullSliceCounter;
static u32  frameReady = 0;
static u32  sizeLuma = 0;
static u32  sizeChroma = 0;
static u32  sliceToUser	= 0;
static u32  sliceSize = 0;
static u32  nbrOfImagesToOut = 0;
static u32  scanCounter	= 0;
//static u32  planarOutput = 0;
static struct vc_jpeg_linear_mem	outputAddressY;
static struct vc_jpeg_linear_mem	outputAddressCbCr;
static u8   *OutImagePtr_Y, *OutImagePtr_U, *OutImagePtr_V;
static int  WriteOutImageBytesCnt;
static bool is_first;

/*
 *  For monitoring frame rate
 */
static uint64_t		_fps_check_jiffy;
static int		_decode_cnt, _collect_cnt, _report_times;

static vc_codec_handle_t	    ppInst = NULL;
static struct vc_pp_config	    ppConfig;

static int jpeg_pp_init(struct hantro_ctx *ctx);
static int jpeg_pp_exit(struct hantro_ctx *ctx);
static int jpeg_pp_in_config(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo);
static int jpeg_pp_out_config(struct hantro_ctx	*ctx);
static void WriteOutput(u8 *dataLuma, u32 picSizeLuma, u8 *dataChroma,
			u32 picSizeChroma, u32 picMode);
//static void WriteFullOutput(u32 picMode);
static void WriteProgressiveOutput(u32 sizeLuma, u32 sizeChroma, u32 mode,
					u8 *dataLuma, u8 *dataCb, u8 *dataCr);
static u32 FindImageInfoEnd(u8 * pStream, u32 stream_length, u32	* pOffset);
static void calcSize(struct vc_jpeg_image_info * imageInfo, u32 picMode);
static void handleSlicedOutput(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo,
				struct vc_jpeg_input *jpegIn, struct vc_jpeg_output *jpegOut);
static void PrintJpegRet(struct hantro_dev *vpu, vc_s32 * pJpegRet);
//static void PrintGetImageInfo(struct hantro_dev *vpu, struct vc_jpeg_image_info * imageInfo);

int ma35d1_jpeg_dec_init(struct	hantro_ctx *ctx)
{
	// struct hantro_dev *vpu = ctx->dev;
	vc_s32  jpegRet;

	// dev_info(vpu->dev, "ma35d1_jpeg_dec_init called.\n");

	/* Initialize variables	*/
	thumbInStream =	0;
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
	is_first = 1;

	memset(&jpegInst, 0, sizeof(jpegInst));
	memset(&jpegIn,	0, sizeof(jpegIn));
	memset(&jpegOut, 0, sizeof(jpegOut));

	jpegRet	= vc_codec_jpeg_create(&jpegInst);
	if(jpegRet != VC_JPEG_OK)
	{
		/* Handle here the error situation */
		// PrintJpegRet(vpu, &jpegRet);
		return -1;
	}

	vc_codec_jpeg_apply_hw_defaults(jpegInst);

	// dev_info(ctx->dev->dev, "PHASE 1: INIT JPEG DECODER successful\n");

	_fps_check_jiffy = jiffies;
	_report_times = 0;
	_collect_cnt = 0;
	_decode_cnt = 0;

	return 0;
}
EXPORT_SYMBOL(ma35d1_jpeg_dec_init);

void ma35d1_jpeg_dec_exit(struct hantro_ctx *ctx)
{
	if (ctx->pp_ctx.enable_pp == true)
		jpeg_pp_exit(ctx);
	vc_codec_jpeg_destroy(jpegInst);
}
EXPORT_SYMBOL(ma35d1_jpeg_dec_exit);

int ma35d1_jpeg_dec_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	g1_addr_t src_dma;
	u8	*byteStrmStart;
	struct vc_jpeg_image_info imageInfo;
	u32	data_len, len;
	int	jpegRet;
	u64	t0;

	// dev_info(vpu->dev, "ma35d1_jpeg_dec_run called.\n");

	src_buf	= hantro_get_src_buf(ctx);
	dst_buf	= hantro_get_dst_buf(ctx);

	src_dma	= vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	data_len = len = vb2_get_plane_payload(&src_buf->vb2_buf, 0);

	if (!is_first && len >= 0x200000) {
		// dev_info(vpu->dev, "Input len = %d, should be EOS.\n", len);
		// dev_info(vpu->dev, "src last flag is %d\n", (src_buf->flags & V4L2_BUF_FLAG_LAST) ? 1 : 0);
		src_buf->flags |= V4L2_BUF_FLAG_LAST;
		dst_buf->flags |= V4L2_BUF_FLAG_LAST;
		return 0;
	}
	is_first = 0;

	if (vpu->vc8k_cfg.use_dev_coherent) {
		byteStrmStart =	vpu->vc8k_cfg.res_mem_virt + (src_dma -
				vpu->vc8k_cfg.res_mem_base);
	} else {
		byteStrmStart =	phys_to_virt(src_dma);
		dma_sync_single_for_cpu(vpu->dev, src_dma, data_len, DMA_FROM_DEVICE);
	}

	/*-----------------------------------------------------------------*/
	/*  PHASE 2: OPEN/READ FILE					   */
	/*-----------------------------------------------------------------*/

	jpegIn.buffer_size = 0;
	streamTotalLen = len;
	streamInFile = streamTotalLen;
	streamSeekLen =	0;

	/* initialize vc_codec_jpeg_decode input structure */
	jpegIn.stream_buffer.bus_address = src_dma;
	jpegIn.stream_buffer.virtual_address = (u32 *)byteStrmStart;
	jpegIn.stream_length = len;

	// dev_info(vpu->dev, "%s -	jpeg len = %d\n", __func__, len);

	/*-----------------------------------------------------------------*/
	/*  PHASE 3: GET IMAGE INFO					   */
	/*-----------------------------------------------------------------*/
	jpegRet	= FindImageInfoEnd(byteStrmStart, len, &imageInfoLength);
	if (jpegRet != 0) {
		dev_err(vpu->dev, "%s -	FindImageInfoEnd failed!\n", __func__);
		return -1;
	}

	/* Get image information of the	JFIF and decode	JFIF header */
	jpegRet	= vc_codec_jpeg_get_image_info(jpegInst,	&jpegIn, &imageInfo);
	if (jpegRet != VC_JPEG_OK) {
		dev_err(vpu->dev, "%s -	vc_codec_jpeg_get_image_info failed!\n",	__func__);
		return -1;
	}

//	if (!ctx->is_pp_inited)
//		PrintGetImageInfo(vpu, &imageInfo);

	if (ctx->pp_changed == -1) {
		/* PP of this context was not configured. Use global setting. */ 
		memcpy(&ctx->pp_ctx, &(vpu->vc8k_cfg.ppc), sizeof(struct vc8k_pp_params));
	}

	if (!ctx->is_pp_inited && (ctx->pp_ctx.enable_pp == true)) {
		if (jpeg_pp_init(ctx) != 0)
			return -1;
		ctx->is_pp_inited = 1;
		jpeg_pp_in_config(ctx, &imageInfo);
        }

	if(ctx->pp_ctx.enable_pp == false) {
		if (dst_buf->vb2_buf.num_planes < 3) {
			dev_err(vpu->dev, "%s - output num_planes=%d !\n", __func__, dst_buf->vb2_buf.num_planes);
			return -EINVAL;
		}
		// dev_info(vpu->dev, "%s - Y,U,V plane size: %d, %d, %d\n", __func__, ctx->dst_fmt.plane_fmt[0].sizeimage, ctx->dst_fmt.plane_fmt[1].sizeimage, ctx->dst_fmt.plane_fmt[2].sizeimage);
		OutImagePtr_Y = (u8 *)phys_to_virt(vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0));
		OutImagePtr_U = (u8 *)phys_to_virt(vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 1));
		OutImagePtr_V = (u8 *)phys_to_virt(vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 2));
		WriteOutImageBytesCnt = 0;
	}

	/*  ******************** THUMBNAIL ****************************	*/
	/* Select if Thumbnail or full resolution image	will be	decoded	*/
	if (imageInfo.thumbnail_type == VC_JPEG_THUMBNAIL_JPEG) {
		/* if all thumbnails processed (MJPEG) */
		if (!ThumbDone)
			jpegIn.image_type = VC_JPEG_THUMBNAIL;
		else
			jpegIn.image_type = VC_JPEG_IMAGE;

		thumbInStream =	1;
	}
	else if(imageInfo.thumbnail_type	== VC_JPEG_NO_THUMBNAIL)
		jpegIn.image_type = VC_JPEG_IMAGE;
	else if	(imageInfo.thumbnail_type == VC_JPEG_THUMBNAIL_NOT_SUPPORTED_FORMAT)
		jpegIn.image_type = VC_JPEG_IMAGE;

	/* check if forced to decode only full resolution images  ==> discard thumbnail	*/
	if (onlyFullResolution)	{
		/* decode only full resolution image */
		// dev_info(vpu->dev, "\n\tNOTE! FORCED BY USER TO DECODE ONLY FULL	RESOLUTION IMAGE\n");
		jpegIn.image_type = VC_JPEG_IMAGE;
	}

	// dev_info(vpu->dev, "PHASE 3: GET	IMAGE INFO successful\n");

	/*-----------------------------------------------------------------*/
	/*  Decode FRAME						   */
	/*-----------------------------------------------------------------*/
	/* TB SPECIFIC == LOOP IF THUMBNAIL IN JFIF */
	/* Decode JFIF */
	if (jpegIn.image_type	== VC_JPEG_THUMBNAIL)
		mode = 1; /* TODO KIMA */
	else
		mode = 0;

	/* no slice mode supported in progressive || non-interleaved ==> force to full mode */
	if ((jpegIn.image_type == VC_JPEG_THUMBNAIL &&
		imageInfo.coding_mode_thumb == VC_JPEG_PROGRESSIVE) ||
		(jpegIn.image_type ==	VC_JPEG_IMAGE &&
		 imageInfo.coding_mode == VC_JPEG_PROGRESSIVE))
		jpegIn.slice_mb_set = 0;

	/******** PHASE	4 ********/
	/* Image mode to decode	*/
	//if (mode)
	//	dev_info(vpu->dev, "\nPHASE 4: DECODE FRAME: THUMBNAIL\n");
	//else
	//	dev_info(vpu->dev, "\nPHASE 4: DECODE FRAME: FULL RESOLUTION\n");

	/* if input (only full,	not tn)	> 4096 MCU	*/
	/* ==> force to	slice mode					*/
	if (mode == 0) {
		// dev_info(vpu->dev, " output_format = 0x%x, output_width=%d, output_height=%d\n",imageInfo.output_format, imageInfo.output_width, imageInfo.output_height);

		/* calculate MCU's */
		if (imageInfo.output_format == VC_JPEG_YCBCR400 ||
			imageInfo.output_format == VC_JPEG_YCBCR444_SEMIPLANAR) {
			amountOfMCUs =
				((imageInfo.output_width	* imageInfo.output_height) / 64);
			mcuInRow = (imageInfo.output_width / 8);
		}
		else if	(imageInfo.output_format	== VC_JPEG_YCBCR420_SEMIPLANAR)
		{
			/* 265 is the amount of	luma samples in	MB for 4:2:0 */
			amountOfMCUs =
				((imageInfo.output_width	* imageInfo.output_height) / 256);
			mcuInRow = (imageInfo.output_width / 16);
		}
		else if	(imageInfo.output_format	== VC_JPEG_YCBCR422_SEMIPLANAR)
		{
			/* 128 is the amount of	luma samples in	MB for 4:2:2 */
			amountOfMCUs =
				((imageInfo.output_width	* imageInfo.output_height) / 128);
			mcuInRow = (imageInfo.output_width / 16);
		}
		else if(imageInfo.output_format == VC_JPEG_YCBCR440)
		{
			/* 128 is the amount of	luma samples in	MB for 4:4:0 */
			amountOfMCUs =
				((imageInfo.output_width	* imageInfo.output_height) / 128);
			mcuInRow = (imageInfo.output_width / 8);
		}
		else if(imageInfo.output_format == VC_JPEG_YCBCR411_SEMIPLANAR)
		{
			amountOfMCUs =
				((imageInfo.output_width	* imageInfo.output_height) / 256);
			mcuInRow = (imageInfo.output_width / 32);
		}

		/* set mcuSizeDivider for slice	size count */
		if (imageInfo.output_format == VC_JPEG_YCBCR400 ||
		   imageInfo.output_format == VC_JPEG_YCBCR440 ||
		   imageInfo.output_format == VC_JPEG_YCBCR444_SEMIPLANAR)
			mcuSizeDivider = 2;
		else
			mcuSizeDivider = 1;

#if 0
		/* over	max MCU	==> force to slice mode	*/
		if ((jpegIn.slice_mb_set == 0) &&
		   (amountOfMCUs > VC_JPEG_MAX_SLICE_SIZE)) {
			do {
				jpegIn.slice_mb_set++;
			}
			while(((jpegIn.slice_mb_set * (mcuInRow /	mcuSizeDivider)) +
				   (mcuInRow / mcuSizeDivider))	<
				  VC_JPEG_MAX_SLICE_SIZE);
			// dev_info(vpu->dev, "Force to slice mode ==> Decoder Slice MB Set	%d\n", jpegIn.slice_mb_set);
		}
#else
		/* 8190	and over 16M ==> force to slice	mode */
		if ((jpegIn.slice_mb_set == 0) &&
		   ((imageInfo.output_width * imageInfo.output_height) >
			VC_CODEC_JPEG_MAX_PIXEL_AMOUNT))
		{
			do {
				jpegIn.slice_mb_set++;
			}
			while(((jpegIn.slice_mb_set * (mcuInRow /	mcuSizeDivider)) +
				   (mcuInRow / mcuSizeDivider))	<
					VC_CODEC_JPEG_MAX_SLICE_SIZE_8190);
			// dev_info(vpu->dev, "Force to slice mode (over 16M) ==> Decoder Slice MB Set %d\n", jpegIn.slice_mb_set);
		}
#endif
	}

	if (jpegIn.slice_mb_set) {
		// dev_info(vpu->dev, "%s - jpegIn.slice_mb_set = %d\n", __func__, jpegIn.slice_mb_set);
	}

	if ((ctx->pp_ctx.enable_pp == true) && (vpu->vc8k_cfg.pp_wait_vsync != 0)) {
		t0 = jiffies;
		while (jiffies - t0 < vpu->vc8k_cfg.pp_wait_vsync) {
			if (readl_relaxed(vpu->dcultra_base + 0x147C)) {
				// printk("%d", jiffies - t0);
				break;
			}
		}
	}

	/* decode */
	do {
	jpegRet	= vc_codec_jpeg_decode(jpegInst, &jpegIn, &jpegOut);
	if (jpegRet == VC_JPEG_FRAME_READY) {
		// dev_info(vpu->dev, "\t-JPEG: VC_JPEG_FRAME_READY\n");

		/* check if progressive	==> planar output */
		if (((imageInfo.coding_mode == VC_JPEG_PROGRESSIVE) && (mode == 0)) ||
			((imageInfo.coding_mode_thumb == VC_JPEG_PROGRESSIVE) &&
			(mode == 1)))  {
			progressive = 1;
		}

		if (((imageInfo.coding_mode == VC_JPEG_NONINTERLEAVED) && (mode == 0))
			|| ((imageInfo.coding_mode_thumb == VC_JPEG_NONINTERLEAVED) &&
			(mode == 1)))
			nonInterleaved = 1;
		else
			nonInterleaved = 0;

		if (jpegIn.slice_mb_set && fullSliceCounter == -1)
			slicedOutputUsed = 1;

		/* info	to handleSlicedOutput */
		frameReady = 1;
		if (!mode)
			nbrOfImagesToOut++;
	} else if (jpegRet == VC_JPEG_SCAN_PROCESSED) {
		/* TODO! Progressive scan ready... */
		// dev_info(vpu->dev, "\t-JPEG:	VC_JPEG_SCAN_PROCESSED\n");

		/* progressive ==> planar output */
		if (imageInfo.coding_mode == VC_JPEG_PROGRESSIVE)
			progressive = 1;

		/* info to handleSlicedOutput */
		// dev_info(vpu->dev, "SCAN %d READY\n", scanCounter);

		if (imageInfo.coding_mode == VC_JPEG_PROGRESSIVE) {
			/* calculate size for output */
			calcSize(&imageInfo, mode);
			// dev_info(vpu->dev, "sizeLuma %d and sizeChroma %d\n", sizeLuma, sizeChroma);
			WriteProgressiveOutput(sizeLuma, sizeChroma, mode,
					(u8 *)jpegOut.output_picture_y.
					virtual_address,
					(u8 *)jpegOut.output_picture_cbcr.
					virtual_address,
					(u8 *)jpegOut.output_picture_cr.
					virtual_address);

			scanCounter++;
		}
		/* update/reset */
		progressive = 0;

	} else if (jpegRet == VC_JPEG_SLICE_READY) {
		// dev_info(vpu->dev, "\t-JPEG: VC_JPEG_SLICE_READY\n");
		slicedOutputUsed = 1;
		/* calculate/write output of slice
		 * and update output budder in case of
		 * user	allocated memory */
		if (jpegOut.output_picture_y.virtual_address != NULL)
			handleSlicedOutput(ctx, &imageInfo, &jpegIn, &jpegOut);
		scanCounter++;

	} else if (jpegRet == VC_JPEG_STRM_PROCESSED) {
		// dev_info(vpu->dev, "\t-JPEG: VC_JPEG_STRM_PROCESSED ==> Load input buffer\n");
		dev_err(vpu->dev, "%s -	VC_JPEG_STRM_PROCESSED not a complete JPEG image!\n",	__func__);
		return -EINVAL;
	} else if (jpegRet == VC_JPEG_STRM_ERROR) {
		dev_err(vpu->dev, "%s %d - VC_JPEG_STRM_ERROR!\n", __func__, __LINE__);
//strm_error:
		if (jpegIn.slice_mb_set && (fullSliceCounter == -1))
			slicedOutputUsed = 1;

		/* calculate/write output of slice and update output budder in case of user allocated memory */
		if (slicedOutputUsed && (jpegOut.output_picture_y.virtual_address != NULL))
			handleSlicedOutput(ctx, &imageInfo, &jpegIn, &jpegOut);

		/* info to handleSlicedOutput */
		frameReady = 1;
		slicedOutputUsed = 0;

		/* Handle here the error situation */
		PrintJpegRet(vpu, &jpegRet);
		if (mode == 1)
			break;
		else
			return -EIO;  // goto error;
	} else {
		/* Handle here the error situation */
		PrintJpegRet(vpu, &jpegRet);
		dev_info(vpu->dev, "%s %d - unhandled jpegRet code 0x%x!\n", __func__, __LINE__,	jpegRet);
		return -EIO;
	}
	} while(jpegRet	!= VC_JPEG_FRAME_READY);

	/* calculate/write output of slice */
	if (slicedOutputUsed &&	jpegOut.output_picture_y.virtual_address != NULL)
	{
		handleSlicedOutput(ctx, &imageInfo, &jpegIn, &jpegOut);
		slicedOutputUsed = 0;
	}

	if (jpegOut.output_picture_y.virtual_address != NULL)
	{
		/* calculate size for output */
		calcSize(&imageInfo, mode);

		/* Thumbnail ||	full resolution	*/
		//if (!mode)
		//	dev_info(vpu->dev, "\n\t-JPEG: ++++++++++ FULL RESOLUTION ++++++++++\n");
		//else
		//	dev_info(vpu->dev, "\t-JPEG: ++++++++++ THUMBNAIL ++++++++++\n");
		//dev_info(vpu->dev, "\t-JPEG decoder instance active\n");
		//dev_info(vpu->dev, "\t-JPEG: Luma size: %d\n", sizeLuma);
		//dev_info(vpu->dev, "\t-JPEG: Chroma size: %d\n", sizeChroma);
		//dev_info(vpu->dev, "\t-JPEG: Luma output	bus: 0x%x\n", (int)jpegOut.output_picture_y.bus_address);
		//dev_info(vpu->dev, "\t-JPEG: Chroma output bus: 0x%x\n", (int)jpegOut.output_picture_cbcr.bus_address);
	}

	// dev_info(vpu->dev, "PHASE 4: DECODE FRAME successful\n");

	_decode_cnt++;
	if (jiffies - _fps_check_jiffy >= 1000) {
        	_report_times++;
		_collect_cnt += _decode_cnt;
		if (_report_times == 10) {
			dev_info(vpu->dev, "FPS: %d, Average %d.%d\n", _decode_cnt, _collect_cnt/10, _collect_cnt%10);
			_report_times = 0;
			_collect_cnt = 0;
		} else {
			dev_info(vpu->dev, "FPS: %d\n", _decode_cnt);
		}
		_decode_cnt = 0;
		_fps_check_jiffy = jiffies;
	}

	if (ctx->pp_ctx.enable_pp == true)
		return 0;

        /******** PHASE 5 ********/
        // dev_info(vpu->dev, "\nPHASE 5: WRITE OUTPUT\n");

	if (imageInfo.output_format) {
		switch (imageInfo.output_format) {
		case VC_JPEG_YCBCR400:
			// dev_info(vpu->dev, "\t-JPEG: DECODER OUTPUT: VC_JPEG_YCBCR400\n");
			break;
		case VC_JPEG_YCBCR420_SEMIPLANAR:
			// dev_info(vpu->dev, "\t-JPEG: DECODER OUTPUT: VC_JPEG_YCBCR420_SEMIPLANAR\n");
			break;
		case VC_JPEG_YCBCR422_SEMIPLANAR:
			// dev_info(vpu->dev, "\t-JPEG: DECODER OUTPUT: VC_JPEG_YCBCR422_SEMIPLANAR\n");
			break;
		case VC_JPEG_YCBCR440:
			// dev_info(vpu->dev, "\t-JPEG: DECODER OUTPUT: VC_JPEG_YCBCR440\n");
			break;
		case VC_JPEG_YCBCR411_SEMIPLANAR:
			// dev_info(vpu->dev, "\t-JPEG: DECODER OUTPUT: VC_JPEG_YCBCR411_SEMIPLANAR\n");
			break;
		case VC_JPEG_YCBCR444_SEMIPLANAR:
			// dev_info(vpu->dev, "\t-JPEG: DECODER OUTPUT: VC_JPEG_YCBCR444_SEMIPLANAR\n");
			break;
		}
	}
	if (imageInfo.coding_mode == VC_JPEG_PROGRESSIVE)
	    progressive = 1;

	/* write output */
	if (jpegIn.slice_mb_set) {
		if (imageInfo.output_format != VC_JPEG_YCBCR400)
			dev_err(vpu->dev, "To do: WriteFullOutput!!\n");  // WriteFullOutput(mode);
	} else {
		if (imageInfo.coding_mode != VC_JPEG_PROGRESSIVE) {
		    WriteOutput(((u8 *) jpegOut.output_picture_y.virtual_address),
				sizeLuma,
				((u8 *) jpegOut.output_picture_cbcr.
				virtual_address), sizeChroma, mode);
		}
		else
		{
		    /* calculate size for output */
		    calcSize(&imageInfo, mode);

		    // dev_info(vpu->dev, "sizeLuma %d and sizeChroma %d\n", sizeLuma, sizeChroma);

		    WriteProgressiveOutput(sizeLuma, sizeChroma, mode,
					   (u8*)jpegOut.output_picture_y.virtual_address,
					   (u8*)jpegOut.output_picture_cbcr.
					   virtual_address,
					   (u8*)jpegOut.output_picture_cr.virtual_address);
		}

	}
#if 0
	if (crop)
	    WriteCroppedOutput(&imageInfo,
	                       (u8*)jpegOut.output_picture_y.virtual_address,
	                       (u8*)jpegOut.output_picture_cbcr.virtual_address,
	                       (u8*)jpegOut.output_picture_cr.virtual_address);
#endif
	progressive = 0;

	// dev_info(vpu->dev, "PHASE 5: WRITE OUTPUT successful. bytes %d written\n", WriteOutImageBytesCnt);
	return 0;
}
EXPORT_SYMBOL(ma35d1_jpeg_dec_run);

/*-----------------------------------------------------------------------------

	Function name:	FindImageInfoEnd

	Purpose:
		Finds 0xFFC4 from the stream and pOffset includes number of bytes to
		this marker. In	case of	an error returns != 0
		(i.e., the marker not found).

-----------------------------------------------------------------------------*/
static u32 FindImageInfoEnd(u8 * pStream, u32 stream_length, u32	* pOffset)
{
	u32 i;

	for (i = 0; i <	stream_length; ++i) {
		if (0xFF == pStream[i])	{
			if (((i	+ 1) < stream_length) &&	0xC4 ==	pStream[i + 1])	{
				*pOffset = i;
				return 0;
			}
		}
	}
	return -1;
}

/*------------------------------------------------------------------------------

    Function name:  calcSize

    Purpose:
        Calculate size

------------------------------------------------------------------------------*/
void calcSize(struct vc_jpeg_image_info * imageInfo, u32 picMode)
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
		if ((format == VC_JPEG_YCBCR420_SEMIPLANAR) ||
		    (format == VC_JPEG_YCBCR411_SEMIPLANAR)) {
			sizeChroma = (sizeLuma / 2);
		} else if (format == VC_JPEG_YCBCR444_SEMIPLANAR) {
			sizeChroma = sizeLuma * 2;
		} else {
			sizeChroma = sizeLuma;
		}
	}
}

/*------------------------------------------------------------------------------

Function name:  WriteOutput

Purpose:
    Write picture pointed by data to file. Size of the
    picture in pixels is indicated by picSize.

------------------------------------------------------------------------------*/
static void WriteOutput(u8 *dataLuma, u32 picSizeLuma, u8 *dataChroma,
			u32 picSizeChroma, u32 picMode)
{
	if (picMode == 1) {
		return;
	}
	if (!dataLuma || !dataChroma) {
		return;
	}

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
		//for (i = 0; i < picSizeChroma; i++)
		//    f_write(&foutput, pYuvOut + (1 * i), 1, &ff_rw);
		memcpy(OutImagePtr_U, dataChroma, picSizeChroma / 2);
		memcpy(OutImagePtr_V, dataChroma + picSizeChroma / 2, picSizeChroma / 2);
		WriteOutImageBytesCnt += picSizeChroma;
	}
}


/*------------------------------------------------------------------------------

	Function name:	handleSlicedOutput

	Purpose:
		Calculates size	for slice and writes sliced output

------------------------------------------------------------------------------*/
void
handleSlicedOutput(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo,
		   struct vc_jpeg_input *jpegIn, struct vc_jpeg_output *jpegOut)
{
	// struct hantro_dev *vpu = ctx->dev;

	/* for output name */
	fullSliceCounter++;

	/******** PHASE	X ********/
	if (jpegIn->slice_mb_set) {
		// dev_info(ctx->dev->dev, "\nPHASE SLICE: HANDLE SLICE %d\n", fullSliceCounter);
	}

	/* save	start pointers for whole output	*/
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
	if (ctx->pp_ctx.enable_pp == false) {
		/******** PHASE 5 ********/
		// dev_info(ctx->dev->dev, "\nPHASE 5: WRITE OUTPUT\n");

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

		//dev_info(vpu->dev, "\t-JPEG: ++++++++++ SLICE INFORMATION ++++++++++\n");
		//dev_info(vpu->dev, "\t-JPEG: Luma output: 0x%llx size: %d\n",
		//        jpegOut->output_picture_y.virtual_address, sizeLuma);
		//dev_info(vpu->dev, "\t-JPEG: Chroma output: 0x%llx size: %d\n",
		//        jpegOut->output_picture_cbcr.virtual_address, sizeChroma);
		//dev_info(vpu->dev, "\t-JPEG: Luma output bus: 0x%llx\n",
		//        (u8 *) jpegOut->output_picture_y.bus_address);
		//dev_info(vpu->dev, "\t-JPEG: Chroma output bus: 0x%llx\n",
		//        (u8 *) jpegOut->output_picture_cbcr.bus_address);

		/* write slice output */
		WriteOutput(((u8 *) jpegOut->output_picture_y.virtual_address),
		            sizeLuma,
		            ((u8 *) jpegOut->output_picture_cbcr.virtual_address),
		            sizeChroma, mode);
		// dev_info(vpu->dev, "PHASE 5: WRITE OUTPUT successful\n");
	}

	if (frameReady) {
		/* give	start pointers for whole output	write */

		/* virtual address */
		jpegOut->output_picture_y.virtual_address	=
			outputAddressY.virtual_address;
		jpegOut->output_picture_cbcr.virtual_address =
			outputAddressCbCr.virtual_address;

		/* bus address */
		jpegOut->output_picture_y.bus_address = outputAddressY.bus_address;
		jpegOut->output_picture_cbcr.bus_address =	outputAddressCbCr.bus_address;
	}

	if (frameReady) {
		frameReady = 0;
		sliceToUser = 0;
		/******** PHASE	X ********/
		if (jpegIn->slice_mb_set) {
			// dev_info(vpu->dev, "\nPHASE SLICE: HANDLE SLICE %d successful\n", fullSliceCounter);
		}

		fullSliceCounter = -1;
	} else {
		/******** PHASE	X ********/
		if (jpegIn->slice_mb_set) {
			// dev_info(vpu->dev, "\nPHASE SLICE: HANDLE SLICE %d successful\n", fullSliceCounter);
		}
	}

}

void WriteProgressiveOutput(u32	sizeLuma, u32 sizeChroma, u32 mode,
			    u8 * dataLuma, u8 *	dataCb,	u8 * dataCr)
{
	memcpy(OutImagePtr_Y, dataLuma, sizeLuma);
	OutImagePtr_Y += sizeLuma;
	memcpy(OutImagePtr_U,	dataCb,	sizeChroma / 2);
	OutImagePtr_U	+= sizeChroma /	2;
	memcpy(OutImagePtr_V,	dataCr,	sizeChroma / 2);
	OutImagePtr_V	+= sizeChroma /	2;
	WriteOutImageBytesCnt += sizeLuma + sizeChroma;
}

/*-----------------------------------------------------------------------------

Print JPEG api return value

-----------------------------------------------------------------------------*/
static void PrintJpegRet(struct hantro_dev *vpu, vc_s32 * pJpegRet)
{
	switch (*pJpegRet)
	{
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

/*-----------------------------------------------------------------------------

Print vc_codec_jpeg_get_image_info values

-----------------------------------------------------------------------------*/
#if 0
static void PrintGetImageInfo(struct hantro_dev *vpu, struct vc_jpeg_image_info * imageInfo)
{
	/* Select if Thumbnail or full resolution image	will be	decoded	*/
	if(imageInfo->thumbnail_type == VC_JPEG_THUMBNAIL_JPEG)
	{
		/* decode thumbnail */
		dev_info(vpu->dev, "\t-JPEG THUMBNAIL IN	STREAM\n");
		dev_info(vpu->dev, "\t-JPEG THUMBNAIL INFO\n");
		dev_info(vpu->dev, "\t\t-JPEG thumbnail width: %d\n",
				imageInfo->output_width_thumb);
		dev_info(vpu->dev, "\t\t-JPEG thumbnail height: %d\n",
				imageInfo->output_height_thumb);

		/* stream type */
		switch (imageInfo->coding_mode_thumb)
		{
		case VC_JPEG_BASELINE:
			dev_info(vpu->dev, "\t\t-JPEG: STREAM TYPE: VC_JPEG_BASELINE\n");
			break;
		case VC_JPEG_PROGRESSIVE:
			dev_info(vpu->dev, "\t\t-JPEG: STREAM TYPE: VC_JPEG_PROGRESSIVE\n");
			break;
		case VC_JPEG_NONINTERLEAVED:
			dev_info(vpu->dev, "\t\t-JPEG: STREAM TYPE: VC_JPEG_NONINTERLEAVED\n");
			break;
		}

		if(imageInfo->output_format_thumb)
		{
			switch (imageInfo->output_format_thumb)
			{
			case VC_JPEG_YCBCR400:
				dev_info(vpu->dev, "\t\t-JPEG: THUMBNAIL OUTPUT: VC_JPEG_YCBCR400\n");
				break;
			case VC_JPEG_YCBCR420_SEMIPLANAR:
				dev_info(vpu->dev, "\t\t-JPEG: THUMBNAIL OUTPUT: VC_JPEG_YCBCR420_SEMIPLANAR\n");
				break;
			case VC_JPEG_YCBCR422_SEMIPLANAR:
				dev_info(vpu->dev, "\t\t-JPEG: THUMBNAIL OUTPUT: VC_JPEG_YCBCR422_SEMIPLANAR\n");
				break;
			case VC_JPEG_YCBCR440:
				dev_info(vpu->dev, "\t\t-JPEG: THUMBNAIL OUTPUT: VC_JPEG_YCBCR440\n");
				break;
			case VC_JPEG_YCBCR411_SEMIPLANAR:
				dev_info(vpu->dev, "\t\t-JPEG: THUMBNAIL OUTPUT: VC_JPEG_YCBCR411_SEMIPLANAR\n");
				break;
			case VC_JPEG_YCBCR444_SEMIPLANAR:
				dev_info(vpu->dev, "\t\t-JPEG: THUMBNAIL OUTPUT: VC_JPEG_YCBCR444_SEMIPLANAR\n");
				break;
			}
		}
	}
	else if(imageInfo->thumbnail_type == VC_JPEG_NO_THUMBNAIL)
	{
		/* decode full image */
		dev_info(vpu->dev, "\t-NO THUMBNAIL IN STREAM ==> Decode	full resolution	image\n");
	}
	else if(imageInfo->thumbnail_type == VC_JPEG_THUMBNAIL_NOT_SUPPORTED_FORMAT)
	{
		/* decode full image */
		dev_info(vpu->dev, "\tNOT SUPPORTED THUMBNAIL IN	STREAM ==> Decode full resolution image\n");
	}

	dev_info(vpu->dev, "\t-JPEG FULL	RESOLUTION INFO\n");
	dev_info(vpu->dev, "\t\t-JPEG width: %d\n", imageInfo->output_width);
	dev_info(vpu->dev, "\t\t-JPEG height: %d\n", imageInfo->output_height);
	if(imageInfo->output_format)
	{
		switch (imageInfo->output_format)
		{
		case VC_JPEG_YCBCR400:
			dev_info(vpu->dev, "\t\t-JPEG: FULL RESOLUTION OUTPUT: VC_JPEG_YCBCR400\n");
			break;
		case VC_JPEG_YCBCR420_SEMIPLANAR:
			dev_info(vpu->dev, "\t\t-JPEG: FULL RESOLUTION OUTPUT: VC_JPEG_YCBCR420_SEMIPLANAR\n");
			break;
		case VC_JPEG_YCBCR422_SEMIPLANAR:
			dev_info(vpu->dev, "\t\t-JPEG: FULL RESOLUTION OUTPUT: VC_JPEG_YCBCR422_SEMIPLANAR\n");
			break;
		case VC_JPEG_YCBCR440:
			dev_info(vpu->dev, "\t\t-JPEG: FULL RESOLUTION OUTPUT: VC_JPEG_YCBCR440\n");
			break;
		case VC_JPEG_YCBCR411_SEMIPLANAR:
			dev_info(vpu->dev, "\t\t-JPEG: FULL RESOLUTION OUTPUT: VC_JPEG_YCBCR411_SEMIPLANAR\n");
			break;
		case VC_JPEG_YCBCR444_SEMIPLANAR:
			dev_info(vpu->dev, "\t\t-JPEG: FULL RESOLUTION OUTPUT: VC_JPEG_YCBCR444_SEMIPLANAR\n");
			break;
		}
	}

	/* stream type */
	switch (imageInfo->coding_mode)
	{
	case VC_JPEG_BASELINE:
		dev_info(vpu->dev, "\t\t-JPEG: STREAM TYPE: VC_JPEG_BASELINE\n");
		break;
	case VC_JPEG_PROGRESSIVE:
		dev_info(vpu->dev, "\t\t-JPEG: STREAM TYPE: VC_JPEG_PROGRESSIVE\n");
		break;
	case VC_JPEG_NONINTERLEAVED:
		dev_info(vpu->dev, "\t\t-JPEG: STREAM TYPE: VC_JPEG_NONINTERLEAVED\n");
		break;
	}

	if(imageInfo->thumbnail_type == VC_JPEG_THUMBNAIL_JPEG)
	{
		dev_info(vpu->dev, "\t-JPEG ThumbnailType: JPEG\n");
	}
	else if(imageInfo->thumbnail_type == VC_JPEG_NO_THUMBNAIL)
		dev_info(vpu->dev, "\t-JPEG ThumbnailType: NO THUMBNAIL\n");
	else if(imageInfo->thumbnail_type == VC_JPEG_THUMBNAIL_NOT_SUPPORTED_FORMAT)
		dev_info(vpu->dev, "\t-JPEG ThumbnailType: NOT SUPPORTED	THUMBNAIL\n");
}
#endif

static int jpeg_pp_init(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	int   ret;

	if (!jpegInst) {
		dev_info(vpu->dev, "%s - JPEG not inited!\n", __func__);
		return -1;
	}

	//for (i = 60; i <= 100; i++)
	//	vc8k_write_swreg(0, i);

	ret = vc_codec_pp_create(&ppInst);
	if (ret	!= VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - failed to create PP\n", __func__);
		return -1;
	}

	ret = vc_codec_pp_enable_combined(ppInst, jpegInst, VC_CODEC_PP_TYPE_JPEG);
	if (ret	!= VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - failed to enable combined mode\n",	__func__);
		goto cleanup_pp;
	}

	// get the current default PP config
	memset (&ppConfig, 0, sizeof(ppConfig));
	ret = vc_codec_pp_get_config(ppInst, &ppConfig);
	if (ret	!= VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - failed to get default PP config\n", __func__);
		goto cleanup_combined;
	}

	if (ctx->pp_ctx.enable_pp == true) {
		ret = jpeg_pp_out_config(ctx);
		if (ret	!= 0)
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

static int jpeg_pp_out_config(struct hantro_ctx	*ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct vc8k_pp_params *pp = &(ctx->pp_ctx);
	struct vb2_v4l2_buffer *dst_buf;

	dst_buf	= hantro_get_dst_buf(ctx);

	//For pp output to frame buffer(ultrafb/overlay)     
    ppConfig.input_rotation = pp->rotation;

	if((pp->pp_out_dst == 0) || (pp->pp_out_dst == 1)) {
		if ((pp->img_out_x != 0) || (pp->img_out_y != 0) ||
			(pp->img_out_w != pp->frame_buf_w) || (pp->img_out_h != pp->frame_buf_h)) {
			ppConfig.framebuffer.enable = 1;
			ppConfig.framebuffer.write_origin_x = pp->img_out_x;
			ppConfig.framebuffer.write_origin_y = pp->img_out_y;
			ppConfig.framebuffer.width = pp->frame_buf_w;
			ppConfig.framebuffer.height = pp->frame_buf_h;
		}
	}

	ppConfig.output.width	= pp->img_out_w;
	ppConfig.output.height = pp->img_out_h;

	if (pp->img_out_fmt == V4L2_PIX_FMT_NV12) {
		ppConfig.output.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;
	}
	else if (pp->img_out_fmt == V4L2_PIX_FMT_YUYV) {
		ppConfig.output.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_2_INTERLEAVED;		
	} else {
		/*
		 * PP output RGB format
		 */
		ppConfig.rgb.transform = VC_PP_YCBCR2RGB_TRANSFORM_CUSTOM;
		ppConfig.rgb.alpha	= 0xFF;
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
			 * should be RGB888, no	need to	check
			 */
			ppConfig.output.pix_format  = VC_PP_PIX_FMT_RGB32;
		}
	}

	if(pp->pp_out_dst == 0)
	{
		ppConfig.output.buffer_bus_addr	= readl_relaxed(vpu->dcultra_base + 0x1400);
	}
	else if(pp->pp_out_dst == 1)
	{
		ppConfig.output.buffer_bus_addr	= readl_relaxed(vpu->dcultra_base + 0x15C0);
	}
	else
	{
		//For PP output to memory(DMA) buffer
		if(pp->frame_buf_paddr)
		{
			ppConfig.output.buffer_bus_addr	= pp->frame_buf_paddr;			
		}
		else
		{
			ppConfig.output.buffer_bus_addr	= vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);;			
		}
	}

	ppConfig.output.buffer_chroma_bus_addr =	ppConfig.output.buffer_bus_addr	+
			ppConfig.output.width	* ppConfig.output.height;

	if ((ppConfig.output.buffer_bus_addr < 0x80000000UL) ||
		(ppConfig.output.buffer_bus_addr > 0xC0000000UL)) {
			dev_info(vpu->dev, "%s - Invalid PP	output address!	0x%llx\n",
					__func__, ppConfig.output.buffer_bus_addr);
		return -1;
	}
	return 0;
}

static int jpeg_pp_in_config(struct hantro_ctx *ctx, struct vc_jpeg_image_info *imageInfo)
{
	struct hantro_dev *vpu = ctx->dev;
	int   ret;

	ppConfig.input_crop_enable = 0;	/* crop	is not supported in current release */

	ppConfig.input.video_range = 1;
	ppConfig.input.width = imageInfo->output_width;
	ppConfig.input.height	= imageInfo->output_height;
	ppConfig.input.pix_format = VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR;

	// dev_info(vpu->dev, "jpeg_pp_in_config: %d x %d\n", imageInfo->output_width, imageInfo->output_height);

	if (imageInfo->output_format)
	{
		switch (imageInfo->output_format)
		{
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

	// and finally set the PP config to the	post-proc
	ret = vc_codec_pp_set_config(ppInst, &ppConfig);
	if (ret	!= VC_CODEC_PP_OK) {
		dev_info(vpu->dev, "%s - PPSetConfig failed! %d\n",	__func__, ret);
		return -1;
	}
	return 0;
}
