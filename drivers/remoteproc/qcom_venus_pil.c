/*
 * Qualcomm Venus Peripheral Image Loader
 *
 * Copyright (C) 2016 Linaro Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <linux/remoteproc.h>

#include "qcom_mdt_loader.h"
#include "remoteproc_internal.h"

#define VENUS_CRASH_REASON_SMEM		425
#define VENUS_FIRMWARE_NAME		"venus.mdt"
#define VENUS_PAS_ID			9

struct qcom_venus {
	struct device *dev;
	struct rproc *rproc;
	phys_addr_t fw_addr;
	phys_addr_t mem_phys;
	void *mem_region;
	size_t mem_size;
};

static int venus_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_venus *venus = rproc->priv;
	phys_addr_t fw_addr;
	size_t fw_size;
	bool relocate;
	int ret;

	ret = qcom_scm_pas_init_image(VENUS_PAS_ID, fw->data, fw->size);
	if (ret) {
		dev_err(&rproc->dev, "invalid firmware metadata (%d)\n", ret);
		return -EINVAL;
	}

	ret = qcom_mdt_parse(fw, &fw_addr, &fw_size, &relocate);
	if (ret) {
		dev_err(&rproc->dev, "failed to parse mdt header (%d)\n", ret);
		return ret;
	}

	venus->fw_addr = fw_addr;

	dev_err(&rproc->dev, "whole fw size is %zu\n", fw_size);

	ret = qcom_scm_pas_mem_setup(VENUS_PAS_ID,
				     relocate ? venus->mem_phys : fw_addr,
				     fw_size);
	if (ret) {
		dev_err(&rproc->dev, "unable to setup memory (%d)\n",
			ret);
		return -EINVAL;
	}

	return qcom_mdt_load(rproc, fw, VENUS_FIRMWARE_NAME);
}

static const struct rproc_fw_ops venus_fw_ops = {
	.find_rsc_table = qcom_mdt_find_rsc_table,
	.load = venus_load,
};

static int venus_start(struct rproc *rproc)
{
	struct qcom_venus *venus = rproc->priv;
	int ret;

	ret = qcom_scm_pas_auth_and_reset(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev,
			"authentication image and release reset failed (%d)\n",
			ret);

	return ret;
}

static int venus_stop(struct rproc *rproc)
{
	struct qcom_venus *venus = rproc->priv;
	int ret;

	ret = qcom_scm_pas_shutdown(VENUS_PAS_ID);
	if (ret)
		dev_err(venus->dev, "failed to shutdown: %d\n", ret);

	return ret;
}

static void *venus_da_to_va(struct rproc *rproc, u64 da, int len)
{
	struct qcom_venus *venus = rproc->priv;
	s64 offset;

	offset = da - venus->fw_addr;

	if (offset < 0 || offset + len > venus->mem_size)
		return NULL;

	return venus->mem_region + offset;
}

static const struct rproc_ops venus_ops = {
	.start = venus_start,
	.stop = venus_stop,
	.da_to_va = venus_da_to_va,
};

static int venus_iommu_sec_ptbl_init(struct qcom_venus *venus)
{
	struct device *dev = venus->dev;
	size_t size = 0;
	dma_addr_t paddr;
	int ret;

	ret = qcom_scm_iommu_secure_ptbl_size(0, &size);
	if (ret) {
		dev_err(dev, "failed to get iommu secure pgtable size (%d)\n",
			ret);
		return ret;
	}

	paddr = venus->mem_phys + 0x600000;

	dev_info(dev, "pgtable: pa:%pa, size:%zu, ret:%d\n", &paddr,
		 size, ret);

	ret = qcom_scm_iommu_secure_ptbl_init(paddr, size, 0);
	if (ret) {
		dev_err(dev, "failed to init iommu pgtable (%d)\n", ret);
		return ret;
	}

	return 0;
}

static int venus_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_venus *venus;
	ssize_t size = 0x600000;
	dma_addr_t dma_handle;
	struct rproc *rproc;
	void *va;
	int ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	if (!qcom_scm_pas_supported(VENUS_PAS_ID)) {
		dev_err(dev, "PAS is not available for venus\n");
		return -ENXIO;
	}

	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return ret;

	rproc = rproc_alloc(dev, pdev->name, &venus_ops, VENUS_FIRMWARE_NAME,
			    sizeof(*venus));
	if (!rproc) {
		dev_err(dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	rproc->fw_ops = &venus_fw_ops;

	venus = rproc->priv;
	venus->dev = dev;
	venus->rproc = rproc;

	platform_set_drvdata(pdev, venus);

	ret = dma_alloc_from_coherent(dev, size, &dma_handle, &va);
	if (!ret) {
		dev_err(dev, "dma alloc failed\n");
		return -ENOMEM;
	}

	dev_err(dev, "dma alloc %pa, %p, size %zu\n", &dma_handle, va, size);

	venus->mem_phys = dma_handle;
	venus->mem_size = size;
	venus->mem_region = va;

	ret = venus_iommu_sec_ptbl_init(venus);
	if (ret)
		goto free_rproc;

	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	return 0;

free_rproc:
	rproc_put(rproc);
	dma_release_from_coherent(dev, get_order(venus->mem_size),
				  venus->mem_region);
	of_reserved_mem_device_release(&pdev->dev);

	return ret;
}

static int venus_remove(struct platform_device *pdev)
{
	struct qcom_venus *venus = platform_get_drvdata(pdev);

	rproc_del(venus->rproc);
	rproc_put(venus->rproc);
	dma_release_from_coherent(&pdev->dev, get_order(venus->mem_size),
				  venus->mem_region);
	of_reserved_mem_device_release(&pdev->dev);

	return 0;
}

static const struct of_device_id venus_of_match[] = {
	{ .compatible = "qcom,venus-pil" },
	{ },
};

static struct platform_driver venus_driver = {
	.probe = venus_probe,
	.remove = venus_remove,
	.driver = {
		.name = "qcom-venus-pil",
		.of_match_table = venus_of_match,
	},
};

module_platform_driver(venus_driver);
MODULE_DESCRIPTION("Peripheral Image Loader for Venus");
MODULE_LICENSE("GPL v2");
