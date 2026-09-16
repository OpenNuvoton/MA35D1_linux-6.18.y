/* SPDX-License-Identifier: MIT */
/*
 * Fixed-width types for the VC8000 core/shim ABI.
 *
 * This header is intentionally independent of Linux and the C library.
 */

#ifndef VC_TYPES_H
#define VC_TYPES_H

typedef __UINT8_TYPE__ vc_u8;
typedef __UINT16_TYPE__ vc_u16;
typedef __UINT32_TYPE__ vc_u32;
typedef unsigned long long vc_u64;

typedef __INT8_TYPE__ vc_s8;
typedef __INT16_TYPE__ vc_s16;
typedef __INT32_TYPE__ vc_s32;
typedef signed long long vc_s64;

#define VC_FALSE 0
#define VC_TRUE  1

#endif /* VC_TYPES_H */
