/* SPDX-License-Identifier: MIT */
/* Versioned JPEG decoder boundary between the VC8000 core and open shim. */
#ifndef VC_JPEG_ABI_H
#define VC_JPEG_ABI_H

#include "vc_types.h"

#define VC_JPEG_YCBCR400 0x080000U
#define VC_JPEG_YCBCR420_SEMIPLANAR 0x020001U
#define VC_JPEG_YCBCR422_SEMIPLANAR 0x010001U
#define VC_JPEG_YCBCR440 0x010004U
#define VC_JPEG_YCBCR411_SEMIPLANAR 0x100000U
#define VC_JPEG_YCBCR444_SEMIPLANAR 0x200000U

#define VC_JPEG_BASELINE 0
#define VC_JPEG_PROGRESSIVE 1
#define VC_JPEG_NONINTERLEAVED 2

#define VC_JPEG_SLICE_READY 2
#define VC_JPEG_FRAME_READY 1
#define VC_JPEG_STRM_PROCESSED 3
#define VC_JPEG_SCAN_PROCESSED 4
#define VC_JPEG_OK 0
#define VC_JPEG_ERROR (-1)
#define VC_JPEG_UNSUPPORTED (-2)
#define VC_JPEG_PARAM_ERROR (-3)
#define VC_JPEG_MEMFAIL (-4)
#define VC_JPEG_INITFAIL (-5)
#define VC_JPEG_INVALID_STREAM_LENGTH (-6)
#define VC_JPEG_STRM_ERROR (-7)
#define VC_JPEG_INVALID_INPUT_BUFFER_SIZE (-8)
#define VC_JPEG_HW_RESERVED (-9)
#define VC_JPEG_INCREASE_INPUT_BUFFER (-10)
#define VC_JPEG_SLICE_MODE_UNSUPPORTED (-11)
#define VC_JPEG_DWL_HW_TIMEOUT (-253)
#define VC_JPEG_DWL_ERROR (-254)
#define VC_JPEG_HW_BUS_ERROR (-255)
#define VC_JPEG_SYSTEM_ERROR (-256)
#define VC_JPEG_FORMAT_NOT_SUPPORTED (-1000)

#define VC_JPEG_THUMBNAIL_JPEG 0x10
#define VC_JPEG_THUMBNAIL_NOT_SUPPORTED_FORMAT 0x11
#define VC_JPEG_NO_THUMBNAIL 0x12
#define VC_JPEG_IMAGE 0
#define VC_JPEG_THUMBNAIL 1

struct vc_jpeg_linear_mem {
	vc_u32 *virtual_address;
	vc_u64 bus_address;
};

struct vc_jpeg_image_info {
	vc_u32 display_width;
	vc_u32 display_height;
	vc_u32 output_width;
	vc_u32 output_height;
	vc_u32 version;
	vc_u32 units;
	vc_u32 x_density;
	vc_u32 y_density;
	vc_u32 output_format;
	vc_u32 coding_mode;
	vc_u32 thumbnail_type;
	vc_u32 display_width_thumb;
	vc_u32 display_height_thumb;
	vc_u32 output_width_thumb;
	vc_u32 output_height_thumb;
	vc_u32 output_format_thumb;
	vc_u32 coding_mode_thumb;
};

struct vc_jpeg_input {
	struct vc_jpeg_linear_mem stream_buffer;
	vc_u32 stream_length;
	vc_u32 buffer_size;
	vc_u32 image_type;
	vc_u32 slice_mb_set;
	struct vc_jpeg_linear_mem picture_buffer_y;
	struct vc_jpeg_linear_mem picture_buffer_cbcr;
	struct vc_jpeg_linear_mem picture_buffer_cr;
};

struct vc_jpeg_output {
	struct vc_jpeg_linear_mem output_picture_y;
	struct vc_jpeg_linear_mem output_picture_cbcr;
	struct vc_jpeg_linear_mem output_picture_cr;
};

#endif /* VC_JPEG_ABI_H */
