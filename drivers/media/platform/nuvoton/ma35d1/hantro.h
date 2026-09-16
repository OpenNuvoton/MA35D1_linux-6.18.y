/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hantro VPU codec driver Nuvoton MA35D1 VC8000
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 *
 * Based on hantro driver by Google LLC. Tomasz Figa <tfiga@chromium.org>
 *
 */

#ifndef HANTRO_H_
#define HANTRO_H_

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/smp.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "hantro_hw.h"

#define NVT_PORT

#define VC8K_MAX_WIDTH			1920
#define VC8K_MAX_HEIGHT			1920

#define VC8000_USE_IRQ

#define PA_BLK_SIZE			0x10000

#define H264_MB_DIM			16
#define H264_MB_WIDTH(w)		DIV_ROUND_UP(w, H264_MB_DIM)
#define H264_MB_HEIGHT(h)		DIV_ROUND_UP(h, H264_MB_DIM)

#define JPEG_MB_DIM			16
#define JPEG_MB_WIDTH(w)		DIV_ROUND_UP(w, JPEG_MB_DIM)
#define JPEG_MB_HEIGHT(h)		DIV_ROUND_UP(h, JPEG_MB_DIM)

struct hantro_ctx;
struct hantro_codec_ops;

#define HANTRO_JPEG_ENCODER	BIT(0)
#define HANTRO_ENCODERS		0x0000ffff
#define HANTRO_MPEG2_DECODER	BIT(16)
#define HANTRO_VP8_DECODER	BIT(17)
#define HANTRO_H264_DECODER	BIT(18)
#define HANTRO_JPEG_DECODER	BIT(19)	
#define HANTRO_DECODERS		0xffff0000


/*
 *  PP parameters of H264/JPEG context
 */
struct vc8k_pp_params {
	int   enable_pp;
	u32   frame_buf_paddr;       /* physical address of frame buffer          */
	int   frame_buff_size;
	int   frame_buf_w;           /* width of frame buffer width               */
	int   frame_buf_h;           /* height of frame buffer                    */
	int   img_out_x;             /* image original point(x,y) on frame buffer */
	int   img_out_y;             /* image original point(x,y) on frame buffer */
	int   img_out_w;             /* image output width on frame buffer        */
	int   img_out_h;             /* image output height on frame buffer       */
	int   img_out_fmt;           /* image output format                       */
	int   rotation;
	int   pp_out_dst;            /* PP output destination.                    */
				     /* 0: fb0                                    */
				     /* 1: fb1                                    */
				     /* otherwise: frame_buf_paddr                */
	int   libjpeg_mode;          /* 0: v4l2-only; 1: libjpeg+v4l2             */
	int   resserved[8];
};

#define VC8KIOC_PP_SET_CONFIG	_IOW ('v', 91, struct vc8k_pp_params)
#define VC8KIOC_PP_GET_CONFIG	_IOW ('v', 92, struct vc8k_pp_params)

/*
 *  VC8000 device & driver configuration
 */
struct vc8k_config {
        struct vc8k_pp_params  ppc;  /* default PP config                     */
	int   fb_width;              /* frame buffer width from devcie tree   */
	int   fb_height;             /* frame buffer height from device tree  */
	int   fb_fmt;                /* frame buffer color format             */
	int   pp_wait_vsync;         /* wait VSYNC when PP enabled */
	int   have_res_mem;          /* has reserved memory or not            */
	int   use_dev_coherent;      /* use device coherent memory            */
	int   debug_level;           /* debug message control 0: no message */
        phys_addr_t  res_mem_base;
        char __iomem *res_mem_virt;
        size_t  res_mem_size;
};


/**
 * struct hantro_irq - irq handler and name
 *
 * @name:			irq name for device tree lookup
 * @handler:			interrupt handler
 */
struct hantro_irq {
	const char *name;
	irqreturn_t (*handler)(int irq, void *priv);
};

