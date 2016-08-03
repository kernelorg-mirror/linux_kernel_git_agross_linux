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

#ifndef __VIDC_SMEM_H__
#define __VIDC_SMEM_H__

struct device;

struct vidc_mem {
	size_t size;
	void *kvaddr;
	dma_addr_t da;
	unsigned long attrs;
	struct device *iommu_dev;
};

struct vidc_mem *mem_alloc(struct device *dev, size_t size, int map_kernel);
void mem_free(struct vidc_mem *mem);

#endif /* __VIDC_SMEM_H__ */
