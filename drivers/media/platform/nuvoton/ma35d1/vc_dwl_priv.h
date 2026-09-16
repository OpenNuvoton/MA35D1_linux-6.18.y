/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Nuvoton Technology Corporation. */

#ifndef VC_DWL_PRIV_H
#define VC_DWL_PRIV_H

#include "vc_legacy_types.h"
#include "vc_dwl_abi.h"
#include "vc_hw_regs.h"

#define VC_DWL_PP_REG_START	0xf0U
#define VC_DWL_DEC_REG_START	0x04U
#define VC_DWL_PP_FUSE_CFG	99U
#define VC_DWL_DEC_FUSE_CFG	57U
#define VC_DWL_HW_ENABLE_BIT	0x000001U

#define VC_DWL_DEC_IRQ_ABORT	(1U << 11)
#define VC_DWL_DEC_IRQ_READY	(1U << 12)
#define VC_DWL_DEC_IRQ_BUS	(1U << 13)
#define VC_DWL_DEC_IRQ_BUFFER	(1U << 14)
#define VC_DWL_DEC_IRQ_ASO	(1U << 15)
#define VC_DWL_DEC_IRQ_ERROR	(1U << 16)
#define VC_DWL_DEC_IRQ_SLICE	(1U << 17)
#define VC_DWL_DEC_IRQ_TIMEOUT	(1U << 18)
#define VC_DWL_PP_IRQ_READY	(1U << 12)
#define VC_DWL_PP_IRQ_BUS	(1U << 13)

#define VC_DWL_DECODER_INT(ctx) \
	((DWLReadReg((ctx), VC_DWL_DEC_REG_START) >> 11) & 0xffU)
#define VC_DWL_PP_INT(ctx) \
	((DWLReadReg((ctx), VC_DWL_PP_REG_START) >> 11) & 0xffU)

struct vc_dwl_context {
	u32 client_type;
	u32 num_cores;
	u32 pp_reserved;
};

#define VC_DWL_REGISTER_COUNT	154U

extern u32 vc_dwl_shadow_regs[MAX_ASIC_CORES][VC_DWL_REGISTER_COUNT];
extern u32 vc_dwl_shadow_config[MAX_ASIC_CORES][VC_DWL_REGISTER_COUNT];

i32 DWLWaitPpHwReady(const void *instance, i32 core_id, u32 timeout);
i32 DWLWaitDecHwReady(const void *instance, i32 core_id, u32 timeout);

#endif /* VC_DWL_PRIV_H */