/**
 * struct hantro_variant - information about VPU hardware variant
 *
 * @enc_offset:			Offset from VPU base to encoder registers.
 * @dec_offset:			Offset from VPU base to decoder registers.
 * @enc_fmts:			Encoder formats.
 * @num_enc_fmts:		Number of encoder formats.
 * @dec_fmts:			Decoder formats.
 * @num_dec_fmts:		Number of decoder formats.
 * @codec:			Supported codecs
 * @codec_ops:			Codec ops.
 * @init:			Initialize hardware.
 * @runtime_resume:		reenable hardware after power gating
 * @irqs:			array of irq names and interrupt handlers
 * @num_irqs:			number of irqs in the array
 * @clk_names:			array of clock names
 * @num_clocks:			number of clocks in the array
 * @reg_names:			array of register range names
 * @num_regs:			number of register range names in the array
 */
struct hantro_variant {
	unsigned int enc_offset;
	unsigned int dec_offset;
	const struct hantro_fmt *enc_fmts;
	unsigned int num_enc_fmts;
	const struct hantro_fmt *dec_fmts;
	unsigned int num_dec_fmts;
	unsigned int codec;
	const struct hantro_codec_ops *codec_ops;
	int (*init)(struct hantro_dev *vpu);
	int (*runtime_resume)(struct hantro_dev *vpu);
	const struct hantro_irq *irqs;
	int num_irqs;
	const char * const *clk_names;
	int num_clocks;
	const char * const *reg_names;
	int num_regs;
};

/**
 * enum hantro_codec_mode - codec operating mode.
 * @HANTRO_MODE_NONE:  No operating mode. Used for RAW video formats.
 * @HANTRO_MODE_JPEG_ENC: JPEG encoder.
 * @HANTRO_MODE_H264_DEC: H264 decoder.
 */
enum hantro_codec_mode {
	HANTRO_MODE_NONE = -1,
	HANTRO_MODE_JPEG_DEC,
	HANTRO_MODE_H264_DEC,
	HANTRO_MODE_PP,
};

/*
 * struct hantro_ctrl - helper type to declare supported controls
 * @codec:	codec id this control belong to (HANTRO_JPEG_ENCODER, etc.)
 * @cfg:	control configuration
 */
struct hantro_ctrl {
	unsigned int codec;
	struct v4l2_ctrl_config cfg;
};

/*
 * struct hantro_func - Hantro VPU functionality
 *
 * @id:			processing functionality ID (can be
 *			%MEDIA_ENT_F_PROC_VIDEO_ENCODER or
 *			%MEDIA_ENT_F_PROC_VIDEO_DECODER)
 * @vdev:		&struct video_device that exposes the encoder or
 *			decoder functionality
 * @source_pad:		&struct media_pad with the source pad.
 * @sink:		&struct media_entity pointer with the sink entity
 * @sink_pad:		&struct media_pad with the sink pad.
 * @proc:		&struct media_entity pointer with the M2M device itself.
 * @proc_pads:		&struct media_pad with the @proc pads.
 * @intf_devnode:	&struct media_intf devnode pointer with the interface
 *			with controls the M2M device.
 *
 * Contains everything needed to attach the video device to the media device.
 */
struct hantro_func {
	unsigned int id;
	struct video_device vdev;
	struct media_pad source_pad;
	struct media_entity sink;
	struct media_pad sink_pad;
	struct media_entity proc;
	struct media_pad proc_pads[2];
	struct media_intf_devnode *intf_devnode;
};

static inline struct hantro_func *
hantro_vdev_to_func(struct video_device *vdev)
{
	return container_of(vdev, struct hantro_func, vdev);
}

/**
 * struct hantro_dev - driver data
 * @v4l2_dev:		V4L2 device to register video devices for.
 * @m2m_dev:		mem2mem device associated to this device.
 * @mdev:		media device associated to this device.
 * @encoder:		encoder functionality.
 * @decoder:		decoder functionality.
 * @pdev:		Pointer to VPU platform device.
 * @dev:		Pointer to device for convenient logging using
 *			dev_ macros.
 * @clocks:		Array of clock handles.
 * @reg_bases:		Mapped addresses of VPU registers.
 * @enc_base:		Mapped address of VPU encoder register for convenience.
 * @dec_base:		Mapped address of VPU decoder register for convenience.
 * @ctrl_base:		Mapped address of VPU control block.
 * @vpu_mutex:		Mutex to synchronize V4L2 calls.
 * @irqlock:		Spinlock to synchronize access to data structures
 *			shared with interrupt handlers.
 * @variant:		Hardware variant-specific parameters.
 * @watchdog_work:	Delayed work for hardware timeout handling.
 */
