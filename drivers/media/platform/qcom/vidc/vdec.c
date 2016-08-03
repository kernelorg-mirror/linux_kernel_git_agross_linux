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
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-dma-sg.h>

#include "core.h"
#include "helpers.h"
#include "load.h"
#include "vdec.h"
#include "vdec_ctrls.h"

#define MACROBLOCKS_PER_PIXEL	(16 * 16)

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

	return ALIGN(size, SZ_4K);
}

static u32 get_framesize_compressed(u32 mbs_per_frame)
{
	return ((mbs_per_frame * MACROBLOCKS_PER_PIXEL * 3 / 2) / 2) + 128;
}

static const struct vidc_format vdec_formats[] = {
	{
		.pixfmt = V4L2_PIX_FMT_NV12,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG4,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_MPEG2,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_H263,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_VC1_ANNEX_G,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_VC1_ANNEX_L,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_H264,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_VP8,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}, {
		.pixfmt = V4L2_PIX_FMT_XVID,
		.num_planes = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	},
};

static const struct vidc_format *find_format(u32 pixfmt, u32 type)
{
	const struct vidc_format *fmt = vdec_formats;
	unsigned int size = ARRAY_SIZE(vdec_formats);
	unsigned int i;

	for (i = 0; i < size; i++) {
		if (fmt[i].pixfmt == pixfmt)
			break;
	}

	if (i == size || fmt[i].type != type)
		return NULL;

	return &fmt[i];
}

static const struct vidc_format *find_format_by_index(int index, u32 type)
{
	const struct vidc_format *fmt = vdec_formats;
	unsigned int size = ARRAY_SIZE(vdec_formats);
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

static int vdec_set_properties(struct vidc_inst *inst)
{
	struct vdec_controls *ctr = &inst->controls.dec;
	struct hfi_core *hfi = &inst->core->hfi;
	struct hfi_enable en = { .enable = 1 };
	struct hfi_framerate frate;
	u32 ptype;
	int ret;

	ptype = HFI_PROPERTY_PARAM_VDEC_CONTINUE_DATA_TRANSFER;
	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &en);
	if (ret)
		return ret;

	ptype = HFI_PROPERTY_CONFIG_FRAME_RATE;
	frate.buffer_type = HFI_BUFFER_INPUT;
	frate.framerate = inst->fps * (1 << 16);

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &frate);
	if (ret)
		return ret;

	if (ctr->post_loop_deb_mode) {
		ptype = HFI_PROPERTY_CONFIG_VDEC_POST_LOOP_DEBLOCKER;
		en.enable = 1;
		ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype,
						    &en);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct vidc_format *
vdec_try_fmt_common(struct vidc_inst *inst, struct v4l2_format *f)
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
			pixmp->pixelformat = V4L2_PIX_FMT_NV12;
		else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
			pixmp->pixelformat = V4L2_PIX_FMT_H264;
		else
			return NULL;
		fmt = find_format(pixmp->pixelformat, f->type);
		pixmp->width = 1280;
		pixmp->height = 720;
	}

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		pixmp->height = ALIGN(pixmp->height, 32);

	pixmp->width = clamp(pixmp->width, hfi_inst->width.min,
			     hfi_inst->width.max);
	pixmp->height = clamp(pixmp->height, hfi_inst->height.min,
			      hfi_inst->height.max);
	if (pixmp->field == V4L2_FIELD_ANY)
		pixmp->field = V4L2_FIELD_NONE;
	pixmp->num_planes = fmt->num_planes;
	pixmp->flags = 0;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		for (p = 0; p < pixmp->num_planes; p++) {
			pfmt[p].sizeimage = get_framesize_nv12(p, pixmp->height,
							       pixmp->width);

			pfmt[p].bytesperline = ALIGN(pixmp->width, 128);
		}
	} else {
		u32 mbs = pixmp->width * pixmp->height / MACROBLOCKS_PER_PIXEL;

		pfmt[0].sizeimage = get_framesize_compressed(mbs);
		pfmt[0].bytesperline = 0;
	}

	return fmt;
}

static int vdec_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vidc_inst *inst = to_inst(file);
	const struct vidc_format *fmt;

	fmt = vdec_try_fmt_common(inst, f);
	if (!fmt)
		return -EINVAL;

	return 0;
}

