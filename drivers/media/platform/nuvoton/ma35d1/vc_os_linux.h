/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Nuvoton Technology Corporation. */
/* Transitional Linux primitives behind the VC8000 OS boundary. */

#ifndef VC_OS_LINUX_H
#define VC_OS_LINUX_H

#include "vc_legacy_types.h"

int vc_os_dma_alloc_from_coherent(u32 size, g1_addr_t *device_address,
				  void **cpu_address);
void vc_os_dma_release_from_coherent(u32 size, void *cpu_address);
void *vc_os_dma_alloc_writecombine(u32 size, g1_addr_t *device_address);
void vc_os_dma_free_writecombine(u32 size, void *cpu_address,
				 g1_addr_t device_address);

u32 vc_os_mmio_read32(u32 register_index);
void vc_os_mmio_write32(u32 register_index, u32 value);

#define VC_OS_HW_DECODER       0U
#define VC_OS_HW_POST_PROCESSOR 1U

struct vc_os_register_buffer {
	u32 core_id;
	u32 *registers;
	u32 size;
};

i32 vc_os_hw_reserve(u32 engine, u32 client_type);
void vc_os_hw_release(u32 engine, i32 core_id);
long vc_os_hw_push_registers(u32 engine,
			     struct vc_os_register_buffer *buffer);
long vc_os_hw_pull_registers(u32 engine,
			     struct vc_os_register_buffer *buffer);
long vc_os_hw_wait(u32 engine, struct vc_os_register_buffer *buffer);

#endif /* VC_OS_LINUX_H */
