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

#include "helpers.h"
#include "int_bufs.h"
#include "mem.h"

struct vidc_internal_buf {
	struct list_head list;
	u32 type;
	struct vidc_mem *mem;
};

static u32 scratch_buf_sufficient(struct vidc_inst *inst, u32 buffer_type)
{
	struct hfi_buffer_requirements bufreq;
	struct vidc_internal_buf *buf;
	unsigned int count = 0;
	int ret;

	ret = vidc_buf_descs(inst, buffer_type, &bufreq);
	if (ret)
		return 0;

	/* Check if current scratch buffers are sufficient */
	mutex_lock(&inst->scratchbufs.lock);
	list_for_each_entry(buf, &inst->scratchbufs.list, list) {
		if (buf->type == buffer_type &&
		    buf->mem->size >= bufreq.size)
			count++;
	}
	mutex_unlock(&inst->scratchbufs.lock);

	if (count != bufreq.count_actual)
		return 0;

	return buffer_type;
}

static int internal_set_buf_on_fw(struct vidc_inst *inst, u32 buffer_type,
				  struct vidc_mem *mem, bool reuse)
{
	struct device *dev = inst->core->dev;
	struct hfi_core *hfi = &inst->core->hfi;
	struct hfi_buffer_desc bd = {0};
	int ret;

	bd.buffer_size = mem->size;
	bd.buffer_type = buffer_type;
	bd.num_buffers = 1;
	bd.device_addr = mem->da;

	ret = vidc_hfi_session_set_buffers(hfi, inst->hfi_inst, &bd);
	if (ret) {
		dev_err(dev, "set session buffers failed\n");
		return ret;
	}

	return 0;
}

static int internal_alloc_and_set(struct vidc_inst *inst,
				  struct hfi_buffer_requirements *bufreq,
				  struct vidc_list *buf_list)
{
	struct vidc_internal_buf *buf;
	struct vidc_mem *mem;
	unsigned int i;
	int ret = 0;

	if (!bufreq->size)
		return 0;

	for (i = 0; i < bufreq->count_actual; i++) {
		mem = mem_alloc(inst->core->dev, bufreq->size, 0);
		if (IS_ERR(mem)) {
			ret = PTR_ERR(mem);
			goto err_no_mem;
		}

		buf = kzalloc(sizeof(*buf), GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto fail_kzalloc;
		}

		buf->mem = mem;
		buf->type = bufreq->type;

		ret = internal_set_buf_on_fw(inst, bufreq->type, mem, false);
		if (ret)
			goto fail_set_buffers;

		mutex_lock(&buf_list->lock);
		list_add_tail(&buf->list, &buf_list->list);
		mutex_unlock(&buf_list->lock);
	}

	return ret;

fail_set_buffers:
	kfree(buf);
fail_kzalloc:
	mem_free(mem);
err_no_mem:
	return ret;
}

static bool scratch_reuse_buffer(struct vidc_inst *inst, u32 buffer_type)
{
	struct device *dev = inst->core->dev;
	struct vidc_internal_buf *buf;
	bool reused = false;
	int ret = 0;

	mutex_lock(&inst->scratchbufs.lock);
	list_for_each_entry(buf, &inst->scratchbufs.list, list) {
		if (buf->type != buffer_type)
			continue;

		ret = internal_set_buf_on_fw(inst, buffer_type, buf->mem, true);
		if (ret) {
			dev_err(dev, "set internal buffers failed\n");
			reused = false;
			break;
		}

		reused = true;
	}
	mutex_unlock(&inst->scratchbufs.lock);

	return reused;
}

static int scratch_set_buffer(struct vidc_inst *inst, u32 type)
{
	struct hfi_buffer_requirements bufreq;
	int ret;

	ret = vidc_buf_descs(inst, type, &bufreq);
	if (ret)
		return 0;

	if (scratch_reuse_buffer(inst, type))
		return 0;

	return internal_alloc_and_set(inst, &bufreq, &inst->scratchbufs);
}

static int persist_set_buffer(struct vidc_inst *inst, u32 type)
{
	struct hfi_buffer_requirements bufreq;
	int ret;

	ret = vidc_buf_descs(inst, type, &bufreq);
	if (ret)
		return 0;

	mutex_lock(&inst->persistbufs.lock);
	if (!list_empty(&inst->persistbufs.list)) {
		mutex_unlock(&inst->persistbufs.lock);
		return 0;
	}
	mutex_unlock(&inst->persistbufs.lock);

	return internal_alloc_and_set(inst, &bufreq, &inst->persistbufs);
}

