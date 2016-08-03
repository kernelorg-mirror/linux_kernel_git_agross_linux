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

	memcpy_toio(venus->mem_region, fw->data, fw->size);

	ret = qcom_scm_pas_init_image(VENUS_PAS_ID, venus->mem_phys, fw->size);
	if (ret) {
		dev_err(&rproc->dev, "invalid firmware metadata (%d)\n", ret);
		return -EINVAL;
	}

	memset(venus->mem_region, 0, fw->size);

	ret = qcom_mdt_parse(fw, &fw_addr, &fw_size, &relocate);
	if (ret) {
		dev_err(&rproc->dev, "failed to parse mdt header (%d)\n", ret);
		return ret;
	}

	venus->fw_addr = fw_addr;

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
	u64 offset;

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

static int venus_alloc_memory_region(struct qcom_venus *venus)
{
	struct device_node *node;
	struct resource r;
	int ret;

	node = of_parse_phandle(venus->dev->of_node, "memory-region", 0);
	if (!node) {
		dev_err(venus->dev, "no memory-region specified\n");
		return -EINVAL;
	}

	ret = of_address_to_resource(node, 0, &r);
	if (ret)
		return ret;

	venus->mem_phys = r.start;
	venus->mem_size = resource_size(&r);
	venus->mem_region = devm_ioremap_wc(venus->dev, venus->mem_phys,
					    venus->mem_size);
	if (!venus->mem_region) {
		dev_err(venus->dev, "unable to map memory region: %pa+%zx\n",
			&r.start, venus->mem_size);
		return -EBUSY;
	}

	return 0;
}

#define SCM_SVC_MP		0xc
#define MAXIMUM_VIRT_SIZE	(300 * SZ_1M)
#define MAKE_VERSION(major, minor, patch) \
	(((major & 0x3FF) << 22) | ((minor & 0x3FF) << 12) | (patch & 0xFFF))

static int venus_iommu_sec_ptbl_init(struct device *dev)
{
	int psize[2] = {0, 0};
	int version;
	void *cpu_addr;
	dma_addr_t paddr;
	unsigned long attrs;
	static bool allocated = false;
	int ret;

	if (allocated)
		return 0;

	version = qcom_scm_get_feat_version(SCM_SVC_MP);

	if (version >= MAKE_VERSION(1, 1, 1)) {
		ret = qcom_scm_iommu_set_cp_pool_size(MAXIMUM_VIRT_SIZE, 0);
		if (ret) {
			dev_err(dev, "failed setting max virtual size (%d)\n",
				ret);
			return ret;
		}
	}

	ret = qcom_scm_iommu_secure_ptbl_size(0, psize);
	if (ret) {
		dev_err(dev, "failed to get iommu secure pgtable size (%d)\n",
			ret);
		return ret;
	}

	if (psize[1]) {
		dev_err(dev, "failed to get iommu secure pgtable size (%d)\n",
			ret);
		return psize[1];
	}

	dev_info(dev, "iommu sec: pgtable size: %d\n", psize[0]);

	attrs = DMA_ATTR_NO_KERNEL_MAPPING;

	cpu_addr = dma_alloc_attrs(dev, psize[0], &paddr, GFP_KERNEL, attrs);
	if (!cpu_addr) {
		dev_err(dev, "failed to allocate %d bytes for pgtable\n",
			psize[0]);
		return -ENOMEM;
	}

	ret = qcom_scm_iommu_secure_ptbl_init(paddr, psize[0], 0);
	if (ret) {
		dev_err(dev, "failed to init iommu pgtable (%d)\n", ret);
		goto free_mem;
	}

	allocated = true;

	return 0;

free_mem:
	dma_free_attrs(dev, psize[0], cpu_addr, paddr, attrs);
	return ret;
}

static int venus_probe(struct platform_device *pdev)
{
	struct qcom_venus *venus;
	struct rproc *rproc;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	if (!qcom_scm_is_available())
		return -EPROBE_DEFER;

	if (!qcom_scm_pas_supported(VENUS_PAS_ID)) {
		dev_err(&pdev->dev, "PAS is not available for venus\n");
		return -ENXIO;
	}

	rproc = rproc_alloc(&pdev->dev, pdev->name, &venus_ops,
			    VENUS_FIRMWARE_NAME, sizeof(*venus));
	if (!rproc) {
		dev_err(&pdev->dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	rproc->fw_ops = &venus_fw_ops;

	venus = rproc->priv;
	venus->dev = &pdev->dev;
	venus->rproc = rproc;
	platform_set_drvdata(pdev, venus);

	ret = venus_alloc_memory_region(venus);
	if (ret)
		goto free_rproc;

	ret = venus_iommu_sec_ptbl_init(&pdev->dev);
	if (ret)
		goto free_rproc;

	ret = rproc_add(rproc);
	if (ret)
		goto free_rproc;

	return 0;

free_rproc:
	rproc_put(rproc);

	return ret;
}

static int venus_remove(struct platform_device *pdev)
{
	struct qcom_venus *venus = platform_get_drvdata(pdev);

	rproc_del(venus->rproc);
	rproc_put(venus->rproc);

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
