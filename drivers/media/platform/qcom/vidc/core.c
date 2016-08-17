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
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/remoteproc.h>
#include <linux/pm_runtime.h>
#include <media/videobuf2-v4l2.h>
#include <media/v4l2-ioctl.h>

#include "core.h"
#include "resources.h"
#include "vdec.h"
#include "venc.h"

static void vidc_add_inst(struct vidc_core *core, struct vidc_inst *inst)
{
	mutex_lock(&core->lock);
	list_add_tail(&inst->list, &core->instances);
	mutex_unlock(&core->lock);
}

static void vidc_del_inst(struct vidc_core *core, struct vidc_inst *inst)
{
	struct vidc_inst *pos, *n;

	mutex_lock(&core->lock);
	list_for_each_entry_safe(pos, n, &core->instances, list) {
		if (pos == inst)
			list_del(&inst->list);
	}
	mutex_unlock(&core->lock);
}

static int vidc_rproc_boot(struct vidc_core *core)
{
	int ret;

	if (core->rproc_booted)
		return 0;

	ret = rproc_boot(core->rproc);
	if (ret)
		return ret;

	core->rproc_booted = true;

	return 0;
}

static void vidc_rproc_shutdown(struct vidc_core *core)
{
	if (!core->rproc_booted)
		return;

	rproc_shutdown(core->rproc);
	core->rproc_booted = false;
}

struct vidc_sys_error {
	struct vidc_core *core;
	struct delayed_work work;
};

static void vidc_sys_error_handler(struct work_struct *work)
{
	struct vidc_sys_error *handler =
		container_of(work, struct vidc_sys_error, work.work);
	struct vidc_core *core = handler->core;
	struct hfi_core *hfi = &core->hfi;
	struct device *dev = core->dev;
	int ret;

	mutex_lock(&hfi->lock);
	if (hfi->state != CORE_INVALID)
		goto exit;

	mutex_unlock(&hfi->lock);

	ret = vidc_hfi_core_deinit(hfi);
	if (ret)
		dev_err(dev, "core: deinit failed (%d)\n", ret);

	mutex_lock(&hfi->lock);

	rproc_report_crash(core->rproc, RPROC_FATAL_ERROR);

	vidc_rproc_shutdown(core);

	ret = vidc_rproc_boot(core);
	if (ret)
		goto exit;

	hfi->state = CORE_INIT;

exit:
	mutex_unlock(&hfi->lock);
	kfree(handler);
}

static int vidc_event_notify(struct hfi_core *hfi, u32 event)
{
	struct vidc_sys_error *handler;
	struct hfi_inst *inst;

	switch (event) {
	case EVT_SYS_WATCHDOG_TIMEOUT:
	case EVT_SYS_ERROR:
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&hfi->lock);

	hfi->state = CORE_INVALID;

	list_for_each_entry(inst, &hfi->instances, list) {
		mutex_lock(&inst->lock);
		inst->state = INST_INVALID;
		mutex_unlock(&inst->lock);
	}

	mutex_unlock(&hfi->lock);

	handler = kzalloc(sizeof(*handler), GFP_KERNEL);
	if (!handler)
		return -ENOMEM;

	handler->core = container_of(hfi, struct vidc_core, hfi);
	INIT_DELAYED_WORK(&handler->work, vidc_sys_error_handler);

	/*
	 * Sleep for 5 sec to ensure venus has completed any
	 * pending cache operations. Without this sleep, we see
	 * device reset when firmware is unloaded after a sys
	 * error.
	 */
	schedule_delayed_work(&handler->work, msecs_to_jiffies(5000));

	return 0;
}

static const struct hfi_core_ops vidc_core_ops = {
	.event_notify = vidc_event_notify,
};

static int vidc_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct vidc_core *core = video_drvdata(file);
	struct vidc_inst *inst;
	int ret = 0;

	inst = kzalloc(sizeof(*inst), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	mutex_init(&inst->lock);

	INIT_VIDC_LIST(&inst->scratchbufs);
	INIT_VIDC_LIST(&inst->persistbufs);
	INIT_VIDC_LIST(&inst->registeredbufs);

	INIT_LIST_HEAD(&inst->bufqueue);
	mutex_init(&inst->bufqueue_lock);

	if (vdev == &core->vdev_dec)
		inst->session_type = VIDC_SESSION_TYPE_DEC;
	else
		inst->session_type = VIDC_SESSION_TYPE_ENC;

	inst->core = core;

	if (inst->session_type == VIDC_SESSION_TYPE_DEC)
		ret = vdec_open(inst);
	else
		ret = venc_open(inst);

	if (ret)
		goto err_free_inst;

	if (inst->session_type == VIDC_SESSION_TYPE_DEC)
		v4l2_fh_init(&inst->fh, &core->vdev_dec);
	else
		v4l2_fh_init(&inst->fh, &core->vdev_enc);

	inst->fh.ctrl_handler = &inst->ctrl_handler;

	v4l2_fh_add(&inst->fh);

	file->private_data = &inst->fh;

	vidc_add_inst(core, inst);

	return 0;

err_free_inst:
	kfree(inst);
	return ret;
}

