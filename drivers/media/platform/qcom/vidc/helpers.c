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
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <media/videobuf2-dma-sg.h>

#include "helpers.h"
#include "int_bufs.h"
#include "load.h"
#include "hfi_helper.h"

static int session_set_buf(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vb2_queue *q = vb->vb2_queue;
	struct vidc_inst *inst = vb2_get_drv_priv(q);
	struct vidc_core *core = inst->core;
	struct device *dev = core->dev;
	struct hfi_core *hfi = &core->hfi;
	struct vidc_buffer *buf = to_vidc_buffer(vbuf);
	struct hfi_frame_data fdata;
	int ret;

	memset(&fdata, 0, sizeof(fdata));

	fdata.alloc_len = vb2_plane_size(vb, 0);
	fdata.device_addr = buf->dma_addr;
	fdata.timestamp = vb->timestamp;
	fdata.flags = 0;
	fdata.clnt_data = buf->dma_addr;

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		fdata.buffer_type = HFI_BUFFER_INPUT;
		fdata.filled_len = vb2_get_plane_payload(vb, 0);
		fdata.offset = vb->planes[0].data_offset;

		if (vbuf->flags & V4L2_BUF_FLAG_LAST || !fdata.filled_len)
			fdata.flags |= HFI_BUFFERFLAG_EOS;

		ret = vidc_hfi_session_etb(hfi, inst->hfi_inst, &fdata);
	} else if (q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		fdata.buffer_type = HFI_BUFFER_OUTPUT;
		fdata.filled_len = 0;
		fdata.offset = 0;

		ret = vidc_hfi_session_ftb(hfi, inst->hfi_inst, &fdata);
	} else {
		ret = -EINVAL;
	}

	if (ret) {
		dev_err(dev, "failed to set session buffer (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int session_unregister_bufs(struct vidc_inst *inst)
{
	struct device *dev = inst->core->dev;
	struct hfi_core *hfi = &inst->core->hfi;
	struct hfi_buffer_desc *bd;
	struct vidc_buffer *buf, *tmp;
	int ret = 0;

	mutex_lock(&inst->registeredbufs.lock);
	list_for_each_entry_safe(buf, tmp, &inst->registeredbufs.list,
				 hfi_list) {
		list_del(&buf->hfi_list);
		bd = &buf->bd;
		bd->response_required = 1;
		ret = vidc_hfi_session_unset_buffers(hfi, inst->hfi_inst, bd);
		if (ret) {
			dev_err(dev, "%s: session release buffers failed\n",
				__func__);
			break;
		}
	}
	mutex_unlock(&inst->registeredbufs.lock);

	return ret;
}

static int session_register_bufs(struct vidc_inst *inst)
{
	struct device *dev = inst->core->dev;
	struct hfi_core *hfi = &inst->core->hfi;
	struct hfi_buffer_desc *bd;
	struct vidc_buffer *buf, *tmp;
	int ret = 0;

	mutex_lock(&inst->registeredbufs.lock);
	list_for_each_entry_safe(buf, tmp, &inst->registeredbufs.list,
				 hfi_list) {
		bd = &buf->bd;
		ret = vidc_hfi_session_set_buffers(hfi, inst->hfi_inst, bd);
		if (ret) {
			dev_err(dev, "%s: session: set buffer failed\n",
				__func__);
			break;
		}
	}
	mutex_unlock(&inst->registeredbufs.lock);

	return ret;
}

int vidc_buf_descs(struct vidc_inst *inst, u32 type,
		   struct hfi_buffer_requirements *out)
{
	struct hfi_core *hfi = &inst->core->hfi;
	u32 ptype = HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS;
	union hfi_get_property hprop;
	int ret, i;

	if (out)
		memset(out, 0, sizeof(*out));

	ret = vidc_hfi_session_get_property(hfi, inst->hfi_inst, ptype, &hprop);
	if (ret)
		return ret;

	ret = -EINVAL;

	for (i = 0; i < HFI_BUFFER_TYPE_MAX; i++) {
		if (hprop.bufreq[i].type != type)
			continue;

		if (out)
			memcpy(out, &hprop.bufreq[i], sizeof(*out));
		ret = 0;
		break;
	}

	return ret;
}

int vidc_set_color_format(struct vidc_inst *inst, u32 type, u32 pixfmt)
{
	struct hfi_uncompressed_format_select fmt;
	struct hfi_core *hfi = &inst->core->hfi;
	u32 ptype = HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT;
	int ret;

	fmt.buffer_type = type;

	switch (pixfmt) {
	case V4L2_PIX_FMT_NV12:
		fmt.format = HFI_COLOR_FORMAT_NV12;
		break;
	case V4L2_PIX_FMT_NV21:
		fmt.format = HFI_COLOR_FORMAT_NV21;
		break;
	default:
		return -ENOTSUPP;
	}

	ret = vidc_hfi_session_set_property(hfi, inst->hfi_inst, ptype, &fmt);
	if (ret)
		return ret;

	return 0;
}

struct vb2_v4l2_buffer *
vidc_vb2_find_buf(struct vidc_inst *inst, dma_addr_t addr)
{
	struct vidc_buffer *buf;
	struct vb2_v4l2_buffer *vb = NULL;

	mutex_lock(&inst->bufqueue_lock);

	list_for_each_entry(buf, &inst->bufqueue, list) {
		if (buf->dma_addr == addr) {
			vb = &buf->vb;
			break;
		}
	}

	if (vb)
		list_del(&buf->list);

	mutex_unlock(&inst->bufqueue_lock);

	return vb;
}

int vidc_vb2_buf_init(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vb2_queue *q = vb->vb2_queue;
	struct vidc_inst *inst = vb2_get_drv_priv(q);
	struct vidc_buffer *buf = to_vidc_buffer(vbuf);
	struct hfi_buffer_desc *bd = &buf->bd;
	struct sg_table *sgt;

	memset(bd, 0, sizeof(*bd));

	if (q->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return 0;

	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt)
		return -EINVAL;

	bd->buffer_size = vb2_plane_size(vb, 0);
	bd->buffer_type = HFI_BUFFER_OUTPUT;
	bd->num_buffers = 1;
	bd->device_addr = sg_dma_address(sgt->sgl);

	mutex_lock(&inst->registeredbufs.lock);
	list_add_tail(&buf->hfi_list, &inst->registeredbufs.list);
	mutex_unlock(&inst->registeredbufs.lock);

	return 0;
}

int vidc_vb2_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vidc_buffer *buf = to_vidc_buffer(vbuf);
	struct sg_table *sgt;

	sgt = vb2_dma_sg_plane_desc(vb, 0);
	if (!sgt)
		return -EINVAL;

	buf->dma_addr = sg_dma_address(sgt->sgl);

	return 0;
}

void vidc_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vidc_inst *inst = vb2_get_drv_priv(vb->vb2_queue);
	struct vidc_core *core = inst->core;
	struct device *dev = core->dev;
	struct vidc_buffer *buf = to_vidc_buffer(vbuf);
	unsigned int state;
	int ret;

	mutex_lock(&inst->hfi_inst->lock);
	state = inst->hfi_inst->state;
	mutex_unlock(&inst->hfi_inst->lock);

	if (state == INST_INVALID || state >= INST_STOP) {
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
		dev_dbg(dev, "%s: type:%d, invalid instance state\n", __func__,
			vb->type);
		return;
	}

	mutex_lock(&inst->bufqueue_lock);
	list_add_tail(&buf->list, &inst->bufqueue);
	mutex_unlock(&inst->bufqueue_lock);

	if (!vb2_is_streaming(&inst->bufq_cap) ||
	    !vb2_is_streaming(&inst->bufq_out))
		return;

	ret = session_set_buf(vb);
	if (ret)
		vb2_buffer_done(vb, VB2_BUF_STATE_ERROR);
}

