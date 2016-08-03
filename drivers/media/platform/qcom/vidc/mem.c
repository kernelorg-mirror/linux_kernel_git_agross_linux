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

#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/slab.h>

#include "mem.h"

struct vidc_mem *mem_alloc(struct device *dev, size_t size, int map_kernel)
{
	struct vidc_mem *mem;

	if (!size)
		return ERR_PTR(-EINVAL);

	if (IS_ERR(dev))
		return ERR_CAST(dev);

	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	mem->size = ALIGN(size, SZ_4K);
	mem->iommu_dev = dev;

	mem->attrs = DMA_ATTR_WRITE_COMBINE;

	if (!map_kernel)
		mem->attrs |= DMA_ATTR_NO_KERNEL_MAPPING;

	mem->kvaddr = dma_alloc_attrs(mem->iommu_dev, mem->size, &mem->da,
				      GFP_KERNEL, mem->attrs);
	if (!mem->kvaddr) {
		kfree(mem);
		return ERR_PTR(-ENOMEM);
	}

	return mem;
}

void mem_free(struct vidc_mem *mem)
{
	if (!mem)
		return;

	dma_free_attrs(mem->iommu_dev, mem->size, mem->kvaddr,
	       mem->da, mem->attrs);
	kfree(mem);
};
