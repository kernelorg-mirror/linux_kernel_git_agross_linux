/*
 * Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2016 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <media/videobuf2-dma-sg.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ctrls.h>

#include "core.h"
#include "helpers.h"
#include "load.h"
#include "venc_ctrls.h"

#define NUM_B_FRAMES_MAX	4

static u32 get_framesize_nv12(int plane, u32 height, u32 width)
{
	u32 y_stride, uv_stride, y_plane;
	u32 y_sclines, uv_sclines, uv_plane;
	u32 size;

	y_stride = ALIGN(width, 128);
	uv_stride = ALIGN(width, 128);
	y_sclines = ALIGN(height, 32);
	uv_sclines = ALIGN(((height + 1) >> 1), 16);

	y_plane = y_stride * y_sclines;
	uv_plane = uv_stride * uv_sclines + SZ_4K;
	size = y_plane + uv_plane + SZ_8K;
	size = ALIGN(size, SZ_4K);

	return size;
}

static u32 get_framesize_compressed(u32 height, u32 width)
{
	u32 sz = ALIGN(height, 32) * ALIGN(width, 32) * 3 / 2;

	return ALIGN(sz, SZ_4K);
}

static const struct vidc_format venc_formats[] = {
	{
		.pixfmt = V4L2_PIX_FMT_NV12,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG4,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_H263,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_H264,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_VP8,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	},
};

static const struct vidc_format *find_format(u32 pixfmt, int type)
{
	const struct vidc_format *fmt = venc_formats;
	unsigned int size = ARRAY_SIZE(venc_formats);
	unsigned int i;

	for (i = 0; i < size; i++) {
		if (fmt[i].pixfmt == pixfmt)
			break;
	}

	if (i == size || fmt[i].type != type)
		return NULL;

	return &fmt[i];
}

static const struct vidc_format *find_format_by_index(int index, int type)
{
	const struct vidc_format *fmt = venc_formats;
	unsigned int size = ARRAY_SIZE(venc_formats);
	int i, k = 0;

	if (index < 0 || index > size)
		return NULL;

	for (i = 0; i < size; i++) {
		if (fmt[i].type != type)
			continue;
		if (k == index)
			break;
		k++;
	}

	if (i == size)
		return NULL;

	return &fmt[i];
}

static int venc_v4l2_to_hfi(int id, int value)
{
	switch (id) {
	case V4L2_CID_MPEG_VIDEO_MPEG4_LEVEL:
		switch (value) {
		case V4L2_MPEG_VIDEO_MPEG4_LEVEL_0:
		default:
			return HFI_MPEG4_LEVEL_0;
		case V4L2_MPEG_VIDEO_MPEG4_LEVEL_0B:
			return HFI_MPEG4_LEVEL_0b;
		case V4L2_MPEG_VIDEO_MPEG4_LEVEL_1:
			return HFI_MPEG4_LEVEL_1;
		case V4L2_MPEG_VIDEO_MPEG4_LEVEL_2:
			return HFI_MPEG4_LEVEL_2;
		case V4L2_MPEG_VIDEO_MPEG4_LEVEL_3:
			return HFI_MPEG4_LEVEL_3;
		case V4L2_MPEG_VIDEO_MPEG4_LEVEL_4:
			return HFI_MPEG4_LEVEL_4;
		case V4L2_MPEG_VIDEO_MPEG4_LEVEL_5:
			return HFI_MPEG4_LEVEL_5;
		}
	case V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE:
		switch (value) {
		case V4L2_MPEG_VIDEO_MPEG4_PROFILE_SIMPLE:
		default:
			return HFI_MPEG4_PROFILE_SIMPLE;
		case V4L2_MPEG_VIDEO_MPEG4_PROFILE_ADVANCED_SIMPLE:
			return HFI_MPEG4_PROFILE_ADVANCEDSIMPLE;
		}
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		switch (value) {
		case V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE:
			return HFI_H264_PROFILE_BASELINE;
		case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
			return HFI_H264_PROFILE_CONSTRAINED_BASE;
		case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
			return HFI_H264_PROFILE_MAIN;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
		default:
			return HFI_H264_PROFILE_HIGH;
		}
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		switch (value) {
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:
			return HFI_H264_LEVEL_1;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1B:
			return HFI_H264_LEVEL_1b;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:
			return HFI_H264_LEVEL_11;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:
			return HFI_H264_LEVEL_12;
		case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:
			return HFI_H264_LEVEL_13;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:
			return HFI_H264_LEVEL_2;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:
			return HFI_H264_LEVEL_21;
		case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:
			return HFI_H264_LEVEL_22;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:
			return HFI_H264_LEVEL_3;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
			return HFI_H264_LEVEL_31;
		case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:
			return HFI_H264_LEVEL_32;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
			return HFI_H264_LEVEL_4;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:
			return HFI_H264_LEVEL_41;
		case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
			return HFI_H264_LEVEL_42;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_0:
		default:
			return HFI_H264_LEVEL_5;
		case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
			return HFI_H264_LEVEL_51;
		}
	case V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE:
		switch (value) {
		case V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC:
		default:
			return HFI_H264_ENTROPY_CAVLC;
		case V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC:
			return HFI_H264_ENTROPY_CABAC;
		}
	case V4L2_CID_MPEG_VIDEO_VPX_PROFILE:
		switch (value) {
		case 0:
		default:
			return HFI_VPX_PROFILE_VERSION_0;
		case 1:
			return HFI_VPX_PROFILE_VERSION_1;
		case 2:
			return HFI_VPX_PROFILE_VERSION_2;
		case 3:
			return HFI_VPX_PROFILE_VERSION_3;
		}
	}

	return 0;
}

static int venc_set_properties(struct vidc_inst *inst)
{
	struct venc_controls *ctr = &inst->controls.enc;
	struct hfi_core *hfi = &inst->core->hfi;
	struct hfi_intra_period intra_period;
	struct hfi_profile_level pl;
	struct hfi_framerate frate;
	struct hfi_bitrate brate;
	struct hfi_idr_period idrp;
	u32 ptype, rate_control, bitrate, profile = 0, level = 0;
	int ret;

	ptype = HFI_PROPERTY_CONFIG_FRAME_RATE;
	frate.buffer_type = HFI_BUFFER_OUTPUT;
	frate.framerate = inst->fps * (1 << 16);

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &frate);
	if (ret)
		return ret;

	if (inst->fmt_cap->pixfmt == V4L2_PIX_FMT_H264) {
		struct hfi_h264_vui_timing_info info;

		ptype = HFI_PROPERTY_PARAM_VENC_H264_VUI_TIMING_INFO;
		info.enable = 1;
		info.fixed_framerate = 1;
		info.time_scale = NSEC_PER_SEC;

		ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype,
						    &info);
		if (ret)
			return ret;
	}

	ptype = HFI_PROPERTY_CONFIG_VENC_IDR_PERIOD;
	idrp.idr_period = ctr->gop_size;
	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &idrp);
	if (ret)
		return ret;

	if (ctr->num_b_frames) {
		u32 max_num_b_frames = NUM_B_FRAMES_MAX;

		ptype = HFI_PROPERTY_PARAM_VENC_MAX_NUM_B_FRAMES;
		ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst,
						    ptype, &max_num_b_frames);
		if (ret)
			return ret;
	}

	/* intra_period = pframes + bframes + 1 */
	if (!ctr->num_p_frames)
		ctr->num_p_frames = 2 * 15 - 1,

	ptype = HFI_PROPERTY_CONFIG_VENC_INTRA_PERIOD;
	intra_period.pframes = ctr->num_p_frames;
	intra_period.bframes = ctr->num_b_frames;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype,
					    &intra_period);
	if (ret)
		return ret;

	if (ctr->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)
		rate_control = HFI_RATE_CONTROL_VBR_CFR;
	else
		rate_control = HFI_RATE_CONTROL_CBR_CFR;

	ptype = HFI_PROPERTY_PARAM_VENC_RATE_CONTROL;
	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype,
					    &rate_control);
	if (ret)
		return ret;

	if (!ctr->bitrate)
		bitrate = 64000;
	else
		bitrate = ctr->bitrate;

	ptype = HFI_PROPERTY_CONFIG_VENC_TARGET_BITRATE;
	brate.bitrate = bitrate;
	brate.layer_id = 0;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &brate);
	if (ret)
		return ret;

	if (!ctr->bitrate_peak)
		bitrate *= 2;
	else
		bitrate = ctr->bitrate_peak;

	ptype = HFI_PROPERTY_CONFIG_VENC_MAX_BITRATE;
	brate.bitrate = bitrate;
	brate.layer_id = 0;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &brate);
	if (ret)
		return ret;

	if (inst->fmt_cap->pixfmt == V4L2_PIX_FMT_H264) {
		profile = venc_v4l2_to_hfi(V4L2_CID_MPEG_VIDEO_H264_PROFILE,
					   ctr->profile);
		level = venc_v4l2_to_hfi(V4L2_CID_MPEG_VIDEO_H264_LEVEL,
					 ctr->level);
	} else if (inst->fmt_cap->pixfmt == V4L2_PIX_FMT_VP8) {
		profile = venc_v4l2_to_hfi(V4L2_CID_MPEG_VIDEO_VPX_PROFILE,
					   ctr->profile);
		level = 0;
	} else if (inst->fmt_cap->pixfmt == V4L2_PIX_FMT_MPEG4) {
		profile = venc_v4l2_to_hfi(V4L2_CID_MPEG_VIDEO_MPEG4_PROFILE,
					   ctr->profile);
		level = venc_v4l2_to_hfi(V4L2_CID_MPEG_VIDEO_MPEG4_LEVEL,
					 ctr->level);
	} else if (inst->fmt_cap->pixfmt == V4L2_PIX_FMT_H263) {
		profile = 0;
		level = 0;
	}

	ptype = HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT;
	pl.profile = profile;
	pl.level = level;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &pl);
	if (ret)
		return ret;

	return 0;
}

