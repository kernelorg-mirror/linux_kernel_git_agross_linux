/*
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
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
#ifndef __VIDC_RESOURCES_H__
#define __VIDC_RESOURCES_H__

#define VIDC_CLKS_NUM_MAX	7

struct freq_tbl {
	unsigned int load;
	unsigned long freq;
};

struct reg_val {
	u32 reg;
	u32 value;
};

struct vidc_resources {
	u64 dma_mask;
	const struct freq_tbl *freq_tbl;
	unsigned int freq_tbl_size;
	const struct reg_val *reg_tbl;
	unsigned int reg_tbl_size;
	const char *clks[VIDC_CLKS_NUM_MAX];
	unsigned int clks_num;
	unsigned int hfi_version;
	u32 max_load;
	unsigned int vmem_id;
	u32 vmem_size;
	u32 vmem_addr;
};

extern const struct vidc_resources msm8916_res;
#endif
