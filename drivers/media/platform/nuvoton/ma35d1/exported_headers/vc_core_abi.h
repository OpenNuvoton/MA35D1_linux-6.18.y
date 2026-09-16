/* SPDX-License-Identifier: MIT */
/*
 * Future binary ABI between the proprietary VC8000 core and an open shim.
 *
 * This header contains declarations only and is independent of Linux.
 */

#ifndef VC_CORE_ABI_H
#define VC_CORE_ABI_H

#include "vc_os.h"

#define VC_CORE_ABI_MAJOR 1U
#define VC_CORE_ABI_MINOR 0U

/* Opaque core-instance token; it is never a kernel or userspace pointer. */
typedef vc_u64 vc_core_handle_t;

struct vc_core_info {
	vc_u32 struct_size;
	vc_u32 abi_major;
	vc_u32 abi_minor;
	vc_u32 build_id;
	vc_u64 required_os_capabilities;
	vc_u64 feature_bits;
	vc_u64 reserved[4];
};

enum vc_status vc_core_query(struct vc_core_info *info);

enum vc_status vc_core_bind_os(const struct vc_os_ops *ops,
			       vc_u32 ops_size);

enum vc_status vc_core_create(const void *parameters,
			      vc_u32 parameters_size,
			      vc_core_handle_t *core);

enum vc_status vc_core_submit(vc_core_handle_t core, const void *job,
			      vc_u32 job_size, void *result,
			      vc_u32 result_size);

enum vc_status vc_core_flush(vc_core_handle_t core, vc_u32 flags);

void vc_core_destroy(vc_core_handle_t core);

#endif /* VC_CORE_ABI_H */