static int vdec_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vidc_inst *inst = to_inst(file);
	const struct vidc_format *fmt = NULL;
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		fmt = inst->fmt_cap;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		fmt = inst->fmt_out;

	if (inst->in_reconfig) {
		inst->height = inst->reconfig_height;
		inst->width = inst->reconfig_width;
		inst->in_reconfig = false;
	}

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

	vdec_try_fmt_common(inst, f);

	return 0;
}

static int vdec_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct vidc_inst *inst = to_inst(file);
	struct v4l2_pix_format_mplane *pixmp = &f->fmt.pix_mp;
	struct v4l2_pix_format_mplane orig_pixmp;
	const struct vidc_format *fmt;
	struct v4l2_format format;
	u32 pixfmt_out = 0, pixfmt_cap = 0;

	orig_pixmp = *pixmp;

	fmt = vdec_try_fmt_common(inst, f);
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
	vdec_try_fmt_common(inst, &format);
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
	vdec_try_fmt_common(inst, &format);
	inst->width = format.fmt.pix_mp.width;
	inst->height = format.fmt.pix_mp.height;

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		inst->fmt_out = fmt;
	else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		inst->fmt_cap = fmt;

	return 0;
}

static int
vdec_g_selection(struct file *file, void *priv, struct v4l2_selection *s)
{
	struct vidc_inst *inst = to_inst(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE:
		break;
	default:
		return -EINVAL;
	}

	s->r.top = 0;
	s->r.left = 0;
	s->r.width = inst->out_width;
	s->r.height = inst->out_height;

	return 0;
}

static int
vdec_reqbufs(struct file *file, void *fh, struct v4l2_requestbuffers *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	if (!b->count)
		vb2_core_queue_release(queue);

	return vb2_reqbufs(queue, b);
}

static int
vdec_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strlcpy(cap->driver, VIDC_DRV_NAME, sizeof(cap->driver));
	strlcpy(cap->card, "video decoder", sizeof(cap->card));
	strlcpy(cap->bus_info, "platform:vidc", sizeof(cap->bus_info));

	cap->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int vdec_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	const struct vidc_format *fmt;

	memset(f->reserved, 0, sizeof(f->reserved));

	fmt = find_format_by_index(f->index, f->type);
	if (!fmt)
		return -EINVAL;

	f->pixelformat = fmt->pixfmt;

	return 0;
}

static int vdec_querybuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);
	unsigned int p;
	int ret;

	if (!queue)
		return -EINVAL;

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

static int
vdec_create_bufs(struct file *file, void *fh, struct v4l2_create_buffers *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->format.type);

	if (!queue)
		return -EINVAL;

	return vb2_create_bufs(queue, b);
}

static int vdec_prepare_buf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_prepare_buf(queue, b);
}

static int vdec_qbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_qbuf(queue, b);
}

static int
vdec_exportbuf(struct file *file, void *fh, struct v4l2_exportbuffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_expbuf(queue, b);
}

static int vdec_dqbuf(struct file *file, void *fh, struct v4l2_buffer *b)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, b->type);

	if (!queue)
		return -EINVAL;

	return vb2_dqbuf(queue, b, file->f_flags & O_NONBLOCK);
}

static int vdec_streamon(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, type);

	if (!queue)
		return -EINVAL;

	return vb2_streamon(queue, type);
}

static int vdec_streamoff(struct file *file, void *fh, enum v4l2_buf_type type)
{
	struct vb2_queue *queue = vidc_to_vb2q(file, type);

	if (!queue)
		return -EINVAL;

	return vb2_streamoff(queue, type);
}

static int vdec_s_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vidc_inst *inst = to_inst(file);
	struct v4l2_captureparm *cap = &a->parm.capture;
	struct v4l2_fract *timeperframe = &cap->timeperframe;
	u64 us_per_frame, fps;

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	memset(cap->reserved, 0, sizeof(cap->reserved));
	if (!timeperframe->denominator)
		timeperframe->denominator = inst->timeperframe.denominator;
	if (!timeperframe->numerator)
		timeperframe->numerator = inst->timeperframe.numerator;
	cap->readbuffers = 0;
	cap->extendedmode = 0;
	cap->capability = V4L2_CAP_TIMEPERFRAME;
	us_per_frame = timeperframe->numerator * (u64)USEC_PER_SEC;
	do_div(us_per_frame, timeperframe->denominator);

	if (!us_per_frame)
		return -EINVAL;

	fps = (u64)USEC_PER_SEC;
	do_div(fps, us_per_frame);

	inst->fps = fps;
	inst->timeperframe = *timeperframe;

	return 0;
}

