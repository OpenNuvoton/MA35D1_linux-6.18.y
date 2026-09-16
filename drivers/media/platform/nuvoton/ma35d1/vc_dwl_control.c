// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Nuvoton Technology Corporation.

#include <linux/device.h>
#include <linux/export.h>
#include <linux/slab.h>

#include "hantro.h"
#include "vc_dwl_abi.h"
#include "vc_dwl_priv.h"
#include "vc_os_linux.h"

static bool vc_dwl_client_supported(u32 client_type)
{
	switch (client_type) {
	case DWL_CLIENT_TYPE_H264_DEC:
	case DWL_CLIENT_TYPE_MPEG4_DEC:
	case DWL_CLIENT_TYPE_JPEG_DEC:
	case DWL_CLIENT_TYPE_VC1_DEC:
	case DWL_CLIENT_TYPE_MPEG2_DEC:
	case DWL_CLIENT_TYPE_VP6_DEC:
	case DWL_CLIENT_TYPE_VP8_DEC:
	case DWL_CLIENT_TYPE_RV_DEC:
	case DWL_CLIENT_TYPE_AVS_DEC:
	case DWL_CLIENT_TYPE_PP:
		return true;
	default:
		return false;
	}
}

u32 DWLReadAsicCoreCount(void)
{
	return 1U;
}
EXPORT_SYMBOL(DWLReadAsicCoreCount);

const void *DWLInit(DWLInitParam_t *params)
{
	struct vc_dwl_context *context;

	if (!params || !vc_dwl_client_supported(params->clientType)) {
		dev_err(_vc8k_vpu->dev, "unsupported DWL client type\n");
		return NULL;
	}

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return NULL;

	context->client_type = params->clientType;
	context->num_cores = DWLReadAsicCoreCount();
	return context;
}
EXPORT_SYMBOL(DWLInit);

i32 DWLRelease(const void *instance)
{
	kfree(instance);
	return DWL_OK;
}
EXPORT_SYMBOL(DWLRelease);

i32 DWLReserveHwPipe(const void *instance, i32 *core_id)
{
	struct vc_dwl_context *context = (struct vc_dwl_context *)instance;
	i32 pp_core;

	if (!context || !core_id || context->client_type == DWL_CLIENT_TYPE_PP)
		return DWL_ERROR;

	*core_id = vc_os_hw_reserve(VC_OS_HW_DECODER,
				    context->client_type);
	if (*core_id != 0)
		return DWL_ERROR;

	pp_core = vc_os_hw_reserve(VC_OS_HW_POST_PROCESSOR, 0);
	if (pp_core != *core_id) {
		vc_os_hw_release(VC_OS_HW_DECODER, *core_id);
		return DWL_ERROR;
	}

	context->pp_reserved = 1;
	return DWL_OK;
}
EXPORT_SYMBOL(DWLReserveHwPipe);

i32 DWLReserveHw(const void *instance, i32 *core_id)
{
	const struct vc_dwl_context *context = instance;
	u32 engine;

	if (!context || !core_id)
		return DWL_ERROR;

	engine = context->client_type == DWL_CLIENT_TYPE_PP ?
		 VC_OS_HW_POST_PROCESSOR : VC_OS_HW_DECODER;
	*core_id = vc_os_hw_reserve(engine, context->client_type);

	if (*core_id < 0 || (engine == VC_OS_HW_POST_PROCESSOR && *core_id != 0))
		return DWL_ERROR;

	return DWL_OK;
}
EXPORT_SYMBOL(DWLReserveHw);

void DWLReleaseHw(const void *instance, i32 core_id)
{
	struct vc_dwl_context *context = (struct vc_dwl_context *)instance;

	if (!context || core_id < 0 || (u32)core_id >= context->num_cores)
		return;

	if (context->client_type == DWL_CLIENT_TYPE_PP) {
		vc_os_hw_release(VC_OS_HW_POST_PROCESSOR, core_id);
		return;
	}

	if (context->pp_reserved) {
		context->pp_reserved = 0;
		vc_os_hw_release(VC_OS_HW_POST_PROCESSOR, core_id);
	}

	vc_os_hw_release(VC_OS_HW_DECODER, core_id);
}
EXPORT_SYMBOL(DWLReleaseHw);
