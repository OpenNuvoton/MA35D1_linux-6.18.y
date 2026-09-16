/* SPDX-License-Identifier: MIT */
/* Stable codec boundary between the proprietary VC8000 core and open shim. */
#ifndef VC_CODEC_ABI_H
#define VC_CODEC_ABI_H

#include "vc_types.h"
#include "vc_pp_abi.h"
#include "vc_jpeg_abi.h"
#include "vc_h264_abi.h"

#define VC_CODEC_ABI_MAJOR	1U
#define VC_CODEC_ABI_MINOR	4U
typedef const void *vc_codec_handle_t;

/* Public limits used by the open JPEG scheduling policy. */
#define VC_CODEC_JPEG_MAX_PIXEL_AMOUNT	16370688U
#define VC_CODEC_JPEG_MAX_SLICE_SIZE_8190	8100U

#define VC_CODEC_PP_ASIC_OUT_FORMAT_420	5U
#define VC_CODEC_PP_ASIC_IN_FORMAT_420_SEMIPLANAR	1U
#define VC_CODEC_PP_TO_FIXED(d, q) \
	((vc_u32)(d) * (vc_u32)(1U << (q)))
#define VC_CODEC_PP_FIXED_DIV(a, b)	((a) / (b))

#define VC_CODEC_PP_OK	0
#define VC_CODEC_PP_TYPE_H264	1U
#define VC_CODEC_PP_TYPE_JPEG	3U

vc_s32 vc_codec_pp_create(vc_codec_handle_t *post_processor);
vc_s32 vc_codec_pp_enable_combined(vc_codec_handle_t post_processor,
				   vc_codec_handle_t decoder,
				   vc_u32 decoder_type);
vc_s32 vc_codec_pp_disable_combined(vc_codec_handle_t post_processor,
				    vc_codec_handle_t decoder);
vc_s32 vc_codec_pp_get_config(vc_codec_handle_t post_processor,
			      struct vc_pp_config *config);
vc_s32 vc_codec_pp_set_config(vc_codec_handle_t post_processor,
			      const struct vc_pp_config *config);
void vc_codec_pp_release(vc_codec_handle_t post_processor);
void vc_codec_h264_apply_hw_defaults(vc_codec_handle_t decoder);
vc_s32 vc_codec_h264_create(vc_codec_handle_t *decoder);
void vc_codec_h264_destroy(vc_codec_handle_t decoder);
vc_s32 vc_codec_h264_decode(vc_codec_handle_t decoder,
			    const struct vc_h264_input *input,
			    struct vc_h264_output *output);
vc_s32 vc_codec_h264_get_info(vc_codec_handle_t decoder,
			      struct vc_h264_info *info);
vc_s32 vc_codec_h264_next_picture(vc_codec_handle_t decoder,
				  struct vc_h264_picture *picture,
				  vc_u32 end_of_stream);
void vc_codec_jpeg_apply_hw_defaults(vc_codec_handle_t decoder);
vc_s32 vc_codec_jpeg_create(vc_codec_handle_t *decoder);
void vc_codec_jpeg_destroy(vc_codec_handle_t decoder);
vc_s32 vc_codec_jpeg_get_image_info(vc_codec_handle_t decoder,
				    struct vc_jpeg_input *input,
				    struct vc_jpeg_image_info *info);
vc_s32 vc_codec_jpeg_decode(vc_codec_handle_t decoder,
			    struct vc_jpeg_input *input,
			    struct vc_jpeg_output *output);
#endif