static int vdec_g_parm(struct file *file, void *fh, struct v4l2_streamparm *a)
{
	struct vidc_inst *inst = to_inst(file);

	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
	    a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		return -EINVAL;

	a->parm.capture.capability |= V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.timeperframe = inst->timeperframe;

	return 0;
}

static int vdec_enum_framesizes(struct file *file, void *fh,
				struct v4l2_frmsizeenum *fsize)
{
	struct hfi_inst *hfi_inst = to_hfi_inst(file);
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

	fsize->stepwise.min_width = hfi_inst->width.min;
	fsize->stepwise.max_width = hfi_inst->width.max;
	fsize->stepwise.step_width = hfi_inst->width.step_size;
	fsize->stepwise.min_height = hfi_inst->height.min;
	fsize->stepwise.max_height = hfi_inst->height.max;
	fsize->stepwise.step_height = hfi_inst->height.step_size;

	return 0;
}

static int vdec_enum_frameintervals(struct file *file, void *fh,
				    struct v4l2_frmivalenum *fival)
{
	struct hfi_inst *hfi_inst = to_hfi_inst(file);
	const struct vidc_format *fmt;

	fival->type = V4L2_FRMIVAL_TYPE_STEPWISE;

	fmt = find_format(fival->pixel_format,
			  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (!fmt)
		return -EINVAL;

	if (fival->index)
		return -EINVAL;

	if (!fival->width || !fival->height)
		return -EINVAL;

	if (fival->width > hfi_inst->width.max ||
	    fival->width < hfi_inst->width.min ||
	    fival->height > hfi_inst->height.max ||
	    fival->height < hfi_inst->height.min)
		return -EINVAL;

	fival->stepwise.min.numerator = hfi_inst->framerate.min;
	fival->stepwise.min.denominator = 1;
	fival->stepwise.max.numerator = hfi_inst->framerate.max;
	fival->stepwise.max.denominator = 1;
	fival->stepwise.step.numerator = hfi_inst->framerate.step_size;
	fival->stepwise.step.denominator = 1;

	return 0;
}

static int vdec_subscribe_event(struct v4l2_fh *fh,
				const struct v4l2_event_subscription *sub)
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

static const struct v4l2_ioctl_ops vdec_ioctl_ops = {
	.vidioc_querycap = vdec_querycap,
	.vidioc_enum_fmt_vid_cap_mplane = vdec_enum_fmt,
	.vidioc_enum_fmt_vid_out_mplane = vdec_enum_fmt,
	.vidioc_s_fmt_vid_cap_mplane = vdec_s_fmt,
	.vidioc_s_fmt_vid_out_mplane = vdec_s_fmt,
	.vidioc_g_fmt_vid_cap_mplane = vdec_g_fmt,
	.vidioc_g_fmt_vid_out_mplane = vdec_g_fmt,
	.vidioc_try_fmt_vid_cap_mplane = vdec_try_fmt,
	.vidioc_try_fmt_vid_out_mplane = vdec_try_fmt,
	.vidioc_g_selection = vdec_g_selection,
	.vidioc_reqbufs = vdec_reqbufs,
	.vidioc_querybuf = vdec_querybuf,
	.vidioc_create_bufs = vdec_create_bufs,
	.vidioc_prepare_buf = vdec_prepare_buf,
	.vidioc_qbuf = vdec_qbuf,
	.vidioc_expbuf = vdec_exportbuf,
	.vidioc_dqbuf = vdec_dqbuf,
	.vidioc_streamon = vdec_streamon,
	.vidioc_streamoff = vdec_streamoff,
	.vidioc_s_parm = vdec_s_parm,
	.vidioc_g_parm = vdec_g_parm,
	.vidioc_enum_framesizes = vdec_enum_framesizes,
	.vidioc_enum_frameintervals = vdec_enum_frameintervals,
	.vidioc_subscribe_event = vdec_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int vdec_init_session(struct vidc_inst *inst)
{
	struct hfi_core *hfi = &inst->core->hfi;
	u32 pixfmt = inst->fmt_out->pixfmt;
	struct hfi_framesize fs;
	u32 ptype;
	int ret;

	ret = vidc_hfi_session_init(hfi, inst->hfi_inst, pixfmt,
				    VIDC_SESSION_TYPE_DEC);
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

	pixfmt = inst->fmt_cap->pixfmt;

	ret = vidc_set_color_format(inst, HFI_BUFFER_OUTPUT, pixfmt);
	if (ret)
		goto err;

	return 0;
err:
	vidc_hfi_session_deinit(hfi, inst->hfi_inst);
	return ret;
}

static int vdec_cap_num_buffers(struct vidc_inst *inst,
				struct hfi_buffer_requirements *bufreq)
{
	struct hfi_core *hfi = &inst->core->hfi;
	struct device *dev = inst->core->dev;
	int ret, ret2;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		return ret;

	ret = vdec_init_session(inst);
	if (ret)
		goto put_sync;

	ret = vidc_buf_descs(inst, HFI_BUFFER_OUTPUT, bufreq);

	vidc_hfi_session_deinit(hfi, inst->hfi_inst);

put_sync:
	ret2 = pm_runtime_put_sync(dev);

	return ret ? ret : ret2;
}

static int vdec_queue_setup(struct vb2_queue *q,
			    unsigned int *num_buffers, unsigned int *num_planes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct vidc_inst *inst = vb2_get_drv_priv(q);
	struct hfi_buffer_requirements bufreq;
	unsigned int p;
	int ret = 0;
	u32 mbs;

	switch (q->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		*num_planes = inst->fmt_out->num_planes;

		*num_buffers = clamp_val(*num_buffers, 4, VIDEO_MAX_FRAME);

		mbs = inst->out_width * inst->out_height /
				MACROBLOCKS_PER_PIXEL;
		for (p = 0; p < *num_planes; p++) {
			sizes[p] = get_framesize_compressed(mbs);
			alloc_devs[p] = inst->core->dev;
		}

		inst->num_input_bufs = *num_buffers;
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		*num_planes = inst->fmt_cap->num_planes;

		ret = vdec_cap_num_buffers(inst, &bufreq);
		if (ret)
			break;

		*num_buffers = max(*num_buffers, bufreq.count_actual);

		for (p = 0; p < *num_planes; p++) {
			sizes[p] = get_framesize_nv12(p, inst->height,
						      inst->width);
			alloc_devs[p] = inst->core->dev;
		}

		inst->num_output_bufs = *num_buffers;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int vdec_check_configuration(struct vidc_inst *inst)
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

	if (/*inst->num_input_bufs < bufreq.count_actual ||*/
	    inst->num_input_bufs < bufreq.count_min)
		ret = -EINVAL;

	return 0;
}

static int vdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vidc_inst *inst = vb2_get_drv_priv(q);
	struct hfi_core *hfi = &inst->core->hfi;
	struct device *dev = inst->core->dev;
	struct hfi_buffer_requirements bufreq;
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

	inst->in_reconfig = false;
	inst->sequence = 0;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		return ret;

	ret = vdec_init_session(inst);
	if (ret)
		goto put_sync;

	ret = vdec_set_properties(inst);
	if (ret)
		goto deinit_sess;

	ret = vdec_check_configuration(inst);
	if (ret)
		goto deinit_sess;

	ptype = HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL;
	buf_count.type = HFI_BUFFER_INPUT;
	buf_count.count_actual = inst->num_input_bufs;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst,
					    ptype, &buf_count);
	if (ret)
		goto deinit_sess;

	ret = vidc_buf_descs(inst, HFI_BUFFER_OUTPUT, &bufreq);
	if (ret)
		goto deinit_sess;

	ptype = HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL;
	buf_count.type = HFI_BUFFER_OUTPUT;
	buf_count.count_actual = inst->num_output_bufs;

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst,
					    ptype, &buf_count);
	if (ret)
		goto deinit_sess;