struct hantro_dev {
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_dev;
	struct media_device mdev;
	struct hantro_func *encoder;
	struct hantro_func *decoder;
	struct platform_device *pdev;
	struct device *dev;
	struct clk_bulk_data *clocks;
	struct clk *clock;
	struct reset_control *reset;
	void __iomem **reg_bases;
	void __iomem *enc_base;
	void __iomem *dec_base;
	void __iomem *ctrl_base;
	void __iomem *dcultra_base;

	struct mutex  lock;
	struct vc8k_config  vc8k_cfg;
	struct mutex vpu_mutex;	/* video_device lock */
	spinlock_t irqlock;
	const struct hantro_variant *variant;
	u32   dec_state;
	struct delayed_work watchdog_work;
};

/**
 * struct hantro_ctx - Context (instance) private data.
 *
 * @dev:		VPU driver data to which the context belongs.
 * @fh:			V4L2 file handler.
 *
 * @sequence_cap:       Sequence counter for capture queue
 * @sequence_out:       Sequence counter for output queue
 *
 * @vpu_src_fmt:	Descriptor of active source format.
 * @src_fmt:		V4L2 pixel format of active source format.
 * @vpu_dst_fmt:	Descriptor of active destination format.
 * @dst_fmt:		V4L2 pixel format of active destination format.
 *
 * @ctrl_handler:	Control handler used to register controls.
 * @jpeg_quality:	User-specified JPEG compression quality.
 *
 * @buf_finish:		Buffer finish. This depends on encoder or decoder
 *			context, and it's called right before
 *			calling v4l2_m2m_job_finish.
 * @codec_ops:		Set of operations related to codec mode.
 * @jpeg_enc:		JPEG-encoding context.
 * @mpeg2_dec:		MPEG-2-decoding context.
 * @vp8_dec:		VP8-decoding context.
 */
struct hantro_ctx {
	struct hantro_dev *dev;
	struct v4l2_fh fh;

	u32 sequence_cap;
	u32 sequence_out;

	int   vc8k_err;
	void  *vc8k_data;
	struct vc8k_pp_params  pp_ctx;
	int   is_pp_inited;
	int   pp_changed;

	const struct hantro_fmt *vpu_src_fmt;
	struct v4l2_pix_format_mplane src_fmt;
	const struct hantro_fmt *vpu_dst_fmt;
	struct v4l2_pix_format_mplane dst_fmt;

	struct v4l2_ctrl_handler ctrl_handler;
	int jpeg_quality;

	int (*buf_finish)(struct hantro_ctx *ctx,
			  struct vb2_buffer *buf,
			  unsigned int bytesused);

	const struct hantro_codec_ops *codec_ops;

	/* Specific for particular codec modes. */
	union {
		struct hantro_h264_dec_hw_ctx h264_dec;
		struct hantro_jpeg_enc_hw_ctx jpeg_enc;
		struct hantro_mpeg2_dec_hw_ctx mpeg2_dec;
		struct hantro_vp8_dec_hw_ctx vp8_dec;
	};
	int  process_id;
	struct list_head list;
};

/**
 * struct hantro_fmt - information about supported video formats.
 * @name:	Human readable name of the format.
 * @fourcc:	FourCC code of the format. See V4L2_PIX_FMT_*.
 * @codec_mode:	Codec mode related to this format. See
 *		enum hantro_codec_mode.
 * @header_size: Optional header size. Currently used by JPEG encoder.
 * @max_depth:	Maximum depth, for bitstream formats
 * @enc_fmt:	Format identifier for encoder registers.
 * @frmsize:	Supported range of frame sizes (only for bitstream formats).
 */
struct hantro_fmt {
	char *name;
	u32 fourcc;
	enum hantro_codec_mode codec_mode;
	int header_size;
	int max_depth;
	enum hantro_enc_fmt enc_fmt;
	struct v4l2_frmsize_stepwise frmsize;
};

struct hantro_reg {
	u32 base;
	u32 shift;
	u32 mask;
};


/* Logging helpers */

