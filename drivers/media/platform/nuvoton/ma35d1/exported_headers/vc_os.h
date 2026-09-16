/* SPDX-License-Identifier: MIT */
/*
 * Future operating-system abstraction ABI for the VC8000 binary core.
 *
 * This file defines an interface only. It has no implementation and exposes
 * no Linux types or structures.
 */

#ifndef VC_OS_H
#define VC_OS_H

#include "vc_types.h"

#define VC_OS_ABI_MAGIC 0x56434f53U
#define VC_OS_ABI_MAJOR 1U
#define VC_OS_ABI_MINOR 0U

/* Opaque token owned and interpreted by the OS shim. */
typedef vc_u64 vc_os_handle_t;

/* Device-visible address. This is not a CPU pointer or an OS DMA type. */
typedef vc_u64 vc_dma_addr_t;

enum vc_status {
	VC_STATUS_OK = 0,
	VC_STATUS_INVALID_ARGUMENT = -1,
	VC_STATUS_NO_MEMORY = -2,
	VC_STATUS_BUSY = -3,
	VC_STATUS_TIMED_OUT = -4,
	VC_STATUS_IO_ERROR = -5,
	VC_STATUS_NOT_SUPPORTED = -6,
};

enum vc_log_level {
	VC_LOG_ERROR = 0,
	VC_LOG_WARNING = 1,
	VC_LOG_INFO = 2,
	VC_LOG_DEBUG = 3,
};

/* Size-tagged description of memory shared with VC8000 hardware. */
struct vc_dma_buffer {
	vc_u32 struct_size;
	vc_u32 flags;
	vc_os_handle_t handle;
	vc_dma_addr_t device_address;
	vc_u64 size;
	vc_u64 reserved[4];
};

/*
 * Placeholder callback table supplied by the future open kernel shim.
 * Members are append-only; struct_size and capability_bits control discovery.
 */
struct vc_os_ops {
	vc_u32 struct_size;
	vc_u32 abi_magic;
	vc_u32 abi_major;
	vc_u32 abi_minor;
	vc_u64 capability_bits;
	vc_os_handle_t context;

	/* General storage owned by the core. */
	void *(*heap_alloc)(vc_os_handle_t context, vc_u64 size,
			    vc_u64 alignment, vc_u32 flags);
	void (*heap_free)(vc_os_handle_t context, void *address);

	/* Fixed-message diagnostic output supplied by the OS shim. */
	void (*log)(vc_os_handle_t context, enum vc_log_level level,
		    const char *message);

	/* DMA allocation and cache-ownership transitions. */
	enum vc_status (*dma_alloc)(vc_os_handle_t context, vc_u64 size,
				    vc_u64 alignment, vc_u32 flags,
				    struct vc_dma_buffer *buffer);
	void (*dma_free)(vc_os_handle_t context,
			 struct vc_dma_buffer *buffer);
	enum vc_status (*dma_sync_for_cpu)(vc_os_handle_t context,
					   vc_os_handle_t handle,
					   vc_u64 offset, vc_u64 size);
	enum vc_status (*dma_sync_for_device)(vc_os_handle_t context,
					      vc_os_handle_t handle,
					      vc_u64 offset, vc_u64 size);

	/* Sleeping synchronization and completion services. */
	vc_os_handle_t (*mutex_create)(vc_os_handle_t context);
	void (*mutex_destroy)(vc_os_handle_t context, vc_os_handle_t mutex);
	enum vc_status (*mutex_lock)(vc_os_handle_t context,
				     vc_os_handle_t mutex, vc_u64 timeout_ns);
	void (*mutex_unlock)(vc_os_handle_t context, vc_os_handle_t mutex);
	vc_os_handle_t (*event_create)(vc_os_handle_t context);
	void (*event_destroy)(vc_os_handle_t context, vc_os_handle_t event);
	enum vc_status (*event_wait)(vc_os_handle_t context,
				     vc_os_handle_t event, vc_u64 timeout_ns);
	void (*event_signal)(vc_os_handle_t context, vc_os_handle_t event);

	/* Ordered register access; the shim maps region identifiers to MMIO. */
	vc_u32 (*mmio_read32)(vc_os_handle_t context, vc_u32 region,
			      vc_u32 offset);
	void (*mmio_write32)(vc_os_handle_t context, vc_u32 region,
			     vc_u32 offset, vc_u32 value);
	void (*mmio_barrier)(vc_os_handle_t context);

	/* Monotonic timing used for bounded waits and hardware timeouts. */
	vc_u64 (*time_monotonic_ns)(vc_os_handle_t context);
	void (*delay_us)(vc_os_handle_t context, vc_u32 minimum,
			 vc_u32 maximum);

	vc_u64 reserved[8];
};

#endif /* VC_OS_H */