static int
venc_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strlcpy(cap->driver, VIDC_DRV_NAME, sizeof(cap->driver));
	strlcpy(cap->card, "video encoder", sizeof(cap->card));
	strlcpy(cap->bus_info, "platform:vidc", sizeof(cap->bus_info));

	cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int venc_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	const struct vidc_format *fmt;

	fmt = find_format_by_index(f->index, f->type);

	memset(f->reserved, 0, sizeof(f->reserved));

	if (!fmt)
		return -EINVAL;

	f->pixelformat = fmt->pixfmt;

	return 0;
}

static const struct vidc_format *
venc_try_fmt_common(struct vidc_inst *inst, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;
	struct v4l2_plane_pix_format *pfmt = pixmp->plane_fmt;
	struct hfi_inst *hfi_inst = inst->hfi_inst;
	const struct vidc_format *fmt;
	unsigned int p;

	memset(pfmt[0].reserved, 0, sizeof(pfmt[0].reserved));
	memset(pixmp->reserved, 0, sizeof(pixmp->reserved));

	fmt = find_format(pixmp->pixelformat, f->type);
	if (!fmt) {
		if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			pixmp->pixelformat = V4L2_PIX_FMT_H264;
		else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
			pixmp->pixelformat = V4L2_PIX_FMT_NV12;
		else
			return NULL;
		fmt = find_format(pixmp->pixelformat, f->type);
		pixmp->width = 1280;
		pixmp->height = 720;
	}

	pixmp->height = ALIGN(pixmp->height, 32);

	pixmp->width = clamp(pixmp->width, hfi_inst->width.min,
			     hfi_inst->width.max);
	pixmp->height = clamp(pixmp->height, hfi_inst->height.min,
			      hfi_inst->height.max);
	if (pixmp->field == V4L2_FIELD_ANY)
		pixmp->field = V4L2_FIELD_NONE;
	pixmp->num_planes = fmt->num_planes;
	pixmp->flags = 0;

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		for (p = 0; p < pixmp->num_planes; p++) {
			pfmt[p].sizeimage = get_framesize_nv12(p, pixmp->height,
							       pixmp->width);

			pfmt[p].bytesperline = ALIGN(pixmp->width, 128);
		}
	} else {
		pfmt[0].sizeimage = get_framesize_compressed(pixmp->height,
							     pixmp->width);
		pfmt[0].bytesperline = 0;
	}

	return fmt;
}