/**
 * debug - Module parameter to control level of debugging messages.
 *
 * Level of debugging messages can be controlled by bits of
 * module parameter called "debug". Meaning of particular
 * bits is as follows:
 *
 * bit 0 - global information: mode, size, init, release
 * bit 1 - each run start/result information
 * bit 2 - contents of small controls from userspace
 * bit 3 - contents of big controls from userspace
 * bit 4 - detail fmt, ctrl, buffer q/dq information
 * bit 5 - detail function enter/leave trace information
 * bit 6 - register write/read information
 */
extern int hantro_debug;

/* Structure access helpers. */
static inline struct hantro_ctx *file_to_ctx(struct file *filp)
{
	return container_of(file_to_v4l2_fh(filp), struct hantro_ctx, fh);
}

/* Register accessors. */
static inline void vepu_write_relaxed(struct hantro_dev *vpu,
				      u32 val, u32 reg)
{
	writel_relaxed(val, vpu->enc_base + reg);
}

static inline void vepu_write(struct hantro_dev *vpu, u32 val, u32 reg)
{
	writel(val, vpu->enc_base + reg);
}

static inline u32 vepu_read(struct hantro_dev *vpu, u32 reg)
{
	u32 val = readl(vpu->enc_base + reg);
	return val;
}

static inline void vdpu_write_relaxed(struct hantro_dev *vpu,
				      u32 val, u32 reg)
{
	writel_relaxed(val, vpu->dec_base + reg);
}

static inline void vdpu_write(struct hantro_dev *vpu, u32 val, u32 reg)
{
	writel(val, vpu->dec_base + reg);
}

static inline u32 vdpu_read(struct hantro_dev *vpu, u32 reg)
{
	u32 val = readl(vpu->dec_base + reg);

	return val;
}

static inline void hantro_reg_write(struct hantro_dev *vpu,
				    const struct hantro_reg *reg,
				    u32 val)
{
	u32 v;

	v = vdpu_read(vpu, reg->base);
	v &= ~(reg->mask << reg->shift);
	v |= ((val & reg->mask) << reg->shift);
	vdpu_write_relaxed(vpu, v, reg->base);
}

bool hantro_is_encoder_ctx(const struct hantro_ctx *ctx);

struct hantro_ctx *get_hantro_context_by_pid(struct hantro_ctx *ctx);

static inline struct vb2_v4l2_buffer *
hantro_get_src_buf(struct hantro_ctx *ctx)
{
	return v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
}

static inline struct vb2_v4l2_buffer *
hantro_get_dst_buf(struct hantro_ctx *ctx)
{
	return v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
}

extern struct hantro_dev *_vc8k_vpu;

static inline void vc8k_write_swreg(u32 val, u32 swreg)
{
	writel_relaxed(val, _vc8k_vpu->dec_base + (swreg * 4));
}

static inline u32 vc8k_read_swreg(u32 swreg)
{
	return readl_relaxed(_vc8k_vpu->dec_base + (swreg * 4));
}

extern int  ma35d1_vc8k_init(struct hantro_dev *vpu);
extern void ma35d1_vc8k_exit(struct hantro_dev *vpu);

extern int VC8K_PreAllocDMABuff(struct device *dev, struct hantro_dev *vpu);
extern void VC8K_ReleaseDMABuff(struct device *dev);

extern int  ma35d1_h264_dec_init(struct hantro_ctx *ctx);
extern int  ma35d1_h264_dec_run(struct hantro_ctx *ctx);
extern void ma35d1_h264_dec_exit(struct hantro_ctx *ctx);

extern int ma35d1_jpeg_dec_init(struct hantro_ctx *ctx);
extern int ma35d1_jpeg_dec_run(struct hantro_ctx *ctx);
extern void ma35d1_jpeg_dec_exit(struct hantro_ctx *ctx);

extern int ma35d1_pp_init(struct hantro_ctx *ctx);
extern int ma35d1_pp_run(struct hantro_ctx *ctx);

extern int vc_hw_engine_init(struct hantro_dev *vpu);
extern long vc_hw_engine_command(unsigned int command, void *argument);
extern void vc_hw_engine_irq(void);


#endif /* HANTRO_H_ */
