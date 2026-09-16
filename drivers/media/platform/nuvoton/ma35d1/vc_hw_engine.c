// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Nuvoton Technology Corporation.

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/wait.h>

#include "hantro.h"
#include "vc_hw_engine.h"
#include "vc_hw_regs.h"
#include "vc_os_linux.h"

#define VC_DEC_REGISTER_COUNT	60U
#define VC_PP_FIRST_REGISTER	60U
#define VC_PP_REGISTER_COUNT	41U
#define VC_PP_LAST_REGISTER	(VC_PP_FIRST_REGISTER + \
				 VC_PP_REGISTER_COUNT - 1U)
#define VC_ENGINE_REGISTER_COUNT	119U

struct hantro_dev *_vc8k_vpu;
EXPORT_SYMBOL(_vc8k_vpu);

static DECLARE_WAIT_QUEUE_HEAD(vc_dec_wait_queue);
static DECLARE_WAIT_QUEUE_HEAD(vc_pp_wait_queue);
static atomic_t vc_dec_irq = ATOMIC_INIT(0);
static atomic_t vc_pp_irq = ATOMIC_INIT(0);
static u32 vc_engine_registers[VC_ENGINE_REGISTER_COUNT];

static long vc_hw_decoder_push(const struct vc_hw_register_set *set)
{
	u32 index;

	memcpy(vc_engine_registers, set->registers,
	       VC_DEC_REGISTER_COUNT * sizeof(*set->registers));
	vc_engine_registers[2] &= ~0x80U;

	for (index = 2; index < VC_DEC_REGISTER_COUNT; index++)
		vc_os_mmio_write32(index, vc_engine_registers[index]);
	vc_os_mmio_write32(VC_HW_DEC_IRQ_REG,
			   vc_engine_registers[VC_HW_DEC_IRQ_REG]);

	return 0;
}

static long vc_hw_decoder_pull(const struct vc_hw_register_set *set)
{
	u32 index;

	for (index = 0; index < VC_DEC_REGISTER_COUNT; index++)
		vc_engine_registers[index] = vc_os_mmio_read32(index);
	memcpy(set->registers, vc_engine_registers,
	       VC_DEC_REGISTER_COUNT * sizeof(*set->registers));

	return 0;
}

static long vc_hw_post_processor_push(const struct vc_hw_register_set *set)
{
	u32 index;

	memcpy(vc_engine_registers + VC_PP_FIRST_REGISTER,
	       set->registers + VC_PP_FIRST_REGISTER,
	       VC_PP_REGISTER_COUNT * sizeof(*set->registers));

	for (index = VC_PP_FIRST_REGISTER + 1U;
	     index <= VC_PP_LAST_REGISTER; index++)
		vc_os_mmio_write32(index, vc_engine_registers[index]);

	/* MA35D1 uses raster output for the display path. */
	vc_os_mmio_write32(91U, 0U);
	vc_os_mmio_write32(VC_PP_FIRST_REGISTER,
			   vc_engine_registers[VC_PP_FIRST_REGISTER]);

	return 0;
}

static long vc_hw_post_processor_pull(const struct vc_hw_register_set *set)
{
	u32 index;

	for (index = VC_PP_FIRST_REGISTER;
	     index <= VC_PP_LAST_REGISTER; index++) {
		vc_engine_registers[index] = vc_os_mmio_read32(index);
		set->registers[index] = vc_engine_registers[index];
	}

	return 0;
}

static long vc_hw_decoder_wait(const struct vc_hw_register_set *set)
{
	long result;

	result = wait_event_interruptible_timeout(vc_dec_wait_queue,
						  atomic_read(&vc_dec_irq),
						  DEC_TIMEOUT);
	if (result == 0)
		result = wait_event_interruptible_timeout(vc_dec_wait_queue,
							  atomic_read(&vc_dec_irq),
							  1000);
	if (result == 0)
		dev_warn(_vc8k_vpu->dev, "decoder interrupt timeout\n");

	atomic_set(&vc_dec_irq, 0);
	return vc_hw_decoder_pull(set);
}

static long vc_hw_post_processor_wait(const struct vc_hw_register_set *set)
{
	if (!wait_event_interruptible_timeout(vc_pp_wait_queue,
					      atomic_read(&vc_pp_irq),
					      PP_TIMEOUT))
		dev_warn(_vc8k_vpu->dev, "post-processor interrupt timeout\n");

	atomic_set(&vc_pp_irq, 0);
	return vc_hw_post_processor_pull(set);
}

long vc_hw_engine_command(unsigned int command, void *argument)
{
	const struct vc_hw_register_set *set = argument;

	switch (command) {
	case VC_HW_DEC_RESERVE:
	case VC_HW_DEC_RELEASE:
	case VC_HW_PP_RESERVE:
	case VC_HW_PP_RELEASE:
		return 0;
	case VC_HW_DEC_PUSH_REGISTERS:
		atomic_set(&vc_dec_irq, 0);
		return set && set->registers ? vc_hw_decoder_push(set) : -EINVAL;
	case VC_HW_PP_PUSH_REGISTERS:
		atomic_set(&vc_pp_irq, 0);
		return set && set->registers ?
			vc_hw_post_processor_push(set) : -EINVAL;
	case VC_HW_DEC_PULL_REGISTERS:
		return set && set->registers ? vc_hw_decoder_pull(set) : -EINVAL;
	case VC_HW_PP_PULL_REGISTERS:
		return set && set->registers ?
			vc_hw_post_processor_pull(set) : -EINVAL;
	case VC_HW_DEC_WAIT:
		return set && set->registers ? vc_hw_decoder_wait(set) : -EINVAL;
	case VC_HW_PP_WAIT:
		return set && set->registers ?
			vc_hw_post_processor_wait(set) : -EINVAL;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL(vc_hw_engine_command);

static void vc_hw_engine_reset(void)
{
	u32 index;
	u32 status = vc_os_mmio_read32(VC_HW_DEC_IRQ_REG);

	if (status & VC_HW_DEC_ENABLE)
		vc_os_mmio_write32(VC_HW_DEC_IRQ_REG,
				   VC_HW_DEC_ABORT | VC_HW_DEC_IRQ_DISABLE);

	for (index = 1; index < VC_ENGINE_REGISTER_COUNT; index++)
		vc_os_mmio_write32(index, 0U);
}

int vc_hw_engine_init(struct hantro_dev *vpu)
{
	if (!vpu)
		return -EINVAL;

	_vc8k_vpu = vpu;
	atomic_set(&vc_dec_irq, 0);
	atomic_set(&vc_pp_irq, 0);
	vc_hw_engine_reset();

	return 0;
}
EXPORT_SYMBOL(vc_hw_engine_init);

void vc_hw_engine_irq(void)
{
	bool handled = false;

	if (vc_os_mmio_read32(VC_HW_DEC_IRQ_REG) &
	    VC_HW_DEC_IRQ_PENDING) {
		atomic_set(&vc_dec_irq, 1);
		wake_up_interruptible_all(&vc_dec_wait_queue);
		handled = true;
	}

	if (vc_os_mmio_read32(VC_HW_PP_IRQ_REG) &
	    VC_HW_PP_IRQ_PENDING) {
		atomic_set(&vc_pp_irq, 1);
		wake_up_interruptible_all(&vc_pp_wait_queue);
		handled = true;
	}

	if (!handled)
		dev_dbg(_vc8k_vpu->dev, "VC8000 interrupt had no engine status\n");
}
EXPORT_SYMBOL(vc_hw_engine_irq);
