// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Nuvoton Technology Corporation.
/*
 * Linux heap and memory services for the VC8000 core.
 *
 * Keep the legacy DWL entry points stable while the proprietary core is
 * migrated to the versioned vc_os_ops interface.
 */

#include <linux/export.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/stdarg.h>

#include "hantro.h"

#include "vc_compat.h"
#include "vc_dwl_abi.h"
#include "vc_hw_engine.h"
#include "vc_os_linux.h"

int vc_os_dma_alloc_from_coherent(u32 size, g1_addr_t *device_address,
				  void **cpu_address)
{
	return dma_alloc_from_dev_coherent(_vc8k_vpu->dev, size,
				   device_address, cpu_address);
}

void vc_os_dma_release_from_coherent(u32 size, void *cpu_address)
{
	dma_release_from_dev_coherent(_vc8k_vpu->dev, get_order(size),
				      cpu_address);
}

void *vc_os_dma_alloc_writecombine(u32 size, g1_addr_t *device_address)
{
	return dma_alloc_attrs(_vc8k_vpu->dev, size, device_address, GFP_KERNEL,
			       DMA_ATTR_WRITE_COMBINE);
}

void vc_os_dma_free_writecombine(u32 size, void *cpu_address,
				 g1_addr_t device_address)
{
	dma_free_attrs(_vc8k_vpu->dev, size, cpu_address, device_address,
			       DMA_ATTR_WRITE_COMBINE);
}

u32 vc_os_mmio_read32(u32 register_index)
{
	return vc8k_read_swreg(register_index);
}

void vc_os_mmio_write32(u32 register_index, u32 value)
{
	vc8k_write_swreg(value, register_index);
}

i32 vc_os_hw_reserve(u32 engine, u32 client_type)
{
	if (engine == VC_OS_HW_POST_PROCESSOR)
		return (i32)vc_hw_engine_command(VC_HW_PP_RESERVE, NULL);

	return (i32)vc_hw_engine_command(VC_HW_DEC_RESERVE,
				   (void *)(unsigned long)client_type);
}

void vc_os_hw_release(u32 engine, i32 core_id)
{
	unsigned int command = engine == VC_OS_HW_POST_PROCESSOR ?
		VC_HW_PP_RELEASE : VC_HW_DEC_RELEASE;

	vc_hw_engine_command(command, (void *)(unsigned long)core_id);
}

static long vc_os_hw_register_command(unsigned int command,
				      struct vc_os_register_buffer *buffer)
{
	struct vc_hw_register_set core = {
		.core_id = buffer->core_id,
		.registers = buffer->registers,
		.size = buffer->size,
	};

	return vc_hw_engine_command(command, &core);
}

long vc_os_hw_push_registers(u32 engine,
			     struct vc_os_register_buffer *buffer)
{
	unsigned int command = engine == VC_OS_HW_POST_PROCESSOR ?
		VC_HW_PP_PUSH_REGISTERS : VC_HW_DEC_PUSH_REGISTERS;

	return vc_os_hw_register_command(command, buffer);
}

long vc_os_hw_pull_registers(u32 engine,
			     struct vc_os_register_buffer *buffer)
{
	unsigned int command = engine == VC_OS_HW_POST_PROCESSOR ?
		VC_HW_PP_PULL_REGISTERS : VC_HW_DEC_PULL_REGISTERS;

	return vc_os_hw_register_command(command, buffer);
}

long vc_os_hw_wait(u32 engine, struct vc_os_register_buffer *buffer)
{
	unsigned int command = engine == VC_OS_HW_POST_PROCESSOR ?
		VC_HW_PP_WAIT : VC_HW_DEC_WAIT;

	return vc_os_hw_register_command(command, buffer);
}

void DWLLog(u32 level, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.fmt = format;
	vaf.va = &args;

	switch (level) {
	case DWL_LOG_ERROR:
		dev_err(_vc8k_vpu->dev, "%pV", &vaf);
		break;
	case DWL_LOG_WARNING:
		dev_warn(_vc8k_vpu->dev, "%pV", &vaf);
		break;
	case DWL_LOG_DEBUG:
		dev_dbg(_vc8k_vpu->dev, "%pV", &vaf);
		break;
	case DWL_LOG_INFO:
	default:
		dev_info(_vc8k_vpu->dev, "%pV", &vaf);
		break;
	}

	va_end(args);
}

void *DWLmalloc(u32 size)
{
	return kmalloc((size_t)size, GFP_KERNEL);
}
EXPORT_SYMBOL(DWLmalloc);

void DWLfree(void *address)
{
	kfree(address);
}
EXPORT_SYMBOL(DWLfree);

void *DWLcalloc(u32 count, u32 size)
{
	/* Preserve the legacy implementation: allocation is not zeroed. */
	return kmalloc((size_t)count * (size_t)size, GFP_KERNEL);
}
EXPORT_SYMBOL(DWLcalloc);

void *DWLmemcpy(void *destination, const void *source, u32 size)
{
	return memcpy(destination, source, (size_t)size);
}
EXPORT_SYMBOL(DWLmemcpy);

void *DWLmemset(void *destination, i32 value, u32 size)
{
	return memset(destination, (int)value, (size_t)size);
}
EXPORT_SYMBOL(DWLmemset);
