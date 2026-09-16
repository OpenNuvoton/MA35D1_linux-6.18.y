// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Nuvoton Technology Corporation.

#include <linux/export.h>
#include <linux/string.h>

#include "vc_dec_abi.h"
#include "vc_dwl_abi.h"
#include "vc_dwl_priv.h"
#include "vc_hw_regs.h"
#include "vc_os_linux.h"

static u32 vc_dwl_fused_width(u32 fuse)
{
	if (fuse & 0x10000U)
		return 4096U;
	if (fuse & 0x8000U)
		return 1920U;
	if (fuse & 0x4000U)
		return 1280U;
	if (fuse & 0x2000U)
		return 720U;
	if (fuse & 0x1000U)
		return 352U;
	return 0U;
}

static void vc_dwl_limit_primary_codecs(DWLHwConfig_t *config,
					u32 decoder_fuse, u32 pp_fuse)
{
	u32 limit = vc_dwl_fused_width(decoder_fuse);
	u32 pp_limit = vc_dwl_fused_width(pp_fuse);

	if (config->maxDecPicWidth > limit)
		config->maxDecPicWidth = limit;
	if (config->maxPpOutPicWidth > pp_limit)
		config->maxPpOutPicWidth = pp_limit;
	if (!(decoder_fuse & (1U << VC_HW_FUSE_H264_SHIFT)))
		config->h264Support = H264_NOT_SUPPORTED;
	if (!(decoder_fuse & (1U << VC_HW_FUSE_JPEG_SHIFT)))
		config->jpegSupport = JPEG_NOT_SUPPORTED;
	else if (config->jpegSupport == JPEG_PROGRESSIVE &&
		 !(decoder_fuse & (1U << VC_HW_FUSE_PROGRESSIVE_JPEG_SHIFT)))
		config->jpegSupport = JPEG_BASELINE;
	if (!(decoder_fuse & (1U << VC_HW_FUSE_REF_BUFFER_SHIFT)))
		config->refBufSupport = REF_BUF_NOT_SUPPORTED;
	if (!(pp_fuse & (1U << VC_HW_PP_FUSE_ENABLE_SHIFT))) {
		config->ppSupport = PP_NOT_SUPPORTED;
		config->ppConfig = 0;
		config->maxPpOutPicWidth = 0;
	}
}

