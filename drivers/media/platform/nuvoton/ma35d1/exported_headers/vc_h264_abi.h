/* SPDX-License-Identifier: MIT */
/* Versioned H.264 decoder boundary between the VC8000 core and open shim. */
#ifndef VC_H264_ABI_H
#define VC_H264_ABI_H

#include "vc_types.h"

#define VC_H264_OK 0
#define VC_H264_STRM_PROCESSED 1
#define VC_H264_PIC_RDY 2
#define VC_H264_PIC_DECODED 3
#define VC_H264_HDRS_RDY 4
#define VC_H264_ADVANCED_TOOLS 5
#define VC_H264_PENDING_FLUSH 6
#define VC_H264_NONREF_PIC_SKIPPED 7
#define VC_H264_END_OF_STREAM 8
#define VC_H264_BUF_EMPTY 12
#define VC_H264_PARAM_ERROR (-1)
#define VC_H264_STRM_ERROR (-2)
#define VC_H264_NOT_INITIALIZED (-3)
#define VC_H264_MEMFAIL (-4)
#define VC_H264_INITFAIL (-5)
#define VC_H264_HDRS_NOT_RDY (-6)
#define VC_H264_STREAM_NOT_SUPPORTED (-8)
#define VC_H264_HW_RESERVED (-254)
#define VC_H264_HW_TIMEOUT (-255)
#define VC_H264_HW_BUS_ERROR (-256)
#define VC_H264_SYSTEM_ERROR (-257)
#define VC_H264_DWL_ERROR (-258)
#define VC_H264_EVALUATION_LIMIT_EXCEEDED (-999)
#define VC_H264_FORMAT_NOT_SUPPORTED (-1000)

#define VC_H264_SEMIPLANAR_YUV420 0x020001U
#define VC_H264_TILED_YUV420 0x020002U
#define VC_H264_YUV400 0x080000U

struct vc_h264_input {
	vc_u8 *stream;
	vc_u64 stream_bus_address;
	vc_u32 data_length;
	vc_u32 picture_id;
	vc_u32 skip_non_reference;
	void *user_data;
};

struct vc_h264_output {
	vc_u8 *stream_current_position;
	vc_u64 stream_current_bus_address;
	vc_u32 data_left;
};

struct vc_h264_crop {
	vc_u32 left_offset;
	vc_u32 output_width;
	vc_u32 top_offset;
	vc_u32 output_height;
};

struct vc_h264_picture {
	vc_u32 width;
	vc_u32 height;
	struct vc_h264_crop crop;
	const vc_u32 *output_picture;
	vc_u64 output_picture_bus_address;
	vc_u32 picture_id;
	vc_u32 decode_id[2];
	vc_u32 coding_type[2];
	vc_u32 is_idr[2];
	vc_u32 error_macroblocks;
	vc_u32 interlaced;
	vc_u32 field_picture;
	vc_u32 top_field;
	vc_u32 view_id;
	vc_u32 output_format;
	vc_u32 picture_structure;
};

struct vc_h264_info {
	vc_u32 width;
	vc_u32 height;
	vc_u32 video_range;
	vc_u32 matrix_coefficients;
	struct vc_h264_crop crop;
	vc_u32 output_format;
	vc_u32 sar_width;
	vc_u32 sar_height;
	vc_u32 monochrome;
	vc_u32 interlaced_sequence;
	vc_u32 dpb_mode;
	vc_u32 picture_buffer_size;
	vc_u32 pp_multibuffer_size;
};

#endif /* VC_H264_ABI_H */
