/* SPDX-License-Identifier: MIT */
/* Stable decoder capability definitions shared by the core and open shim. */
#ifndef VC_DEC_ABI_H
#define VC_DEC_ABI_H

#include "vc_legacy_types.h"

#define MAX_ASIC_CORES 4

#define MPEG4_NOT_SUPPORTED          ((u32)0x00)
#define MPEG4_CUSTOM_NOT_SUPPORTED   ((u32)0x00)
#define H264_NOT_SUPPORTED           ((u32)0x00)
#define VC1_NOT_SUPPORTED            ((u32)0x00)
#define MPEG2_NOT_SUPPORTED          ((u32)0x00)
#define JPEG_NOT_SUPPORTED           ((u32)0x00)
#define JPEG_BASELINE                ((u32)0x01)
#define JPEG_PROGRESSIVE             ((u32)0x02)
#define PP_NOT_SUPPORTED             ((u32)0x00)
#define PP_DEINTERLACING             ((u32)0x02000000)
#define PP_ALPHA_BLENDING            ((u32)0x01000000)
#define SORENSON_SPARK_NOT_SUPPORTED ((u32)0x00)
#define VP6_NOT_SUPPORTED            ((u32)0x00)
#define VP7_NOT_SUPPORTED            ((u32)0x00)
#define VP8_NOT_SUPPORTED            ((u32)0x00)
#define REF_BUF_NOT_SUPPORTED        ((u32)0x00)
#define AVS_NOT_SUPPORTED            ((u32)0x00)
#define JPEG_EXT_NOT_SUPPORTED       ((u32)0x00)
#define RV_NOT_SUPPORTED             ((u32)0x00)
#define MVC_NOT_SUPPORTED            ((u32)0x00)
#define WEBP_NOT_SUPPORTED           ((u32)0x00)

struct DecHwConfig_ {
	u32 mpeg4Support;
	u32 customMpeg4Support;
	u32 h264Support;
	u32 vc1Support;
	u32 mpeg2Support;
	u32 jpegSupport;
	u32 jpegProgSupport;
	u32 maxDecPicWidth;
	u32 ppSupport;
	u32 ppConfig;
	u32 maxPpOutPicWidth;
	u32 sorensonSparkSupport;
	u32 refBufSupport;
	u32 tiledModeSupport;
	u32 vp6Support;
	u32 vp7Support;
	u32 vp8Support;
	u32 avsSupport;
	u32 jpegESupport;
	u32 rvSupport;
	u32 mvcSupport;
	u32 webpSupport;
	u32 ecSupport;
	u32 strideSupport;
	u32 fieldDpbSupport;
	u32 avsPlusSupport;
	u32 addr64Support;
};
typedef struct DecHwConfig_ DecHwConfig;

struct DecSwHwBuild_ {
	u32 swBuild;
	u32 hwBuild;
	DecHwConfig hwConfig[MAX_ASIC_CORES];
};
typedef struct DecSwHwBuild_ DecSwHwBuild;

typedef enum {
	DEC_PIC_TYPE_I = 0,
	DEC_PIC_TYPE_P = 1,
	DEC_PIC_TYPE_B = 2,
	DEC_PIC_TYPE_D = 3,
	DEC_PIC_TYPE_FI = 4,
	DEC_PIC_TYPE_BI = 5
} DecPicCodingType;

typedef enum {
	DEC_REF_FRM_RASTER_SCAN = 0x0,
	DEC_REF_FRM_TILED_DEFAULT = 0x1,
	DEC_DPB_ALLOW_FIELD_ORDERING = 0x40000000
} DecDpbFlags;

#define DEC_REF_FRM_FMT_MASK 0x01

typedef enum {
	DEC_OUT_FRM_RASTER_SCAN = 0,
	DEC_OUT_FRM_TILED_8X4 = 1
} DecOutFrmFormat;

typedef enum {
	DEC_EC_PICTURE_FREEZE = 0,
	DEC_EC_VIDEO_FREEZE = 1,
	DEC_EC_PARTIAL_FREEZE = 2,
	DEC_EC_PARTIAL_IGNORE = 3
} DecErrorHandling;

#endif /* VC_DEC_ABI_H */
