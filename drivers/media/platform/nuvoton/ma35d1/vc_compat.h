/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Nuvoton Technology Corporation. */
/*
 * Linux compatibility boundary for the open VC8000 kernel shim.
 *
 * Keep all LINUX_VERSION_CODE checks in this header. Driver sources use
 * semantic VC_COMPAT_* interfaces and must not contain version conditionals.
 */

#ifndef VC8000_LINUX_COMPAT_H
#define VC8000_LINUX_COMPAT_H

#include <linux/dma-map-ops.h>
#include <linux/platform_device.h>
#include <linux/version.h>

/*
 * Transitional view of the per-device coherent-memory descriptor.
 *
 * The current V4L2 input path can receive buffers for which
 * vb2_plane_vaddr() returns NULL. Preserve the validated coherent-pool base
 * translation here until the shim owns a portable mapping for those buffers.
 */
struct vc_compat_dma_coherent_mem {
	void *virt_base;
	dma_addr_t device_base;
	unsigned long pfn_base;
	int size;
	unsigned long *bitmap;
	spinlock_t spinlock;
	bool use_dev_dma_pfn_offset;
};

static inline int
vc_compat_dma_declare_coherent_memory(struct device *dev,
				      phys_addr_t phys_addr,
				      dma_addr_t device_addr,
				      size_t size, void **virt_base)
{
	struct vc_compat_dma_coherent_mem *mem;
	int ret;

	ret = dma_declare_coherent_memory(dev, phys_addr, device_addr, size);
	if (ret)
		return ret;

	mem = (struct vc_compat_dma_coherent_mem *)dev->dma_mem;
	*virt_base = mem->virt_base;
	return 0;
}

/* platform_driver::remove changed from int to void in Linux 6.11. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#define VC_COMPAT_PLATFORM_REMOVE_RETURN	void
#define VC_COMPAT_PLATFORM_REMOVE_DONE()	return
#else
#define VC_COMPAT_PLATFORM_REMOVE_RETURN	int
#define VC_COMPAT_PLATFORM_REMOVE_DONE()	return 0
#endif

#endif /* VC8000_LINUX_COMPAT_H */
