// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Nuvoton Technology Corporation.

#include <linux/export.h>

#include "vc_dwl_abi.h"
#include "vc_dwl_priv.h"
#include "vc_os_linux.h"

u32 vc_dwl_shadow_regs[MAX_ASIC_CORES][VC_DWL_REGISTER_COUNT];
u32 vc_dwl_shadow_config[MAX_ASIC_CORES][VC_DWL_REGISTER_COUNT];

u32 DWLReadAsicID(void)
{
	u32 id = vc_dwl_shadow_config[0][0];

	if (!id) {
		id = vc_os_mmio_read32(0);
		vc_dwl_shadow_config[0][0] = id;
	}

	return id;
}
EXPORT_SYMBOL(DWLReadAsicID);

static u32 vc_dwl_engine(const struct vc_dwl_context *context)
{
	return context->client_type == DWL_CLIENT_TYPE_PP ?
		VC_OS_HW_POST_PROCESSOR : VC_OS_HW_DECODER;
}

static u32 vc_dwl_register_bytes(u32 engine)
{
#ifdef USE_64BIT_ENV
	return engine == VC_OS_HW_POST_PROCESSOR ? 50U * 4U : 87U * 4U;
#else
	return engine == VC_OS_HW_POST_PROCESSOR ? 41U * 4U : 60U * 4U;
#endif
}

void DWLWriteReg(const void *instance, i32 core_id, u32 offset, u32 value)
{
	(void)instance;
	vc_dwl_shadow_regs[core_id][offset / 4U] = value;
}
EXPORT_SYMBOL(DWLWriteReg);

u32 DWLReadReg(const void *instance, i32 core_id, u32 offset)
{
	(void)instance;
	return vc_dwl_shadow_regs[core_id][offset / 4U];
}
EXPORT_SYMBOL(DWLReadReg);

static void vc_dwl_push(const struct vc_dwl_context *context, i32 core_id)
{
	u32 engine = vc_dwl_engine(context);
	struct vc_os_register_buffer buffer = {
		.core_id = core_id,
		.registers = vc_dwl_shadow_regs[core_id],
		.size = vc_dwl_register_bytes(engine),
	};

	vc_os_hw_push_registers(engine, &buffer);
}

void DWLEnableHW(const void *instance, i32 core_id, u32 offset, u32 value)
{
	const struct vc_dwl_context *context = instance;

	DWLWriteReg(instance, core_id, offset, value);
	vc_dwl_push(context, core_id);
}
EXPORT_SYMBOL(DWLEnableHW);

void DWLDisableHW(const void *instance, i32 core_id, u32 offset, u32 value)
{
	const struct vc_dwl_context *context = instance;

	DWLWriteReg(instance, core_id, offset, value);
	vc_dwl_push(context, core_id);
}
EXPORT_SYMBOL(DWLDisableHW);

i32 DWLWaitHwReady(const void *instance, i32 core_id, u32 timeout)
{
	const struct vc_dwl_context *context = instance;
	u32 engine = vc_dwl_engine(context);
	struct vc_os_register_buffer buffer = {
		.core_id = core_id,
		.registers = vc_dwl_shadow_regs[core_id],
		.size = vc_dwl_register_bytes(engine),
	};

	(void)timeout;
#ifdef DWL_USE_DEC_IRQ
	vc_os_hw_wait(engine, &buffer);
	return DWL_HW_WAIT_OK;
#else
	return vc_os_hw_pull_registers(engine, &buffer) < 0 ?
		DWL_HW_WAIT_ERROR : DWL_HW_WAIT_OK;
#endif
}
EXPORT_SYMBOL(DWLWaitHwReady);