	if (inst->num_output_bufs != bufreq.count_actual) {
		struct hfi_buffer_display_hold_count_actual display;

		ptype = HFI_PROPERTY_PARAM_BUFFER_DISPLAY_HOLD_COUNT_ACTUAL;
		display.type = HFI_BUFFER_OUTPUT;
		display.hold_count = inst->num_output_bufs -
				     bufreq.count_actual;

		ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst,
						    ptype, &display);
		if (ret)
			goto deinit_sess;
	}

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

static const struct vb2_ops vdec_vb2_ops = {
	.queue_setup = vdec_queue_setup,
	.buf_init = vidc_vb2_buf_init,
	.buf_prepare = vidc_vb2_buf_prepare,
	.start_streaming = vdec_start_streaming,
	.stop_streaming = vidc_vb2_stop_streaming,
	.buf_queue = vidc_vb2_buf_queue,
};

static int vdec_empty_buf_done(struct hfi_inst *hfi_inst, u32 addr,
			       u32 bytesused, u32 data_offset, u32 flags)
{
	struct vidc_inst *inst = hfi_inst->ops_priv;
	struct vb2_v4l2_buffer *vbuf;
	struct vb2_buffer *vb;

	vbuf = vidc_vb2_find_buf(inst, addr);
	if (!vbuf)
		return -EINVAL;

	vb = &vbuf->vb2_buf;
	vbuf->flags = flags;

	vb2_buffer_done(vb, VB2_BUF_STATE_DONE);

	return 0;
}