static int scratch_unset_buffers(struct vidc_inst *inst, bool reuse)
{
	struct hfi_core *hfi = &inst->core->hfi;
	struct vidc_internal_buf *buf, *n;
	struct hfi_buffer_desc bd = {0};
	u32 sufficient = 0;
	int ret = 0;

	mutex_lock(&inst->scratchbufs.lock);
	list_for_each_entry_safe(buf, n, &inst->scratchbufs.list, list) {
		bd.buffer_size = buf->mem->size;
		bd.buffer_type = buf->type;
		bd.num_buffers = 1;
		bd.device_addr = buf->mem->da;
		bd.response_required = true;

		ret = vidc_hfi_session_unset_buffers(hfi, inst->hfi_inst, &bd);

		/* If scratch buffers can be reused, do not free the buffers */
		if (reuse) {
			sufficient = scratch_buf_sufficient(inst, buf->type);
			if (sufficient == buf->type)
				continue;
		}

		list_del(&buf->list);
		mem_free(buf->mem);
		kfree(buf);
	}
	mutex_unlock(&inst->scratchbufs.lock);

	return ret;
}

static int persist_unset_buffers(struct vidc_inst *inst)
{
	struct hfi_core *hfi = &inst->core->hfi;
	struct vidc_internal_buf *buf, *n;
	struct hfi_buffer_desc bd = {0};
	int ret = 0;

	mutex_lock(&inst->persistbufs.lock);
	list_for_each_entry_safe(buf, n, &inst->persistbufs.list, list) {
		bd.buffer_size = buf->mem->size;
		bd.buffer_type = buf->type;
		bd.num_buffers = 1;
		bd.device_addr = buf->mem->da;
		bd.response_required = true;

		ret = vidc_hfi_session_unset_buffers(hfi, inst->hfi_inst, &bd);

		list_del(&buf->list);
		mem_free(buf->mem);
		kfree(buf);
	}
	mutex_unlock(&inst->persistbufs.lock);

	return ret;
}

static int scratch_set_buffers(struct vidc_inst *inst)
{
	struct device *dev = inst->core->dev;
	int ret;

	ret = scratch_unset_buffers(inst, true);
	if (ret)
		dev_warn(dev, "Failed to release scratch buffers\n");

	ret = scratch_set_buffer(inst, HFI_BUFFER_INTERNAL_SCRATCH);
	if (ret)
		goto error;

	ret = scratch_set_buffer(inst, HFI_BUFFER_INTERNAL_SCRATCH_1);
	if (ret)
		goto error;

	ret = scratch_set_buffer(inst, HFI_BUFFER_INTERNAL_SCRATCH_2);
	if (ret)
		goto error;

	return 0;
error:
	scratch_unset_buffers(inst, false);
	return ret;
}

static int persist_set_buffers(struct vidc_inst *inst)
{
	int ret;

	ret = persist_set_buffer(inst, HFI_BUFFER_INTERNAL_PERSIST);
	if (ret)
		goto error;

	ret = persist_set_buffer(inst, HFI_BUFFER_INTERNAL_PERSIST_1);
	if (ret)
		goto error;

	return 0;

error:
	persist_unset_buffers(inst);
	return ret;
}

int internal_bufs_alloc(struct vidc_inst *inst)
{
	struct device *dev = inst->core->dev;
	int ret;

	ret = scratch_set_buffers(inst);
	if (ret) {
		dev_err(dev, "set scratch buffers (%d)\n", ret);
		return ret;
	}

	ret = persist_set_buffers(inst);
	if (ret) {
		dev_err(dev, "set persist buffers (%d)\n", ret);
		goto error;
	}

	return 0;

error:
	scratch_unset_buffers(inst, false);
	return ret;
}

int internal_bufs_free(struct vidc_inst *inst)
{
	struct device *dev = inst->core->dev;
	int ret;

	ret = scratch_unset_buffers(inst, false);
	if (ret)
		dev_err(dev, "failed to release scratch buffers: %d\n", ret);

	ret = persist_unset_buffers(inst);
	if (ret)
		dev_err(dev, "failed to release persist buffers: %d\n", ret);

	return ret;
}