void DWLReadAsicConfig(DWLHwConfig_t *config)
{
	u32 id, product, primary, secondary, pp;

	if (!config)
		return;
	memset(config, 0, sizeof(*config));
	id = vc_os_mmio_read32(0);
	product = id >> 16;
	primary = vc_os_mmio_read32(VC_HW_DEC_SYNTH_REG);
	secondary = vc_os_mmio_read32(VC_HW_DEC_SYNTH2_REG);
	pp = vc_os_mmio_read32(VC_HW_PP_SYNTH_REG);

	config->h264Support = (primary >> VC_HW_CFG_H264_SHIFT) & 3U;
	config->jpegSupport = (primary >> VC_HW_CFG_JPEG_SHIFT) & 1U;
	if (config->jpegSupport &&
	    ((primary >> VC_HW_CFG_PROGRESSIVE_JPEG_SHIFT) & 1U))
		config->jpegSupport = JPEG_PROGRESSIVE;
	config->mpeg4Support = (primary >> VC_HW_CFG_MPEG4_SHIFT) & 3U;
	config->vc1Support = (primary >> VC_HW_CFG_VC1_SHIFT) & 3U;
	config->mpeg2Support = (primary >> VC_HW_CFG_MPEG2_SHIFT) & 1U;
	config->sorensonSparkSupport =
		(primary >> VC_HW_CFG_SORENSON_SHIFT) & 1U;
#ifndef DWL_REFBUFFER_DISABLE
	config->refBufSupport = (primary >> VC_HW_CFG_REF_BUFFER_SHIFT) & 1U;
#endif
	config->vp6Support = (primary >> VC_HW_CFG_VP6_SHIFT) & 1U;
	config->maxDecPicWidth = primary & 0x7ffU;
	if (config->refBufSupport) {
		config->refBufSupport |=
			((secondary >> VC_HW_CFG2_REF_INTERLACED_SHIFT) & 1U) << 1;
		config->refBufSupport |=
			((secondary >> VC_HW_CFG2_REF_DOUBLE_SHIFT) & 1U) << 2;
	}
	config->customMpeg4Support =
		(secondary >> VC_HW_CFG2_MPEG4_CUSTOM_SHIFT) & 1U;
	config->vp7Support = (secondary >> VC_HW_CFG_VP7_SHIFT) & 1U;
	config->vp8Support = (secondary >> VC_HW_CFG_VP8_SHIFT) & 1U;
	config->avsSupport = (secondary >> VC_HW_CFG_AVS_SHIFT) & 1U;
	if (product >= 0x8190U || product == 0x6731U || product == 0x6e64U)
		config->jpegESupport =
			(secondary >> VC_HW_CFG2_JPEG_EXT_SHIFT) & 1U;
	if (product >= 0x9170U || product == 0x6731U || product == 0x6e64U)
		config->rvSupport = (secondary >> VC_HW_CFG_RV_SHIFT) & 3U;
	config->mvcSupport = (secondary >> VC_HW_CFG2_MVC_SHIFT) & 3U;
	config->webpSupport = (secondary >> VC_HW_CFG_WEBP_SHIFT) & 1U;
	config->tiledModeSupport = (secondary >> VC_HW_CFG2_TILED_SHIFT) & 3U;
	config->maxDecPicWidth +=
		((secondary >> VC_HW_CFG2_WIDTH_EXT_SHIFT) & 3U) << 11;
	config->ecSupport = (secondary >> VC_HW_CFG2_EC_SHIFT) & 3U;
	config->strideSupport = (secondary >> VC_HW_CFG2_STRIDE_SHIFT) & 1U;
	config->fieldDpbSupport =
		(secondary >> VC_HW_CFG2_FIELD_DPB_SHIFT) & 1U;
	config->avsPlusSupport =
		(secondary >> VC_HW_CFG2_AVS_PLUS_SHIFT) & 1U;
	config->addr64Support = (secondary >> VC_HW_CFG2_ADDR64_SHIFT) & 1U;
	if (config->refBufSupport &&
	    (product == 0x6731U || product == 0x6e64U))
		config->refBufSupport |= 8U;
	if ((pp >> VC_HW_CFG_PP_SHIFT) & 1U) {
		config->ppSupport = 1U;
		config->maxPpOutPicWidth = pp & 0x1fffU;
		config->ppConfig = pp;
	}
	if (product >= 0x8190U || product == 0x6731U || product == 0x6e64U)
		vc_dwl_limit_primary_codecs(config,
			vc_os_mmio_read32(VC_DWL_DEC_FUSE_CFG),
			vc_os_mmio_read32(VC_DWL_PP_FUSE_CFG));
	config->webpSupport = 1U;
	vc_dwl_shadow_config[0][0] = id;
	vc_dwl_shadow_config[0][VC_HW_DEC_SYNTH_REG] = primary;
	vc_dwl_shadow_config[0][VC_HW_DEC_SYNTH2_REG] = secondary;
	vc_dwl_shadow_config[0][VC_HW_PP_SYNTH_REG] = pp;
	vc_dwl_shadow_config[0][VC_DWL_DEC_FUSE_CFG] =
		vc_os_mmio_read32(VC_DWL_DEC_FUSE_CFG);
	vc_dwl_shadow_config[0][VC_DWL_PP_FUSE_CFG] =
		vc_os_mmio_read32(VC_DWL_PP_FUSE_CFG);
}
EXPORT_SYMBOL(DWLReadAsicConfig);