static int vdec_fill_buf_done(struct hfi_inst *hfi_inst, u32 addr,
			      u32 bytesused, u32 data_offset, u32 flags,
			      struct timeval *timestamp)
{
	struct vidc_inst *inst = hfi_inst->ops_priv;
	struct vb2_v4l2_buffer *vbuf;
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

	vb2_buffer_done(vb, VB2_BUF_STATE_DONE);

	if (vbuf->flags & V4L2_BUF_FLAG_LAST) {
		const struct v4l2_event ev = {
			.type = V4L2_EVENT_EOS
		};

		v4l2_event_queue_fh(&inst->fh, &ev);
	}

	return 0;
}

static int vdec_event_notify(struct hfi_inst *hfi_inst, u32 event,
			     struct hfi_event_data *data)
{
	struct vidc_inst *inst = hfi_inst->ops_priv;
	struct device *dev = inst->core->dev;
	const struct v4l2_event ev = { .type = V4L2_EVENT_SOURCE_CHANGE };

	switch (event) {
	case EVT_SESSION_ERROR:
		if (hfi_inst) {
			mutex_lock(&hfi_inst->lock);
			inst->hfi_inst->state = INST_INVALID;
			mutex_unlock(&hfi_inst->lock);
		}
		dev_err(dev, "dec: event session error (inst:%p)\n", hfi_inst);
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

			v4l2_event_queue_fh(&inst->fh, &ev);

			dev_dbg(dev, "event not sufficient resources (%ux%u)\n",
				data->width, data->height);
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

static const struct hfi_inst_ops vdec_hfi_ops = {
	.empty_buf_done = vdec_empty_buf_done,
	.fill_buf_done = vdec_fill_buf_done,
	.event_notify = vdec_event_notify,
};

static void vdec_inst_init(struct vidc_inst *inst)
{
	struct hfi_inst *hfi_inst = inst->hfi_inst;

	inst->fmt_out = &vdec_formats[6];
	inst->fmt_cap = &vdec_formats[0];
	inst->width = 1280;
	inst->height = ALIGN(720, 32);
	inst->out_width = 1280;
	inst->out_height = 720;
	inst->fps = 30;
	inst->timeperframe.numerator = 1;
	inst->timeperframe.denominator = 30;

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

int vdec_init(struct vidc_core *core, struct video_device *dec)
{
	int ret;

	dec->release = video_device_release_empty;
	dec->fops = &vidc_fops;
	dec->ioctl_ops = &vdec_ioctl_ops;
	dec->vfl_dir = VFL_DIR_M2M;
	dec->v4l2_dev = &core->v4l2_dev;

	ret = video_register_device(dec, VFL_TYPE_GRABBER, -1);
	if (ret)
		return ret;

	video_set_drvdata(dec, core);

	return 0;
}

void vdec_deinit(struct vidc_core *core, struct video_device *dec)
{
	video_unregister_device(dec);
}

int vdec_open(struct vidc_inst *inst)
{
	struct hfi_core *hfi = &inst->core->hfi;
	struct vb2_queue *q;
	int ret;

	ret = vdec_ctrl_init(inst);
	if (ret)
		return ret;

	inst->hfi_inst = vidc_hfi_session_create(hfi, &vdec_hfi_ops, inst);
	if (IS_ERR(inst->hfi_inst)) {
		ret = PTR_ERR(inst->hfi_inst);
		goto err_ctrl_deinit;
	}

	vdec_inst_init(inst);

	q = &inst->bufq_cap;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	q->ops = &vdec_vb2_ops;
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
	q->ops = &vdec_vb2_ops;
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
	vdec_ctrl_deinit(inst);
	return ret;
}

void vdec_close(struct vidc_inst *inst)
{
	struct hfi_core *hfi = &inst->core->hfi;

	vb2_queue_release(&inst->bufq_out);
	vb2_queue_release(&inst->bufq_cap);
	vdec_ctrl_deinit(inst);
	vidc_hfi_session_destroy(hfi, inst->hfi_inst);
}
