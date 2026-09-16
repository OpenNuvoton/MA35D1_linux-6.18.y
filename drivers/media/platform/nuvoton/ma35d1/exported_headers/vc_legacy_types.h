/* SPDX-License-Identifier: MIT */
/*
 * VC8000 fixed-width compatibility types used by the open kernel shim.
 *
 * Keep legacy aliases here only while the remaining decoder entry points are
 * migrated to the versioned VC codec ABI.
 */
#ifndef VC_LEGACY_TYPES_H
#define VC_LEGACY_TYPES_H

#include "vc_types.h"

#ifndef VC_EXPORT_SYMBOL
#define VC_EXPORT_SYMBOL(symbol)
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

#define UNUSED(value) ((void)(value))

typedef vc_u8 u8;
typedef vc_u16 u16;
typedef vc_u32 u32;
typedef vc_u64 u64;
typedef vc_s8 i8;
typedef vc_s16 i16;
typedef vc_s32 i32;
typedef vc_s64 i64;
typedef vc_u64 g1_addr_t;

typedef unsigned int u16x;
typedef signed int i16x;

#define DWL_USE_DEC_IRQ
#define DEC_TIMEOUT 5000
#define PP_TIMEOUT 5000

#define PP_H264DEC_PIPELINE_SUPPORT
#define PP_JPEGDEC_PIPELINE_SUPPORT
#define PP_PIPELINE_ENABLED

#endif /* VC_LEGACY_TYPES_H */