void vidc_vb2_stop_streaming(struct vb2_queue *q)
{
	struct vidc_inst *inst = vb2_get_drv_priv(q);
	struct hfi_inst *hfi_inst = inst->hfi_inst;
	struct vidc_core *core = inst->core;
	struct device *dev = core->dev;
	struct hfi_core *hfi = &core->hfi;
	int ret, streamoff;

	mutex_lock(&inst->lock);
	streamoff = inst->streamoff;
	mutex_unlock(&inst->lock);

	if (streamoff)
		return;

	mutex_lock(&inst->lock);
	if (inst->streamon == 0) {
		mutex_unlock(&inst->lock);
		return;
	}
	mutex_unlock(&inst->lock);

	ret = vidc_hfi_session_stop(hfi, inst->hfi_inst);
	if (ret) {
		dev_err(dev, "session: stop failed (%d)\n", ret);
		goto abort;
	}

	ret = vidc_hfi_session_unload_res(hfi, inst->hfi_inst);
	if (ret) {
		dev_err(dev, "session: release resources failed (%d)\n", ret);
		goto abort;
	}

	ret = session_unregister_bufs(inst);
	if (ret) {
		dev_err(dev, "failed to release capture buffers: %d\n", ret);
		goto abort;
	}

	ret = internal_bufs_free(inst);

	if (hfi_inst->state == INST_INVALID || hfi->state == CORE_INVALID) {
		ret = -EINVAL;
		goto abort;
	}

abort:
	if (ret)
		vidc_hfi_session_abort(hfi, inst->hfi_inst);

	vidc_scale_clocks(inst->core);

	ret = vidc_hfi_session_deinit(hfi, inst->hfi_inst);

	mutex_lock(&inst->lock);
	inst->streamoff = 1;
	mutex_unlock(&inst->lock);

	if (ret)
		dev_err(dev, "stop streaming failed type: %d, ret: %d\n",
			q->type, ret);

	ret = pm_runtime_put_sync(dev);
	if (ret < 0)
		dev_err(dev, "%s: pm_runtime_put_sync (%d)\n", __func__, ret);
}

int vidc_vb2_start_streaming(struct vidc_inst *inst)
{
	struct device *dev = inst->core->dev;
	struct hfi_core *hfi = &inst->core->hfi;
	struct vidc_buffer *buf, *n;
	int ret;

	ret = session_register_bufs(inst);
	if (ret)
		return ret;

	ret = internal_bufs_alloc(inst);
	if (ret)
		return ret;

	vidc_scale_clocks(inst->core);

	ret = vidc_hfi_session_load_res(hfi, inst->hfi_inst);
	if (ret) {
		dev_err(dev, "session: load resources (%d)\n", ret);
		return ret;
	}

	ret = vidc_hfi_session_start(hfi, inst->hfi_inst);
	if (ret) {
		dev_err(dev, "session: start failed (%d)\n", ret);
		return ret;
	}

	mutex_lock(&inst->bufqueue_lock);
	list_for_each_entry_safe(buf, n, &inst->bufqueue, list) {
		ret = session_set_buf(&buf->vb.vb2_buf);
		if (ret)
			break;
	}
	mutex_unlock(&inst->bufqueue_lock);

	if (!ret) {
		mutex_lock(&inst->lock);
		inst->streamon = 1;
		mutex_unlock(&inst->lock);
	}

	return ret;
}
