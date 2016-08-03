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
#ifndef __HFI_H__
#define __HFI_H__

#include <linux/interrupt.h>

#include "resources.h"
#include "hfi_helper.h"

#define VIDC_SESSION_TYPE_VPE			0
#define VIDC_SESSION_TYPE_ENC			1
#define VIDC_SESSION_TYPE_DEC			2

/* core capabilities */
#define VIDC_ENC_ROTATION_CAPABILITY		0x1
#define VIDC_ENC_SCALING_CAPABILITY		0x2
#define VIDC_ENC_DEINTERLACE_CAPABILITY		0x4
#define VIDC_DEC_MULTI_STREAM_CAPABILITY	0x8

#define VIDC_RESOURCE_NONE			0
#define VIDC_RESOURCE_OCMEM			1
#define VIDC_RESOURCE_VMEM			2

struct hfi_buffer_desc {
	u32 buffer_type;
	u32 buffer_size;
	u32 num_buffers;
	u32 device_addr;
	u32 extradata_addr;
	u32 extradata_size;
	u32 response_required;
};

struct hfi_frame_data {
	u32 buffer_type;
	u32 device_addr;
	u32 extradata_addr;
	u64 timestamp;
	u32 flags;
	u32 offset;
	u32 alloc_len;
	u32 filled_len;
	u32 mark_target;
	u32 mark_data;
	u32 clnt_data;
	u32 extradata_size;
};

union hfi_get_property {
	struct hfi_profile_level profile_level;
	struct hfi_buffer_requirements bufreq[HFI_BUFFER_TYPE_MAX];
};

/* HFI events */
#define EVT_SYS_EVENT_CHANGE			1
#define EVT_SYS_WATCHDOG_TIMEOUT		2
#define EVT_SYS_ERROR				3
#define EVT_SESSION_ERROR			4

/* HFI event callback structure */
struct hfi_event_data {
	u32 error;
	u32 height;
	u32 width;
	u32 event_type;
	u32 packet_buffer;
	u32 extradata_buffer;
	u32 profile;
	u32 level;
};

/* define core states */
#define CORE_UNINIT				0
#define CORE_INIT				1
#define CORE_INVALID				2

/* define instance states */
#define INST_INVALID				1
#define INST_UNINIT				2
#define INST_INIT				3
#define INST_LOAD_RESOURCES			4
#define INST_START				5
#define INST_STOP				6
#define INST_RELEASE_RESOURCES			7

#define call_hfi_op(hfi, op, args...)	\
	(((hfi) && (hfi)->ops && (hfi)->ops->op) ?	\
	((hfi)->ops->op(args)) : 0)

struct hfi_core;
struct hfi_inst;

struct hfi_core_ops {
	int (*event_notify)(struct hfi_core *hfi, u32 event);
};

struct hfi_inst_ops {
	int (*empty_buf_done)(struct hfi_inst *inst, u32 addr, u32 bytesused,
			      u32 data_offset, u32 flags);
	int (*fill_buf_done)(struct hfi_inst *inst, u32 addr, u32 bytesused,
			     u32 data_offset, u32 flags, struct timeval *ts);
	int (*event_notify)(struct hfi_inst *inst, u32 event,
			    struct hfi_event_data *data);
};

struct hfi_inst {
	struct list_head list;
	struct mutex lock;
	unsigned int state;
	struct completion done;
	unsigned int error;

	/* instance operations passed by outside world */
	const struct hfi_inst_ops *ops;
	void *ops_priv;

	void *priv;

	u32 session_type;
	union hfi_get_property hprop;

	/* capabilities filled by session_init */
	struct hfi_capability width;
	struct hfi_capability height;
	struct hfi_capability mbs_per_frame;
	struct hfi_capability mbs_per_sec;
	struct hfi_capability framerate;
	struct hfi_capability scale_x;
	struct hfi_capability scale_y;
	struct hfi_capability bitrate;
	struct hfi_capability hier_p;
	struct hfi_capability ltr_count;
	struct hfi_capability secure_output2_threshold;
	bool alloc_mode_static;
	bool alloc_mode_dynamic;

	/* profile & level pairs supported */
	unsigned int pl_count;
	struct hfi_profile_level pl[HFI_MAX_PROFILE_COUNT];

	/* buffer requirements */
	struct hfi_buffer_requirements bufreq[HFI_BUFFER_TYPE_MAX];
};

struct hfi_core {
	struct device *dev;	/* mostly used for dev_xxx */

	struct mutex lock;
	unsigned int state;
	struct completion done;
	unsigned int error;

	/*
	 * list of 'struct hfi_inst' instances which belong to
	 * this hfi core device
	 */
	struct list_head instances;

	/* core operations passed by outside world */
	const struct hfi_core_ops *core_ops;