static int venc_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vidc_inst *inst = to_inst(file);
	const struct vidc_format *fmt;

	fmt = venc_try_fmt_common(inst, f);
	if (!fmt)
		return -EINVAL;

	return 0;
}

static int venc_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vidc_inst *inst = to_inst(file);
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;
	struct v4l2_pix_format_mplane orig_pixmp;
	const struct vidc_format *fmt;
	struct v4l2_format format;
	u32 pixfmt_out = 0, pixfmt_cap = 0;

	orig_pixmp = *pixmp;

	fmt = venc_try_fmt_common(inst, f);
	if (!fmt)
		return -EINVAL;

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		pixfmt_out = pixmp->pixelformat;
		pixfmt_cap = inst->fmt_cap->pixfmt;
	} else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		pixfmt_cap = pixmp->pixelformat;
		pixfmt_out = inst->fmt_out->pixfmt;
	}

	memset(&format, 0, sizeof(format));

	format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	format.fmt.pix_mp.pixelformat = pixfmt_out;
	format.fmt.pix_mp.width = orig_pixmp.width;
	format.fmt.pix_mp.height = orig_pixmp.height;
	venc_try_fmt_common(inst, &format);
	inst->out_width = format.fmt.pix_mp.width;
	inst->out_height = format.fmt.pix_mp.height;
	inst->colorspace = pixmp->colorspace;
	inst->ycbcr_enc = pixmp->ycbcr_enc;
	inst->quantization = pixmp->quantization;
	inst->xfer_func = pixmp->xfer_func;

	memset(&format, 0, sizeof(format));

	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	format.fmt.pix_mp.pixelformat = pixfmt_cap;
	format.fmt.pix_mp.width = orig_pixmp.width;
	format.fmt.pix_mp.height = orig_pixmp.height;
	venc_try_fmt_common(inst, &format);
	inst->width = format.fmt.pix_mp.width;
	inst->height = format.fmt.pix_mp.height;

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		inst->fmt_out = fmt;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		inst->fmt_cap = fmt;

	return 0;
}