static int vidc_close(struct file *file)
{
	struct vidc_inst *inst = to_inst(file);
	struct vidc_core *core = inst->core;

	if (inst->session_type == VIDC_SESSION_TYPE_DEC)
		vdec_close(inst);
	else
		venc_close(inst);

	vidc_del_inst(core, inst);

	mutex_destroy(&inst->bufqueue_lock);
	mutex_destroy(&inst->scratchbufs.lock);
	mutex_destroy(&inst->persistbufs.lock);
	mutex_destroy(&inst->registeredbufs.lock);

	v4l2_fh_del(&inst->fh);
	v4l2_fh_exit(&inst->fh);

	kfree(inst);
	return 0;
}

static unsigned int vidc_poll(struct file *file, struct poll_table_struct *pt)
{
	struct vidc_inst *inst = to_inst(file);
	struct vb2_queue *outq = &inst->bufq_out;
	struct vb2_queue *capq = &inst->bufq_cap;
	unsigned int ret;

	ret = vb2_poll(outq, file, pt);
	ret |= vb2_poll(capq, file, pt);

	return ret;
}

static int vidc_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vidc_inst *inst = to_inst(file);
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	int ret;

	if (offset < DST_QUEUE_OFF_BASE) {
		ret = vb2_mmap(&inst->bufq_out, vma);
	} else {
		vma->vm_pgoff -= DST_QUEUE_OFF_BASE >> PAGE_SHIFT;
		ret = vb2_mmap(&inst->bufq_cap, vma);
	}

	return ret;
}

const struct v4l2_file_operations vidc_fops = {
	.owner = THIS_MODULE,
	.open = vidc_open,
	.release = vidc_close,
	.unlocked_ioctl = video_ioctl2,
	.poll = vidc_poll,
	.mmap = vidc_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = v4l2_compat_ioctl32,
#endif
};

static irqreturn_t vidc_isr_thread(int irq, void *dev_id)
{
	return vidc_hfi_isr_thread(irq, dev_id);
}

static irqreturn_t vidc_isr(int irq, void *dev)
{
	return vidc_hfi_isr(irq, dev);
}

static int vidc_clks_get(struct vidc_core *core, unsigned int clks_num,
			 const char * const *clks_id)
{
	struct device *dev = core->dev;
	unsigned int i;

	for (i = 0; i < clks_num; i++) {
		core->clks[i] = devm_clk_get(dev, clks_id[i]);
		if (IS_ERR(core->clks[i]))
			return PTR_ERR(core->clks[i]);
	}

	return 0;
}

static int
vidc_clks_enable(struct vidc_core *core, const struct vidc_resources *res)
{
	unsigned int i;
	int ret;

	for (i = 0; i < res->clks_num; i++) {
		ret = clk_prepare_enable(core->clks[i]);
		if (ret)
			goto err;
	}

	return 0;
err:
	while (--i)
		clk_disable_unprepare(core->clks[i]);

	return ret;
}

static void
vidc_clks_disable(struct vidc_core *core, const struct vidc_resources *res)
{
	unsigned int i;

	for (i = 0; i < res->clks_num; i++)
		clk_disable_unprepare(core->clks[i]);
}

static const struct of_device_id vidc_dt_match[] = {
	{ .compatible = "qcom,vidc-msm8916", .data = &msm8916_res, },
	{ }
};

MODULE_DEVICE_TABLE(of, vidc_dt_match);