	/* filled by sys core init */
	u32 enc_codecs;
	u32 dec_codecs;

	/* core capabilities */
	unsigned int core_caps;

	/* internal hfi operations */
	void *priv;
	const struct hfi_ops *ops;
	const struct hfi_packetization_ops *pkt_ops;
	enum hfi_packetization_type packetization_type;
};

struct hfi_ops {
	int (*core_init)(struct hfi_core *hfi);
	int (*core_deinit)(struct hfi_core *hfi);
	int (*core_ping)(struct hfi_core *hfi, u32 cookie);
	int (*core_trigger_ssr)(struct hfi_core *hfi, u32 trigger_type);

	int (*session_init)(struct hfi_core *hfi, struct hfi_inst *inst,
			    u32 session_type, u32 codec);
	int (*session_end)(struct hfi_inst *inst);
	int (*session_abort)(struct hfi_inst *inst);
	int (*session_flush)(struct hfi_inst *inst, u32 flush_mode);
	int (*session_start)(struct hfi_inst *inst);
	int (*session_stop)(struct hfi_inst *inst);
	int (*session_etb)(struct hfi_inst *inst,
			   struct hfi_frame_data *input_frame);
	int (*session_ftb)(struct hfi_inst *inst,
			   struct hfi_frame_data *output_frame);
	int (*session_set_buffers)(struct hfi_inst *inst,
				   struct hfi_buffer_desc *bd);
	int (*session_release_buffers)(struct hfi_inst *inst,
				       struct hfi_buffer_desc *bd);
	int (*session_load_res)(struct hfi_inst *inst);
	int (*session_release_res)(struct hfi_inst *inst);
	int (*session_parse_seq_hdr)(struct hfi_inst *inst, u32 seq_hdr,
				     u32 seq_hdr_len);
	int (*session_get_seq_hdr)(struct hfi_inst *inst, u32 seq_hdr,
				   u32 seq_hdr_len);
	int (*session_set_property)(struct hfi_inst *inst, u32 ptype,
				    void *pdata);
	int (*session_get_property)(struct hfi_inst *inst, u32 ptype);

	int (*resume)(struct hfi_core *hfi);
	int (*suspend)(struct hfi_core *hfi);

	/* interrupt operations */
	irqreturn_t (*isr)(int irq, struct hfi_core *hfi);
	irqreturn_t (*isr_thread)(int irq, struct hfi_core *hfi);
};

static inline void *to_hfi_priv(struct hfi_core *hfi)
{
	return hfi->priv;
}

int vidc_hfi_create(struct hfi_core *hfi, const struct vidc_resources *res,
		    void __iomem *base);
void vidc_hfi_destroy(struct hfi_core *hfi);

int vidc_hfi_core_init(struct hfi_core *hfi);
int vidc_hfi_core_deinit(struct hfi_core *hfi);
int vidc_hfi_core_suspend(struct hfi_core *hfi);
int vidc_hfi_core_resume(struct hfi_core *hfi);
int vidc_hfi_core_trigger_ssr(struct hfi_core *hfi, u32 type);
int vidc_hfi_core_ping(struct hfi_core *hfi);

struct hfi_inst *vidc_hfi_session_create(struct hfi_core *hfi,
					 const struct hfi_inst_ops *ops,
					 void *ops_priv);
void vidc_hfi_session_destroy(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_init(struct hfi_core *hfi, struct hfi_inst *inst,
			  u32 pixfmt, u32 session_type);
int vidc_hfi_session_deinit(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_start(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_stop(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_abort(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_load_res(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_unload_res(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_flush(struct hfi_core *hfi, struct hfi_inst *inst);
int vidc_hfi_session_set_buffers(struct hfi_core *hfi, struct hfi_inst *inst,
				 struct hfi_buffer_desc *bd);
int vidc_hfi_session_unset_buffers(struct hfi_core *hfi, struct hfi_inst *inst,
				   struct hfi_buffer_desc *bd);
int vidc_hfi_session_get_property(struct hfi_core *hfi, struct hfi_inst *inst,
				  u32 ptype, union hfi_get_property *hprop);
int vidc_hfi_session_set_property(struct hfi_core *hfi, struct hfi_inst *inst,
				  u32 ptype, void *pdata);
int vidc_hfi_session_etb(struct hfi_core *hfi, struct hfi_inst *inst,
			 struct hfi_frame_data *fdata);
int vidc_hfi_session_ftb(struct hfi_core *hfi, struct hfi_inst *inst,
			 struct hfi_frame_data *fdata);
irqreturn_t vidc_hfi_isr_thread(int irq, void *dev_id);
irqreturn_t vidc_hfi_isr(int irq, void *dev);

#endif