static int venc_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;
	struct vidc_inst *inst = to_inst(file);
	const struct vidc_format *fmt;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		fmt = inst->fmt_cap;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = inst->fmt_out;
	else
		return -EINVAL;

	pixmp->pixelformat = fmt->pixfmt;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		pixmp->width = inst->width;
		pixmp->height = inst->height;
		pixmp->colorspace = inst->colorspace;
		pixmp->ycbcr_enc = inst->ycbcr_enc;
		pixmp->quantization = inst->quantization;
		pixmp->xfer_func = inst->xfer_func;
	} else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		pixmp->width = inst->out_width;
		pixmp->height = inst->out_height;
	}

	venc_try_fmt_common(inst, f);

	return 0;
}

static int venc_g_selection(struct file *file, void *fh,
			    struct v4l2_selection *s)
{
	struct vidc_inst *inst = to_inst(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		s->r.top = 0;
		s->r.left = 0;
		s->r.width = inst->width;
		s->r.height = inst->height;
		break;
	case V4L2_SEL_TGT_CROP:
		s->r.top = 0;
		s->r.left = 0;
		s->r.width = inst->out_width;
		s->r.height = inst->out_height;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int venc_s_selection(struct file *file, void *fh,
			    struct v4l2_selection *s)
{
	return -EINVAL;
}

static int
venc_reqbufs(struct file *file, void *fh, struct v4l2_requestbuffers *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	if (!b->count)
		vb2_core_queue_release(queue);

	return vb2_reqbufs(queue, b);
}

static int venc_querybuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);
	unsigned int p;
	int ret;

	ret = vb2_querybuf(queue, b);
	if (ret)
		return ret;

	if (b->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    b->memory == V4L2_MEMORY_MMAP) {
		for (p = 0; p < b->length; p++)
			b->m.planes[p].m.mem_offset += DST_QUEUE_OFF_BASE;
	}

	return 0;
}

static int venc_create_bufs(struct file *file, void *fh,
			    struct v4l2_create_buffers *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->format.type);

	if (!queue)
		return -EINVAL;

	return vb2_create_bufs(queue, b);
}

static int venc_prepare_buf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_prepare_buf(queue, b);
}

static int venc_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_qbuf(queue, b);
}

static int
venc_exportbuf(struct file *file, void *fh, struct v4l2_exportbuffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_expbuf(queue, b);
}

static int venc_dqbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_dqbuf(queue, b, file->f_flags & O_NONBLOCK);
}

static int venc_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, type);

	if (!queue)
		return -EINVAL;

	return vb2_streamon(queue, type);
}