static int vidc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vidc_core *core;
	struct device_node *rproc;
	struct resource *r;
	int ret;

	core = devm_kzalloc(dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	core->dev = dev;
	platform_set_drvdata(pdev, core);

	rproc = of_parse_phandle(dev->of_node, "rproc", 0);
	if (IS_ERR(rproc))
		return PTR_ERR(rproc);

	core->rproc = rproc_get_by_phandle(rproc->phandle);
	if (IS_ERR(core->rproc))
		return PTR_ERR(core->rproc);
	else if (!core->rproc)
		return -EPROBE_DEFER;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	core->base = devm_ioremap_resource(dev, r);
	if (IS_ERR(core->base))
		return PTR_ERR(core->base);

	core->irq = platform_get_irq(pdev, 0);
	if (core->irq < 0)
		return core->irq;

	core->res = of_device_get_match_data(dev);
	if (!core->res)
		return -ENODEV;

	ret = vidc_clks_get(core, core->res->clks_num, core->res->clks);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, core->res->dma_mask);
	if (ret)
		return ret;

	INIT_LIST_HEAD(&core->instances);
	mutex_init(&core->lock);

	ret = devm_request_threaded_irq(dev, core->irq, vidc_isr,
					vidc_isr_thread,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
					"vidc", &core->hfi);
	if (ret)
		return ret;

	core->hfi.core_ops = &vidc_core_ops;
	core->hfi.dev = dev;

	ret = vidc_hfi_create(&core->hfi, core->res, core->base);
	if (ret)
		return ret;

	ret = vidc_clks_enable(core, core->res);
	if (ret)
		goto err_hfi_destroy;

	ret = vidc_rproc_boot(core);
	if (ret) {
		vidc_clks_disable(core, core->res);
		goto err_hfi_destroy;
	}

	pm_runtime_enable(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0)
		goto err_runtime_disable;

	ret = vidc_hfi_core_init(&core->hfi);
	if (ret)
		goto err_rproc_shutdown;

	ret = pm_runtime_put_sync(dev);
	if (ret)
		goto err_core_deinit;

	vidc_clks_disable(core, core->res);

	ret = v4l2_device_register(dev, &core->v4l2_dev);
	if (ret)
		goto err_core_deinit;

	ret = vdec_init(core, &core->vdev_dec);
	if (ret)
		goto err_dev_unregister;

	ret = venc_init(core, &core->vdev_enc);
	if (ret)
		goto err_vdec_deinit;

	return 0;

err_vdec_deinit:
	vdec_deinit(core, &core->vdev_dec);
err_dev_unregister:
	v4l2_device_unregister(&core->v4l2_dev);
err_core_deinit:
	vidc_hfi_core_deinit(&core->hfi);
err_rproc_shutdown:
	vidc_rproc_shutdown(core);
err_runtime_disable:
	pm_runtime_set_suspended(dev);
	pm_runtime_disable(dev);
err_hfi_destroy:
	vidc_hfi_destroy(&core->hfi);
	return ret;
}

static int vidc_remove(struct platform_device *pdev)
{
	struct vidc_core *core = platform_get_drvdata(pdev);
	int ret;

	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0)
		return ret;

	ret = vidc_hfi_core_deinit(&core->hfi);
	if (ret) {
		pm_runtime_put_sync(&pdev->dev);
		return ret;
	}

	vidc_rproc_shutdown(core);

	ret = pm_runtime_put_sync(&pdev->dev);

	vidc_hfi_destroy(&core->hfi);
	vdec_deinit(core, &core->vdev_dec);
	venc_deinit(core, &core->vdev_enc);
	v4l2_device_unregister(&core->v4l2_dev);

	pm_runtime_disable(core->dev);

	return ret < 0 ? ret : 0;
}

static int vidc_runtime_suspend(struct device *dev)
{
	struct vidc_core *core = dev_get_drvdata(dev);
	int ret;

	ret = vidc_hfi_core_suspend(&core->hfi);

	vidc_clks_disable(core, core->res);

	return ret;
}

static int vidc_runtime_resume(struct device *dev)
{
	struct vidc_core *core = dev_get_drvdata(dev);
	int ret;

	ret = vidc_clks_enable(core, core->res);
	if (ret)
		return ret;

	return vidc_hfi_core_resume(&core->hfi);
}

static int vidc_pm_suspend(struct device *dev)
{
	return vidc_runtime_suspend(dev);
}

static int vidc_pm_resume(struct device *dev)
{
	return vidc_runtime_resume(dev);
}

static const struct dev_pm_ops vidc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(vidc_pm_suspend, vidc_pm_resume)
	SET_RUNTIME_PM_OPS(vidc_runtime_suspend, vidc_runtime_resume, NULL)
};

static struct platform_driver qcom_vidc_driver = {
	.probe = vidc_probe,
	.remove = vidc_remove,
	.driver = {
		.name = "qcom-vidc",
		.of_match_table = vidc_dt_match,
		.pm = &vidc_pm_ops,
	},
};

module_platform_driver(qcom_vidc_driver);

MODULE_ALIAS("platform:qcom-vidc");
MODULE_DESCRIPTION("Qualcomm video encoder and decoder driver");
MODULE_LICENSE("GPL v2");
