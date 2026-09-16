// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Nuvoton Technology Corporation.

#include <linux/device.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "hantro.h"
#include "vc_dwl_abi.h"
#include "vc_os_linux.h"

struct vc_dwl_dma_block {
	void *owner;
	void *cpu_address;
	dma_addr_t device_address;
	u32 span;
	bool used;
};

static struct vc_dwl_dma_block *vc_dma_pool;
static u32 vc_dma_block_count;
static u32 vc_dma_blocks_used;
static DEFINE_MUTEX(vc_dma_lock);

int VC8K_PreAllocDMABuff(struct device *dev, struct hantro_dev *vpu)
{
	struct vc8k_config *config = &vpu->vc8k_cfg;
	u32 i;

	(void)dev;
	vc_dma_block_count = config->res_mem_size / PA_BLK_SIZE;
	vc_dma_pool = kcalloc(vc_dma_block_count, sizeof(*vc_dma_pool),
			      GFP_KERNEL);
	if (!vc_dma_pool)
		return -ENOMEM;

	for (i = 0; i < vc_dma_block_count; ++i) {
		size_t offset = (size_t)i * PA_BLK_SIZE;

		vc_dma_pool[i].device_address = config->res_mem_base + offset;
		vc_dma_pool[i].cpu_address =
			(void *)(config->res_mem_virt + offset);
	}

	vc_dma_blocks_used = 0;
	return 0;
}
EXPORT_SYMBOL(VC8K_PreAllocDMABuff);

void VC8K_ReleaseDMABuff(struct device *dev)
{
	(void)dev;
	kfree(vc_dma_pool);
	vc_dma_pool = NULL;
	vc_dma_block_count = 0;
	vc_dma_blocks_used = 0;
}
EXPORT_SYMBOL(VC8K_ReleaseDMABuff);

static void *vc_dwl_pool_alloc(void *owner, dma_addr_t *device_address,
			       u32 size)
{
	u32 required = DIV_ROUND_UP(size, PA_BLK_SIZE);
	u32 run = 0;
	u32 start = 0;
	u32 i;

	if (!vc_dma_pool || !required || required > vc_dma_block_count)
		return NULL;

	mutex_lock(&vc_dma_lock);
	for (i = 0; i < vc_dma_block_count; ++i) {
		if (!vc_dma_pool[i].used) {
			if (!run)
				start = i;
			if (++run == required)
				break;
		} else {
			run = 0;
		}
	}

	if (run != required) {
		mutex_unlock(&vc_dma_lock);
		return NULL;
	}

	for (i = start; i < start + required; ++i) {
		vc_dma_pool[i].owner = owner;
		vc_dma_pool[i].used = true;
		vc_dma_pool[i].span = start + required - i;
	}
	vc_dma_blocks_used += required;
	*device_address = vc_dma_pool[start].device_address;
	mutex_unlock(&vc_dma_lock);

	return vc_dma_pool[start].cpu_address;
}

static i32 vc_dwl_pool_free(void *owner, dma_addr_t device_address)
{
	dma_addr_t base;
	u32 start;
	u32 span;
	u32 i;

	if (!vc_dma_pool || !vc_dma_block_count)
		return DWL_ERROR;

	base = vc_dma_pool[0].device_address;
	if (device_address < base ||
	    device_address >= base + (dma_addr_t)vc_dma_block_count * PA_BLK_SIZE)
		return DWL_ERROR;

	start = (device_address - base) / PA_BLK_SIZE;
	if (vc_dma_pool[start].device_address != device_address ||
	    vc_dma_pool[start].owner != owner || !vc_dma_pool[start].used)
		return DWL_ERROR;

	span = vc_dma_pool[start].span;
	if (!span || start + span > vc_dma_block_count)
		return DWL_ERROR;

	mutex_lock(&vc_dma_lock);
	for (i = start; i < start + span; ++i) {
		vc_dma_pool[i].owner = NULL;
		vc_dma_pool[i].used = false;
		vc_dma_pool[i].span = 0;
	}
	vc_dma_blocks_used -= span;
	mutex_unlock(&vc_dma_lock);
	return DWL_OK;
}

i32 DWLMallocLinear(const void *instance, u32 size, DWLLinearMem_t *memory)
{
	void *cpu_address = NULL;
	int ret;

	(void)instance;
	if (!memory || !size)
		return DWL_ERROR;

	if (_vc8k_vpu->vc8k_cfg.use_dev_coherent) {
		ret = vc_os_dma_alloc_from_coherent(size, &memory->busAddress,
					    &cpu_address);
		if (!ret || !cpu_address)
			return DWL_ERROR;
	} else {
		cpu_address = vc_os_dma_alloc_writecombine(size,
						    &memory->busAddress);
		if (!cpu_address)
			return DWL_ERROR;
	}

	memory->virtualAddress = cpu_address;
	memory->size = size;
	return DWL_OK;
}
EXPORT_SYMBOL(DWLMallocLinear);

void DWLFreeLinear(const void *instance, DWLLinearMem_t *memory)
{
	(void)instance;
	if (!memory || !memory->virtualAddress)
		return;

	if (_vc8k_vpu->vc8k_cfg.use_dev_coherent)
		vc_os_dma_release_from_coherent(memory->size,
						memory->virtualAddress);
	else
		vc_os_dma_free_writecombine(memory->size, memory->virtualAddress,
					    memory->busAddress);
}
EXPORT_SYMBOL(DWLFreeLinear);

i32 DWLMallocRefFrm(const void *instance, u32 size, DWLLinearMem_t *memory)
{
	void *cpu_address = NULL;
	int ret;

	if (!memory || !size)
		return DWL_ERROR;

	if (!_vc8k_vpu->vc8k_cfg.have_res_mem)
		return DWLMallocLinear(instance, size, memory);

	if (_vc8k_vpu->vc8k_cfg.use_dev_coherent) {
		ret = vc_os_dma_alloc_from_coherent(size, &memory->busAddress,
					    &cpu_address);
		if (!ret || !cpu_address)
			return DWL_ERROR;
	} else {
		cpu_address = vc_dwl_pool_alloc((void *)instance,
						&memory->busAddress, size);
		if (!cpu_address)
			return DWLMallocLinear(instance, size, memory);
	}

	memory->virtualAddress = cpu_address;
	memory->size = size;
	return DWL_OK;
}
EXPORT_SYMBOL(DWLMallocRefFrm);

void DWLFreeRefFrm(const void *instance, DWLLinearMem_t *memory)
{
	if (!memory)
		return;

	if (!_vc8k_vpu->vc8k_cfg.have_res_mem) {
		DWLFreeLinear(instance, memory);
		return;
	}

	if (_vc8k_vpu->vc8k_cfg.use_dev_coherent) {
		vc_os_dma_release_from_coherent(memory->size,
						memory->virtualAddress);
		return;
	}

	if (vc_dwl_pool_free((void *)instance, memory->busAddress) != DWL_OK)
		DWLFreeLinear(instance, memory);
}
EXPORT_SYMBOL(DWLFreeRefFrm);