static int venc_streamoff(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, type);

	if (!queue)
		return -EINVAL;

	return vb2_streamoff(queue, type);
}

static int venc_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vidc_inst *inst = to_inst(file);
	struct v4l2_outputparm *out = &a->parm.output;
	struct v4l2_fract *timeperframe = &out->timeperframe;
	u64 us_per_frame, fps;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	memset(out->reserved, 0, sizeof(out->reserved));

	if (!timeperframe->denominator)
		timeperframe->denominator = inst->timeperframe.denominator;
	if (!timeperframe->numerator)
		timeperframe->numerator = inst->timeperframe.numerator;

	out->capability = V4L2_CAP_TIMEPERFRAME;

	us_per_frame = timeperframe->numerator * (u64)USEC_PER_SEC;
	do_div(us_per_frame, timeperframe->denominator);

	if (!us_per_frame)
		return -EINVAL;

	fps = (u64)USEC_PER_SEC;
	do_div(fps, us_per_frame);

	inst->timeperframe = *timeperframe;
	inst->fps = fps;

	return 0;
}

static int venc_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vidc_inst *inst = to_inst(file);

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	a->parm.output.capability |= V4L2_CAP_TIMEPERFRAME;
	a->parm.output.timeperframe = inst->timeperframe;

	return 0;
}

static int venc_enum_framesizes(struct file *file, void *fh,
				struct v4l2_frmsizeenum *fsize)
{
	struct hfi_inst *inst = to_hfi_inst(file);
	const struct vidc_format *fmt;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;

