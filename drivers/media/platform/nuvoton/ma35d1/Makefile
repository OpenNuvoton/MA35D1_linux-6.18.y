# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for Nuvoton MA35D1 VC8K driver
#

ccflags-y := -fno-stack-protector \
	     -I $(srctree)/drivers/media/platform/nuvoton/ma35d1 \
	     -I $(srctree)/drivers/media/platform/nuvoton/ma35d1/exported_headers

ccflags-y += -I $(src)

vc8000-core-blob := $(src)/blob/vc8000_core.o_shipped

ma35-vc8000-objs += \
		hantro_drv.o \
		hantro_v4l2.o \
		hantro_jpeg.o \
		ma35d1_vpu_hw.o \
		ma35d1_h264_glue.o \
		ma35d1_jpeg_glue.o \
		ma35d1_pp_glue.o \
		vc_dwl_control.o \
		vc_dwl_hw.o \
		vc_dwl_memory.o \
		vc_dwl_caps.o \
		vc_hw_engine.o \
		vc_os_linux.o \
		vc8000_core_link.o

quiet_cmd_vc8000_core = COPY    $@
      cmd_vc8000_core = cp $< $@

targets += vc8000_core_link.o
always-y += vc8000_core_link.o

$(obj)/vc8000_core_link.o: $(vc8000-core-blob) FORCE
	$(call if_changed,vc8000_core)

obj-$(CONFIG_VIDEO_NUVOTON_VC8K) += ma35-vc8000.o
