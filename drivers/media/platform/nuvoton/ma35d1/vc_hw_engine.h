/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2026 Nuvoton Technology Corporation. */

#ifndef VC_HW_ENGINE_H
#define VC_HW_ENGINE_H

#include "vc_legacy_types.h"

struct hantro_dev;

struct vc_hw_register_set {
	u32 core_id;
	u32 *registers;
	u32 size;
};

enum vc_hw_engine_command {
	VC_HW_DEC_PUSH_REGISTERS = 1,
	VC_HW_PP_PUSH_REGISTERS,
	VC_HW_DEC_RESERVE,
	VC_HW_DEC_RELEASE,
	VC_HW_PP_RESERVE,
	VC_HW_PP_RELEASE,
	VC_HW_DEC_WAIT,
	VC_HW_PP_WAIT,
	VC_HW_DEC_PULL_REGISTERS,
	VC_HW_PP_PULL_REGISTERS,
};

int vc_hw_engine_init(struct hantro_dev *vpu);
long vc_hw_engine_command(unsigned int command, void *argument);
void vc_hw_engine_irq(void);

#endif /* VC_HW_ENGINE_H */