	fmt = find_format(fsize->pixel_format,
			  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (!fmt) {
		fmt = find_format(fsize->pixel_format,
				  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (!fmt)
			return -EINVAL;
	}

	if (fsize->index)
		return -EINVAL;

	fsize->stepwise.min_width = inst->width.min;
	fsize->stepwise.max_width = inst->width.max;
	fsize->stepwise.step_width = inst->width.step_size;
	fsize->stepwise.min_height = inst->height.min;
	fsize->stepwise.max_height = inst->height.max;
	fsize->stepwise.step_height = inst->height.step_size;

	return 0;
}

static int venc_enum_frameintervals(struct file *file, void *fh,
				    struct v4l2_frmivalenum *fival)
{
	struct hfi_inst *inst = to_hfi_inst(file);
	const struct vidc_format *fmt;

	fival->type = V4L2_FRMIVAL_TYPE_STEPWISE;

	fmt = find_format(fival->pixel_format,
			  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (!fmt) {
		fmt = find_format(fival->pixel_format,
				  V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
		if (!fmt)
			return -EINVAL;
	}

	if (fival->index)
		return -EINVAL;

	if (!fival->width || !fival->height)
		return -EINVAL;

	if (fival->width > inst->width.max ||
	    fival->width < inst->width.min ||
	    fival->height > inst->height.max ||
	    fival->height < inst->height.min)
		return -EINVAL;

	fival->stepwise.min.numerator = 1;
	fival->stepwise.min.denominator = inst->framerate.max;
	fival->stepwise.max.numerator = 1;
	fival->stepwise.max.denominator = inst->framerate.min;
	fival->stepwise.step.numerator = 1;
	fival->stepwise.step.denominator = inst->framerate.max;

	return 0;
}

static int venc_subscribe_event(struct v4l2_fh *fh,
				const struct  v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 2, NULL);
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ioctl_ops venc_ioctl_ops = {
	.vidioc_querycap = venc_querycap,
	.vidioc_enum_fmt_vid_cap_mplane = venc_enum_fmt,
	.vidioc_enum_fmt_vid_out_mplane = venc_enum_fmt,
	.vidioc_s_fmt_vid_cap_mplane = venc_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = venc_s_fmt,
	.vidioc_g_fmt_vid_cap_mplane = venc_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = venc_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = venc_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = venc_try_fmt,
	.vidioc_g_selection = venc_g_selection,
	.vidioc_s_selection = venc_s_selection,
	.vidioc_reqbufs = venc_reqbufs,
	.vidioc_querybuf = venc_querybuf,
	.vidioc_create_bufs = venc_create_bufs,
	.vidioc_prepare_buf = venc_prepare_buf,
	.vidioc_qbuf = venc_qbuf,
	.vidioc_expbuf = venc_exportbuf,
	.vidioc_dqbuf = venc_dqbuf,
	.vidioc_streamon = venc_streamon,
	.vidioc_streamoff = venc_streamoff,
	.vidioc_s_parm = venc_s_parm,
	.vidioc_g_parm = venc_g_parm,
	.vidioc_enum_framesizes = venc_enum_framesizes,
	.vidioc_enum_frameintervals = venc_enum_frameintervals,
	.vidioc_subscribe_event = venc_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int venc_init_session(struct vidc_inst *inst)
{
	struct hfi_core *hfi = &inst->core->hfi;
	u32 pixfmt = inst->fmt_cap->pixfmt;
	struct hfi_framesize fs;
	u32 ptype;
	int ret;

	ret = vidc_hfi_session_init(hfi, inst->hfi_inst, pixfmt,
				    VIDC_SESSION_TYPE_ENC);
	if (ret)
		return ret;

	ptype = HFI_PROPERTY_PARAM_FRAME_SIZE;
	fs.buffer_type = HFI_BUFFER_INPUT;
	fs.width = inst->out_width;
	fs.height = inst->out_height;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &fs);
	if (ret)
		goto err;

	fs.buffer_type = HFI_BUFFER_OUTPUT;
	fs.width = inst->width;
	fs.height = inst->height;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &fs);
	if (ret)
		goto err;

	pixfmt = inst->fmt_out->pixfmt;

	ret = vidc_set_color_format(inst, HFI_BUFFER_INPUT, pixfmt);
	if (ret)
		goto err;

	return 0;
err:
	vidc_hfi_session_deinit(hfi, inst->hfi_inst);
	return ret;
}

static int venc_cap_num_buffers(struct vidc_inst *inst,
				struct hfi_buffer_requirements *bufreq)
{
	struct hfi_core *hfi = &inst->core->hfi;
	struct device *dev = inst->core->dev;
	int ret, ret2;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		return ret;

	ret = venc_init_session(inst);
	if (ret)
		goto put_sync;

	ret = vidc_buf_descs(inst, HFI_BUFFER_OUTPUT, bufreq);

	vidc_hfi_session_deinit(hfi, inst->hfi_inst);

put_sync:
	ret2 = pm_runtime_put_sync(dev);

	return ret ? ret : ret2;
}

static int venc_queue_setup(struct vb2_queue *q,
			    unsigned int *num_buffers, unsigned int *num_planes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct vidc_inst *inst = vb2_get_drv_priv(q);
	struct hfi_buffer_requirements bufreq;
	unsigned int p;
	int ret = 0;

	switch (q->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		*num_planes = inst->fmt_out->num_planes;
		*num_buffers = clamp_val(*num_buffers, 5, VIDEO_MAX_FRAME);
		inst->num_input_bufs = *num_buffers;

		for (p = 0; p < *num_planes; ++p) {
			sizes[p] = get_framesize_nv12(p, inst->height,
						      inst->width);
			alloc_devs[p] = inst->core->dev;
		}
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		*num_planes = inst->fmt_cap->num_planes;

		ret = venc_cap_num_buffers(inst, &bufreq);
		if (ret)
			break;

		*num_buffers = max(*num_buffers, bufreq.count_actual);
		inst->num_output_bufs = *num_buffers;

		sizes[0] = get_framesize_compressed(inst->height, inst->width);
		alloc_devs[0] = inst->core->dev;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int venc_check_configuration(struct vidc_inst *inst)
{
	struct hfi_buffer_requirements bufreq;
	int ret;

	ret = vidc_buf_descs(inst, HFI_BUFFER_OUTPUT, &bufreq);
	if (ret)
		return ret;

	if (inst->num_output_bufs < bufreq.count_actual ||
	    inst->num_output_bufs < bufreq.count_min)
		ret = -EINVAL;

	ret = vidc_buf_descs(inst, HFI_BUFFER_INPUT, &bufreq);
	if (ret)
		return ret;

	if (inst->num_input_bufs < bufreq.count_actual ||
	    inst->num_input_bufs < bufreq.count_min)
		ret = -EINVAL;

	return ret;
}

static int venc_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vidc_inst *inst = vb2_get_drv_priv(q);
	struct hfi_core *hfi = &inst->core->hfi;
	struct device *dev = inst->core->dev;
	struct hfi_buffer_count_actual buf_count;
	struct vb2_queue *queue;
	u32 ptype;
	int ret;

	switch (q->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		queue = &inst->bufq_cap;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		queue = &inst->bufq_out;
		break;
	default:
		return -EINVAL;
	}

	if (!vb2_is_streaming(queue))
		return 0;

	inst->sequence = 0;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		return ret;

	ret = venc_init_session(inst);
	if (ret)
		goto put_sync;

	ret = venc_set_properties(inst);
	if (ret)
		goto deinit_sess;

	ret = venc_check_configuration(inst);
	if (ret)
		goto deinit_sess;

	ptype = HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL;
	buf_count.type = HFI_BUFFER_OUTPUT;
	buf_count.count_actual = inst->num_output_bufs;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype,
					    &buf_count);
	if (ret)
		goto deinit_sess;

	buf_count.type = HFI_BUFFER_INPUT;
	buf_count.count_actual = inst->num_input_bufs;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype,
					    &buf_count);
	if (ret)
		goto deinit_sess;

	ret = vidc_vb2_start_streaming(inst);
	if (ret)
		goto deinit_sess;

	return 0;

deinit_sess:
	vidc_hfi_session_deinit(hfi, inst->hfi_inst);
put_sync:
	pm_runtime_put_sync(dev);
	return ret;
}

static const struct vb2_ops venc_vb2_ops = {
	.queue_setup = venc_queue_setup,
	.buf_init = vidc_vb2_buf_init,
	.buf_prepare = vidc_vb2_buf_prepare,
	.start_streaming = venc_start_streaming,
	.stop_streaming = vidc_vb2_stop_streaming,
	.buf_queue = vidc_vb2_buf_queue,
};

static int venc_empty_buf_done(struct hfi_inst *hfi_inst, u32 addr,
			       u32 bytesused, u32 data_offset, u32 flags)
{
	struct vidc_inst *inst = hfi_inst->ops_priv;
	struct vb2_v4l2_buffer *vbuf;
	enum vb2_buffer_state state;
	struct vb2_buffer *vb;

	vbuf = vidc_vb2_find_buf(inst, addr);
	if (!vbuf)
		return -EINVAL;

	vb = &vbuf->vb2_buf;
	vb->planes[0].bytesused = bytesused;
	vb->planes[0].data_offset = data_offset;
	vbuf->flags = flags;
	state = VB2_BUF_STATE_DONE;

	if (data_offset > vb->planes[0].length ||
	    bytesused > vb->planes[0].length)
		state = VB2_BUF_STATE_ERROR;

	vb2_buffer_done(vb, state);

	return 0;
}

static int venc_fill_buf_done(struct hfi_inst *hfi_inst, u32 addr,
			      u32 bytesused, u32 data_offset, u32 flags,
			      struct timeval *timestamp)
{
	struct vidc_inst *inst = hfi_inst->ops_priv;
	struct vb2_v4l2_buffer *vbuf;
	enum vb2_buffer_state state;
	struct vb2_buffer *vb;

	vbuf = vidc_vb2_find_buf(inst, addr);
	if (!vbuf)
		return -EINVAL;

	vb = &vbuf->vb2_buf;
	vb->planes[0].bytesused = bytesused;
	vb->planes[0].data_offset = data_offset;
	vb->timestamp = timeval_to_ns(timestamp);
	vbuf->flags = flags;
	vbuf->sequence = inst->sequence++;
	state = VB2_BUF_STATE_DONE;

	if (data_offset > vb->planes[0].length ||
	    bytesused > vb->planes[0].length)
		state = VB2_BUF_STATE_ERROR;

	vb2_buffer_done(vb, state);

	return 0;
}

static int venc_event_notify(struct hfi_inst *hfi_inst, u32 event,
			     struct hfi_event_data *data)
{
	struct vidc_inst *inst = hfi_inst->ops_priv;
	struct device *dev = inst->core->dev;

	switch (event) {
	case EVT_SESSION_ERROR:
		if (hfi_inst) {
			mutex_lock(&hfi_inst->lock);
			inst->hfi_inst->state = INST_INVALID;
			mutex_unlock(&hfi_inst->lock);
		}
		dev_err(dev, "enc: event session error (inst:%p)\n", hfi_inst);
		break;
	case EVT_SYS_EVENT_CHANGE:
		switch (data->event_type) {
		case HFI_EVENT_DATA_SEQUENCE_CHANGED_SUFFICIENT_BUF_RESOURCES:
			dev_dbg(dev, "event sufficient resources\n");
			break;
		case HFI_EVENT_DATA_SEQUENCE_CHANGED_INSUFFICIENT_BUF_RESOURCES:
			inst->reconfig_height = data->height;
			inst->reconfig_width = data->width;
			inst->in_reconfig = true;
			dev_dbg(dev, "event not sufficient resources\n");
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

static const struct hfi_inst_ops venc_hfi_ops = {
	.empty_buf_done = venc_empty_buf_done,
	.fill_buf_done = venc_fill_buf_done,
	.event_notify = venc_event_notify,
};

static void venc_inst_init(struct vidc_inst *inst)
{
	struct hfi_inst *hfi_inst = inst->hfi_inst;

	inst->fmt_cap = &venc_formats[2];
	inst->fmt_out = &venc_formats[0];
	inst->width = 1280;
	inst->height = ALIGN(720, 32);
	inst->fps = 15;
	inst->timeperframe.numerator = 1;
	inst->timeperframe.denominator = 15;

	hfi_inst->width.min = 64;
	hfi_inst->width.max = 1920;
	hfi_inst->width.step_size = 1;
	hfi_inst->height.min = 64;
	hfi_inst->height.max = ALIGN(1080, 32);
	hfi_inst->height.step_size = 1;
	hfi_inst->framerate.min = 1;
	hfi_inst->framerate.max = 30;
	hfi_inst->framerate.step_size = 1;
	hfi_inst->mbs_per_frame.min = 16;
	hfi_inst->mbs_per_frame.max = 8160;
}

int venc_init(struct vidc_core *core, struct video_device *enc)
{
	int ret;

	enc->release = video_device_release_empty;
	enc->fops = &vidc_fops;
	enc->ioctl_ops = &venc_ioctl_ops;
	enc->vfl_dir = VFL_DIR_M2M;
	enc->v4l2_dev = &core->v4l2_dev;

	ret = video_register_device(enc, VFL_TYPE_GRABBER, -1);
	if (ret)
		return ret;

	video_set_drvdata(enc, core);

	return 0;
}

void venc_deinit(struct vidc_core *core, struct video_device *enc)
{
	video_unregister_device(enc);
}

int venc_open(struct vidc_inst *inst)
{
	struct hfi_core *hfi = &inst->core->hfi;
	struct vb2_queue *q;
	int ret;

	ret = venc_ctrl_init(inst);
	if (ret)
		return ret;

	inst->hfi_inst = vidc_hfi_session_create(hfi, &venc_hfi_ops, inst);
	if (IS_ERR(inst->hfi_inst)) {
		ret = PTR_ERR(inst->hfi_inst);
		goto err_ctrl_deinit;
	}

	venc_inst_init(inst);

	q = &inst->bufq_cap;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->ops = &venc_vb2_ops;
	q->mem_ops = &vb2_dma_sg_memops;
	q->drv_priv = inst;
	q->buf_struct_size = sizeof(struct vidc_buffer);
	q->allow_zero_bytesused = 1;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_session_destroy;

	q = &inst->bufq_out;
	q->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->ops = &venc_vb2_ops;
	q->mem_ops = &vb2_dma_sg_memops;
	q->drv_priv = inst;
	q->buf_struct_size = sizeof(struct vidc_buffer);
	q->allow_zero_bytesused = 1;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_cap_queue_release;

	return 0;

err_cap_queue_release:
	vb2_queue_release(&inst->bufq_cap);
err_session_destroy:
	vidc_hfi_session_destroy(hfi, inst->hfi_inst);
	inst->hfi_inst = NULL;
err_ctrl_deinit:
	venc_ctrl_deinit(inst);
	return ret;
}

void venc_close(struct vidc_inst *inst)
{
	struct hfi_core *hfi = &inst->core->hfi;

	vb2_queue_release(&inst->bufq_out);
	vb2_queue_release(&inst->bufq_cap);
	venc_ctrl_deinit(inst);
	vidc_hfi_session_destroy(hfi, inst->hfi_inst);
}
