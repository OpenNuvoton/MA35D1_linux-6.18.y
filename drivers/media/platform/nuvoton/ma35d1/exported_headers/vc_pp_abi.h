/* SPDX-License-Identifier: MIT */
/* Nuvoton-owned post-processor configuration ABI. */
#ifndef VC_PP_ABI_H
#define VC_PP_ABI_H

#include "vc_types.h"

#define VC_PP_ABI_MAJOR 1U
#define VC_PP_ABI_MINOR 0U

#define VC_PP_PIX_FMT_YCBCR_4_0_0             0x080000U
#define VC_PP_PIX_FMT_YCBCR_4_2_2_INTERLEAVED 0x010001U
#define VC_PP_PIX_FMT_YCBCR_4_2_2_SEMIPLANAR  0x010002U
#define VC_PP_PIX_FMT_YCBCR_4_4_0             0x010004U
#define VC_PP_PIX_FMT_YCBCR_4_2_0_SEMIPLANAR  0x020001U
#define VC_PP_PIX_FMT_YCBCR_4_1_1_SEMIPLANAR  0x100001U
#define VC_PP_PIX_FMT_YCBCR_4_4_4_SEMIPLANAR  0x200001U
#define VC_PP_PIX_FMT_RGB16_5_6_5              0x040002U
#define VC_PP_PIX_FMT_RGB32                    0x041001U

#define VC_PP_YCBCR2RGB_TRANSFORM_CUSTOM 0U

struct vc_pp_input_image {
	vc_u32 pix_format;
	vc_u32 video_range;
	vc_u32 width;
	vc_u32 height;
};

struct vc_pp_output_image {
	vc_u32 pix_format;
	vc_u32 width;
	vc_u32 height;
	vc_u64 buffer_bus_addr;
	vc_u64 buffer_chroma_bus_addr;
};

struct vc_pp_rgb_coefficients {
	vc_u32 a;
	vc_u32 b;
	vc_u32 c;
	vc_u32 d;
	vc_u32 e;
};

struct vc_pp_rgb_output {
	vc_u32 transform;
	vc_u32 alpha;
	struct vc_pp_rgb_coefficients coefficients;
	vc_u32 dithering_enable;
};

struct vc_pp_framebuffer_output {
	vc_u32 enable;
	vc_s32 write_origin_x;
	vc_s32 write_origin_y;
	vc_u32 width;
	vc_u32 height;
};

struct vc_pp_config {
	vc_u32 struct_size;
	vc_u32 input_crop_enable;
	vc_u32 input_rotation;
	vc_u32 reserved0;
	struct vc_pp_input_image input;
	struct vc_pp_output_image output;
	struct vc_pp_rgb_output rgb;
	struct vc_pp_framebuffer_output framebuffer;
	vc_u64 reserved[4];
};

#endif /* VC_PP_ABI_H */
