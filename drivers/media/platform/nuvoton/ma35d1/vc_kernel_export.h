/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Nuvoton Technology Corporation. */
/*
 * Linux mapping for proprietary-core portability annotations.
 *
 * This header is force-included by the Linux build. Standalone core builds
 * use the no-op fallback from vc_legacy_types.h and contain no kernel export metadata.
 */

#ifndef VC_KERNEL_EXPORT_H
#define VC_KERNEL_EXPORT_H

#include <linux/export.h>

#define VC_EXPORT_SYMBOL(symbol) EXPORT_SYMBOL(symbol)

#endif /* VC_KERNEL_EXPORT_H */
