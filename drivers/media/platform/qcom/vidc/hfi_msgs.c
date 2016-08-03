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
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <media/videobuf2-v4l2.h>

#include "hfi.h"
#include "hfi_helper.h"
#include "hfi_msgs.h"

struct hfi_msg_fbd {
	u32 stream_id;
	u32 view_id;
	u32 timestamp_hi;
	u32 timestamp_lo;
	u32 flags1;
	u32 mark_target;
	u32 mark_data;
	u32 stats;
	u32 alloc_len1;
	u32 filled_len1;
	u32 offset1;
	u32 frame_width;
	u32 frame_height;
	u32 start_x_coord;
	u32 start_y_coord;
	u32 input_tag;
	u32 input_tag1;
	u32 picture_type;
	u32 packet_buffer1;
	u32 extradata_buffer;
	u32 flags2;
	u32 alloc_len2;
	u32 filled_len2;
	u32 offset2;
	u32 packet_buffer2;
	u32 flags3;
	u32 alloc_len3;
	u32 filled_len3;
	u32 offset3;
	u32 packet_buffer3;
	u32 buffer_type;
};

static struct hfi_inst *to_hfi_instance(struct hfi_core *hfi, u32 session_id)
{
	struct hfi_inst *inst;

	mutex_lock(&hfi->lock);
	list_for_each_entry(inst, &hfi->instances, list)
		if (hash32_ptr(inst) == session_id) {
			mutex_unlock(&hfi->lock);
			return inst;
		}
	mutex_unlock(&hfi->lock);

	return NULL;
}

static void event_seq_changed(struct hfi_core *hfi, struct hfi_inst *inst,
			      struct hfi_msg_event_notify_pkt *pkt)
{
	struct device *dev = hfi->dev;
	struct hfi_event_data event = {0};
	int num_properties_changed;
	struct hfi_framesize *frame_sz;
	struct hfi_profile_level *profile_level;
	u8 *data_ptr;
	u32 ptype;

	inst->error = HFI_ERR_NONE;

	switch (pkt->event_data1) {
	case HFI_EVENT_DATA_SEQUENCE_CHANGED_SUFFICIENT_BUF_RESOURCES:
	case HFI_EVENT_DATA_SEQUENCE_CHANGED_INSUFFICIENT_BUF_RESOURCES:
		break;
	default:
		inst->error = HFI_ERR_SESSION_INVALID_PARAMETER;
		goto done;
	}

	event.event_type = pkt->event_data1;

	num_properties_changed = pkt->event_data2;
	if (!num_properties_changed) {
		inst->error = HFI_ERR_SESSION_INSUFFICIENT_RESOURCES;
		goto done;
	}

	data_ptr = (u8 *) &pkt->ext_event_data[0];
	do {
		ptype = *((u32 *)data_ptr);
		switch (ptype) {
		case HFI_PROPERTY_PARAM_FRAME_SIZE:
			data_ptr += sizeof(u32);
			frame_sz = (struct hfi_framesize *) data_ptr;
			event.width = frame_sz->width;
			event.height = frame_sz->height;
			data_ptr += sizeof(frame_sz);
			dev_dbg(dev, "%s cmd: frame size: %ux%u\n",
				__func__, event.width, event.height);
			break;
		case HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT:
			data_ptr += sizeof(u32);
			profile_level = (struct hfi_profile_level *) data_ptr;
			event.profile = profile_level->profile;
			event.level = profile_level->level;
			data_ptr += sizeof(profile_level);
			dev_dbg(dev, "%s cmd: profile-level: %u - %u\n",
				__func__, event.profile, event.level);
			break;
		default:
			dev_dbg(dev, "%s cmd: %#x not supported\n",
				__func__, ptype);
			break;
		}
		num_properties_changed--;
	} while (num_properties_changed > 0);

done:
	inst->ops->event_notify(inst, EVT_SYS_EVENT_CHANGE, &event);
}

static void event_release_buffer_ref(struct hfi_core *hfi,
				     struct hfi_inst *inst,
				     struct hfi_msg_event_notify_pkt *pkt)
{
	struct hfi_event_data event = {0};
	struct hfi_msg_event_release_buffer_ref_pkt *data;

	data = (struct hfi_msg_event_release_buffer_ref_pkt *)
		pkt->ext_event_data;

	event.event_type = HFI_EVENT_RELEASE_BUFFER_REFERENCE;
	event.packet_buffer = data->packet_buffer;
	event.extradata_buffer = data->extradata_buffer;

	inst->error = HFI_ERR_NONE;
	inst->ops->event_notify(inst, EVT_SYS_EVENT_CHANGE, &event);
}

static void event_sys_error(struct hfi_core *hfi, u32 event)
{
	hfi->core_ops->event_notify(hfi, event);
}

static void event_session_error(struct hfi_core *hfi, struct hfi_inst *inst,
				struct hfi_msg_event_notify_pkt *pkt)
{
	struct device *dev = hfi->dev;

	dev_dbg(dev, "session error: event id:%x, session id:%x\n",
		pkt->event_data1, pkt->shdr.session_id);

	if (!inst)
		return;

	switch (pkt->event_data1) {
	/* non fatal session errors */
	case HFI_ERR_SESSION_INVALID_SCALE_FACTOR:
	case HFI_ERR_SESSION_UNSUPPORT_BUFFERTYPE:
	case HFI_ERR_SESSION_UNSUPPORTED_SETTING:
	case HFI_ERR_SESSION_UPSCALE_NOT_SUPPORTED:
		inst->error = HFI_ERR_NONE;
		break;
	default:
		dev_err(dev, "session error: event id:%x, session id:%x\n",
			pkt->event_data1, pkt->shdr.session_id);

		inst->error = pkt->event_data1;
		inst->ops->event_notify(inst, EVT_SESSION_ERROR, NULL);
		break;
	}
}

static void hfi_event_notify(struct hfi_core *hfi, struct hfi_inst *inst,
			     void *packet)
{
	struct hfi_msg_event_notify_pkt *pkt = packet;

	if (!packet) {
		dev_err(hfi->dev, "invalid packet\n");
		return;
	}

	switch (pkt->event_id) {
	case HFI_EVENT_SYS_ERROR:
		event_sys_error(hfi, EVT_SYS_ERROR);
		break;
	case HFI_EVENT_SESSION_ERROR:
		event_session_error(hfi, inst, pkt);
		break;
	case HFI_EVENT_SESSION_SEQUENCE_CHANGED:
		event_seq_changed(hfi, inst, pkt);
		break;
	case HFI_EVENT_RELEASE_BUFFER_REFERENCE:
		event_release_buffer_ref(hfi, inst, pkt);
		break;
	case HFI_EVENT_SESSION_PROPERTY_CHANGED:
		break;
	default:
		break;
	}
}

static void hfi_sys_init_done(struct hfi_core *hfi, struct hfi_inst *inst,
			      void *packet)
{
	struct hfi_msg_sys_init_done_pkt *pkt = packet;
	u32 enc_codecs = 0, dec_codecs = 0;
	u32 rem_bytes, read_bytes = 0, num_properties;
	u8 *data_ptr;
	u32 error;
	u32 ptype;

	error = pkt->error_type;
	if (error != HFI_ERR_NONE)
		goto err_no_prop;

	num_properties = pkt->num_properties;

	if (!num_properties) {
		error = HFI_ERR_SYS_INVALID_PARAMETER;
		goto err_no_prop;
	}

	rem_bytes = pkt->hdr.size - sizeof(*pkt) + sizeof(u32);

	if (!rem_bytes) {
		/* missing property data */
		error = HFI_ERR_SYS_INSUFFICIENT_RESOURCES;
		goto err_no_prop;
	}

	data_ptr = (u8 *)&pkt->data[0];

	while (num_properties && rem_bytes >= sizeof(u32)) {
		ptype = *((u32 *)data_ptr);
		data_ptr += sizeof(u32);

		switch (ptype) {
		case HFI_PROPERTY_PARAM_CODEC_SUPPORTED: {
			struct hfi_codec_supported *prop;

			prop = (struct hfi_codec_supported *)data_ptr;

			if (rem_bytes < sizeof(*prop)) {
				error = HFI_ERR_SYS_INSUFFICIENT_RESOURCES;
				break;
			}
			dec_codecs = prop->dec_codecs;
			enc_codecs = prop->enc_codecs;
			break;
		}
		default:
			error = HFI_ERR_SYS_INVALID_PARAMETER;
			break;
		}

		if (!error) {
			rem_bytes -= read_bytes;
			data_ptr += read_bytes;
			num_properties--;
		}
	}

	hfi->enc_codecs = enc_codecs;
	hfi->dec_codecs = dec_codecs;

err_no_prop:
	hfi->error = error;
	complete(&hfi->done);
}

static void
sys_get_prop_image_version(struct device *dev,
			   struct hfi_msg_sys_property_info_pkt *pkt)
{
	int req_bytes;

	req_bytes = pkt->hdr.size - sizeof(*pkt);

	if (req_bytes < 128 || !pkt->data[1] || pkt->num_properties > 1)
		/* bad packet */
		return;

	dev_dbg(dev, "F/W version: %s\n", (u8 *)&pkt->data[1]);
}

static void hfi_sys_property_info(struct hfi_core *hfi, struct hfi_inst *inst,
				  void *packet)
{
	struct hfi_msg_sys_property_info_pkt *pkt = packet;
	struct device *dev = hfi->dev;

	if (!pkt->num_properties) {
		dev_dbg(dev, "%s: no properties\n", __func__);
		return;
	}

	switch (pkt->data[0]) {
	case HFI_PROPERTY_SYS_IMAGE_VERSION:
		sys_get_prop_image_version(dev, pkt);
		break;
	default:
		dev_dbg(dev, "%s: unknown property data\n", __func__);
		break;
	}
}

static void hfi_sys_rel_resource_done(struct hfi_core *hfi,
				      struct hfi_inst *inst,
				      void *packet)
{
	struct hfi_msg_sys_release_resource_done_pkt *pkt = packet;

	hfi->error = pkt->error_type;
	complete(&hfi->done);
}

static void hfi_sys_ping_done(struct hfi_core *hfi, struct hfi_inst *inst,
			      void *packet)
{
	struct hfi_msg_sys_ping_ack_pkt *pkt = packet;

	hfi->error = HFI_ERR_NONE;

	if (pkt->client_data != 0xbeef)
		hfi->error = HFI_ERR_SYS_FATAL;

	complete(&hfi->done);
}

static void hfi_sys_idle_done(struct hfi_core *hfi, struct hfi_inst *inst,
			      void *packet)
{
	dev_dbg(hfi->dev, "sys idle\n");
}

static void hfi_sys_pc_prepare_done(struct hfi_core *hfi, struct hfi_inst *inst,
				    void *packet)
{
	struct hfi_msg_sys_pc_prep_done_pkt *pkt = packet;

	dev_dbg(hfi->dev, "pc prepare done (error %x)\n", pkt->error_type);
}

static void hfi_copy_cap_prop(struct hfi_capability *in, struct hfi_inst *inst)
{
	if (!in || !inst)
		return;

	switch (in->capability_type) {
	case HFI_CAPABILITY_FRAME_WIDTH:
		inst->width = *in;
		break;
	case HFI_CAPABILITY_FRAME_HEIGHT:
		inst->height = *in;
		break;
	case HFI_CAPABILITY_MBS_PER_FRAME:
		inst->mbs_per_frame = *in;
		break;
	case HFI_CAPABILITY_MBS_PER_SECOND:
		inst->mbs_per_sec = *in;
		break;
	case HFI_CAPABILITY_FRAMERATE:
		inst->framerate = *in;
		break;
	case HFI_CAPABILITY_SCALE_X:
		inst->scale_x = *in;
		break;
	case HFI_CAPABILITY_SCALE_Y:
		inst->scale_y = *in;
		break;
	case HFI_CAPABILITY_BITRATE:
		inst->bitrate = *in;
		break;
	case HFI_CAPABILITY_HIER_P_NUM_ENH_LAYERS:
		inst->hier_p = *in;
		break;
	case HFI_CAPABILITY_ENC_LTR_COUNT:
		inst->ltr_count = *in;
		break;
	case HFI_CAPABILITY_CP_OUTPUT2_THRESH:
		inst->secure_output2_threshold = *in;
		break;
	default:
		break;
	}
}

static unsigned int
session_get_prop_profile_level(struct hfi_msg_session_property_info_pkt *pkt,
			       struct hfi_profile_level *profile_level)
{
	struct hfi_profile_level *hfi;
	u32 req_bytes;

	req_bytes = pkt->shdr.hdr.size - sizeof(*pkt);

	if (!req_bytes || req_bytes % sizeof(struct hfi_profile_level))
		/* bad packet */
		return HFI_ERR_SESSION_INVALID_PARAMETER;

	hfi = (struct hfi_profile_level *)&pkt->data[1];
	profile_level->profile = hfi->profile;
	profile_level->level = hfi->level;

	return HFI_ERR_NONE;
}

static unsigned int
session_get_prop_buf_req(struct hfi_msg_session_property_info_pkt *pkt,
			 struct hfi_buffer_requirements *bufreq)
{
	struct hfi_buffer_requirements *buf_req;
	u32 req_bytes;
	unsigned int idx = 0;

	req_bytes = pkt->shdr.hdr.size - sizeof(*pkt);

	if (!req_bytes || req_bytes % sizeof(*buf_req) || !pkt->data[1])
		/* bad packet */
		return HFI_ERR_SESSION_INVALID_PARAMETER;

	buf_req = (struct hfi_buffer_requirements *)&pkt->data[1];
	if (!buf_req)
		return HFI_ERR_SESSION_INVALID_PARAMETER;

	while (req_bytes) {
		memcpy(&bufreq[idx], buf_req, sizeof(*bufreq));
		idx++;

		if (idx > HFI_BUFFER_TYPE_MAX)
			return HFI_ERR_SESSION_INVALID_PARAMETER;

		req_bytes -= sizeof(struct hfi_buffer_requirements);
		buf_req++;
	}

	return HFI_ERR_NONE;
}

static void hfi_session_prop_info(struct hfi_core *hfi, struct hfi_inst *inst,
				  void *packet)
{
	struct hfi_msg_session_property_info_pkt *pkt = packet;
	struct device *dev = hfi->dev;
	union hfi_get_property *hprop = &inst->hprop;
	unsigned int error = HFI_ERR_NONE;

	if (!pkt->num_properties) {
		error = HFI_ERR_SESSION_INVALID_PARAMETER;
		dev_err(dev, "%s: no properties\n", __func__);
		goto done;
	}

	switch (pkt->data[0]) {
	case HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS:
		memset(hprop->bufreq, 0, sizeof(hprop->bufreq));
		error = session_get_prop_buf_req(pkt, hprop->bufreq);
		break;
	case HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT:
		memset(&hprop->profile_level, 0, sizeof(hprop->profile_level));
		error = session_get_prop_profile_level(pkt,
						       &hprop->profile_level);
		break;
	default:
		dev_dbg(dev, "%s: unknown property id:%x\n", __func__,
			pkt->data[0]);
		return;
	}

done:
	inst->error = error;
	complete(&inst->done);
}

static u32
session_init_done_read_prop(struct hfi_core *hfi, struct hfi_inst *inst,
			    struct hfi_msg_session_init_done_pkt *pkt)
{
	struct device *dev = hfi->dev;
	u32 rem_bytes, num_props;
	u32 ptype, next_offset = 0;
	u32 err;
	u8 *data;

	rem_bytes = pkt->shdr.hdr.size - sizeof(*pkt) + sizeof(u32);
	if (!rem_bytes) {
		dev_err(dev, "%s: missing property info\n", __func__);
		return HFI_ERR_SESSION_INSUFFICIENT_RESOURCES;
	}

	err = pkt->error_type;
	if (err)
		return err;

	data = (u8 *) &pkt->data[0];
	num_props = pkt->num_properties;

	while (err == HFI_ERR_NONE && num_props && rem_bytes >= sizeof(u32)) {
		ptype = *((u32 *)data);
		next_offset = sizeof(u32);

		switch (ptype) {
		case HFI_PROPERTY_PARAM_CAPABILITY_SUPPORTED: {
			struct hfi_capabilities *caps;
			struct hfi_capability *cap;
			u32 num_caps;

			if ((rem_bytes - next_offset) < sizeof(*cap)) {
				err = HFI_ERR_SESSION_INVALID_PARAMETER;
				break;
			}

			caps = (struct hfi_capabilities *)(data + next_offset);

			num_caps = caps->num_capabilities;
			cap = &caps->data[0];
			next_offset += sizeof(u32);

			while (num_caps &&
			      (rem_bytes - next_offset) >= sizeof(u32)) {
				hfi_copy_cap_prop(cap, inst);
				cap++;
				next_offset += sizeof(*cap);
				num_caps--;
			}
			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SUPPORTED: {
			struct hfi_uncompressed_format_supported *prop =
				(struct hfi_uncompressed_format_supported *)
				(data + next_offset);
			u32 num_fmt_entries;
			u8 *fmt;
			struct hfi_uncompressed_plane_info *inf;

			if ((rem_bytes - next_offset) < sizeof(*prop)) {
				err = HFI_ERR_SESSION_INVALID_PARAMETER;
				break;
			}

			num_fmt_entries = prop->format_entries;
			next_offset = sizeof(*prop) - sizeof(u32);
			fmt = (u8 *)&prop->format_info[0];

			dev_dbg(dev, "uncomm format support num entries:%u\n",
				num_fmt_entries);

			while (num_fmt_entries) {
				struct hfi_uncompressed_plane_constraints *cnts;
				u32 bytes_to_skip;

				inf = (struct hfi_uncompressed_plane_info *)fmt;

				if ((rem_bytes - next_offset) < sizeof(*inf)) {
					err = HFI_ERR_SESSION_INVALID_PARAMETER;
					break;
				}

				dev_dbg(dev, "plane info: fmt:%x, planes:%x\n",
					inf->format, inf->num_planes);

				cnts = &inf->plane_format[0];
				dev_dbg(dev, "%u %u %u %u\n",
					cnts->stride_multiples,
					cnts->max_stride,
					cnts->min_plane_buffer_height_multiple,
					cnts->buffer_alignment);

				bytes_to_skip = sizeof(*inf) - sizeof(*cnts) +
						inf->num_planes * sizeof(*cnts);

				fmt += bytes_to_skip;
				next_offset += bytes_to_skip;
				num_fmt_entries--;
			}
			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_PROPERTIES_SUPPORTED: {
			struct hfi_properties_supported *prop =
				(struct hfi_properties_supported *)
				(data + next_offset);

			next_offset += sizeof(*prop) - sizeof(u32)
					+ prop->num_properties * sizeof(u32);
			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_PROFILE_LEVEL_SUPPORTED: {
			struct hfi_profile_level_supported *prop =
				(struct hfi_profile_level_supported *)
				(data + next_offset);
			struct hfi_profile_level *pl;
			unsigned int prop_count = 0;
			unsigned int count = 0;
			u8 *ptr;

			ptr = (u8 *)&prop->profile_level[0];
			prop_count = prop->profile_count;

			if (prop_count > HFI_MAX_PROFILE_COUNT)
				prop_count = HFI_MAX_PROFILE_COUNT;

			while (prop_count) {
				ptr++;
				pl = (struct hfi_profile_level *)ptr;

				inst->pl[count].profile = pl->profile;
				inst->pl[count].level = pl->level;
				prop_count--;
				count++;
				ptr += sizeof(*pl) / sizeof(u32);
			}

			inst->pl_count = count;
			next_offset += sizeof(*prop) - sizeof(*pl) +
				       prop->profile_count * sizeof(*pl);

			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_NAL_STREAM_FORMAT_SUPPORTED: {
			struct hfi_nal_stream_format *nal =
				(struct hfi_nal_stream_format *)
				(data + next_offset);
			dev_dbg(dev, "NAL format: %x\n", nal->format);
			next_offset += sizeof(*nal);
			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_NAL_STREAM_FORMAT_SELECT: {
			next_offset += sizeof(u32);
			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_MAX_SEQUENCE_HEADER_SIZE: {
			u32 *max_seq_sz = (u32 *)(data + next_offset);

			dev_dbg(dev, "max seq header sz: %x\n", *max_seq_sz);
			next_offset += sizeof(u32);
			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_VENC_INTRA_REFRESH: {
			next_offset += sizeof(struct hfi_intra_refresh);
			num_props--;
			break;
		}
		case HFI_PROPERTY_PARAM_BUFFER_ALLOC_MODE_SUPPORTED: {
			struct hfi_buffer_alloc_mode_supported *prop =
				(struct hfi_buffer_alloc_mode_supported *)
				(data + next_offset);
			int i;

			if (prop->buffer_type == HFI_BUFFER_OUTPUT ||
			    prop->buffer_type == HFI_BUFFER_OUTPUT2) {
				for (i = 0; i < prop->num_entries; i++) {
					switch (prop->data[i]) {
					case HFI_BUFFER_MODE_STATIC:
						inst->alloc_mode_static = true;
						break;
					case HFI_BUFFER_MODE_DYNAMIC:
						inst->alloc_mode_dynamic = true;
						break;
					}
				}
			}
			next_offset += sizeof(*prop) -
				sizeof(u32) + prop->num_entries * sizeof(u32);
			num_props--;
			break;
		}
		default:
			dev_dbg(dev, "%s: default case %#x\n", __func__, ptype);
			break;
		}

		rem_bytes -= next_offset;
		data += next_offset;
	}

	return err;
}

static void hfi_session_init_done(struct hfi_core *hfi, struct hfi_inst *inst,
				  void *packet)
{
	struct hfi_msg_session_init_done_pkt *pkt = packet;
	unsigned int error;

	error = pkt->error_type;
	if (error != HFI_ERR_NONE)
		goto done;

	error = session_init_done_read_prop(hfi, inst, pkt);

done:
	inst->error = error;
	complete(&inst->done);
}

static void hfi_session_load_res_done(struct hfi_core *hfi,
				      struct hfi_inst *inst, void *packet)
{
	struct hfi_msg_session_load_resources_done_pkt *pkt = packet;

	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_flush_done(struct hfi_core *hfi, struct hfi_inst *inst,
				   void *packet)
{
	struct hfi_msg_session_flush_done_pkt *pkt = packet;

	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_etb_done(struct hfi_core *hfi, struct hfi_inst *inst,
				 void *packet)
{
	struct hfi_msg_session_empty_buffer_done_pkt *pkt = packet;
	u32 flags = 0;

	inst->error = pkt->error_type;

	inst->ops->empty_buf_done(inst, pkt->input_tag, pkt->filled_len,
				  pkt->offset, flags);
}

static void hfi_session_ftb_done(struct hfi_core *hfi, struct hfi_inst *inst,
				 void *packet)
{
	u32 session_type = inst->session_type;
	struct hfi_msg_fbd fbd = {0};
	struct timeval timestamp;
	int64_t time_usec = 0;
	unsigned int error;
	u32 flags = 0;

	if (session_type == VIDC_SESSION_TYPE_ENC) {
		struct hfi_msg_session_fbd_compressed_pkt *pkt = packet;

		fbd.timestamp_hi = pkt->time_stamp_hi;
		fbd.timestamp_lo = pkt->time_stamp_lo;
		fbd.flags1 = pkt->flags;
		fbd.offset1 = pkt->offset;
		fbd.alloc_len1 = pkt->alloc_len;
		fbd.filled_len1 = pkt->filled_len;
		fbd.picture_type = pkt->picture_type;
		fbd.packet_buffer1 = pkt->packet_buffer;
		fbd.extradata_buffer = pkt->extradata_buffer;
		fbd.buffer_type = HFI_BUFFER_OUTPUT;

		error = pkt->error_type;
	} else if (session_type == VIDC_SESSION_TYPE_DEC) {
		struct hfi_msg_session_fbd_uncompressed_plane0_pkt *pkt =
			packet;

		fbd.timestamp_hi = pkt->time_stamp_hi;
		fbd.timestamp_lo = pkt->time_stamp_lo;
		fbd.flags1 = pkt->flags;
		fbd.offset1 = pkt->offset;
		fbd.alloc_len1 = pkt->alloc_len;
		fbd.filled_len1 = pkt->filled_len;
		fbd.picture_type = pkt->picture_type;
		fbd.packet_buffer1 = pkt->packet_buffer;
		fbd.extradata_buffer = pkt->extradata_buffer;

		if (pkt->stream_id == 0)
			fbd.buffer_type = HFI_BUFFER_OUTPUT;
		else if (pkt->stream_id == 1)
			fbd.buffer_type = HFI_BUFFER_OUTPUT2;

		error = pkt->error_type;
	} else {
		error = HFI_ERR_SESSION_INVALID_PARAMETER;
	}

	if (fbd.buffer_type != HFI_BUFFER_OUTPUT)
		return;

	if (fbd.flags1 & HFI_BUFFERFLAG_EOS)
		flags |= V4L2_BUF_FLAG_LAST;

	switch (fbd.picture_type) {
	case HFI_PICTURE_IDR:
		flags |= V4L2_BUF_FLAG_KEYFRAME;
		break;
	case HFI_PICTURE_I:
		flags |= V4L2_BUF_FLAG_KEYFRAME;
		break;
	case HFI_PICTURE_P:
		flags |= V4L2_BUF_FLAG_PFRAME;
		break;
	case HFI_PICTURE_B:
		flags |= V4L2_BUF_FLAG_BFRAME;
		break;
	case HFI_FRAME_NOTCODED:
	case HFI_UNUSED_PICT:
	case HFI_FRAME_YUV:
	default:
		break;
	}

	if (!(fbd.flags1 & HFI_BUFFERFLAG_TIMESTAMPINVALID) &&
	      fbd.filled_len1) {
		time_usec = fbd.timestamp_hi;
		time_usec = (time_usec << 32) | fbd.timestamp_lo;
	}

	timestamp = ns_to_timeval(time_usec * NSEC_PER_USEC);

	inst->error = error;
	inst->ops->fill_buf_done(inst, fbd.packet_buffer1, fbd.filled_len1,
				 fbd.offset1, flags, &timestamp);
}

static void hfi_session_start_done(struct hfi_core *hfi, struct hfi_inst *inst,
				   void *packet)
{
	struct hfi_msg_session_start_done_pkt *pkt = packet;

	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_stop_done(struct hfi_core *hfi, struct hfi_inst *inst,
				  void *packet)
{
	struct hfi_msg_session_stop_done_pkt *pkt = packet;

	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_rel_res_done(struct hfi_core *hfi,
				     struct hfi_inst *inst, void *packet)
{
	struct hfi_msg_session_release_resources_done_pkt *pkt = packet;

	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_rel_buf_done(struct hfi_core *hfi,
				     struct hfi_inst *inst, void *packet)
{
	struct hfi_msg_session_release_buffers_done_pkt *pkt = packet;

	/*
	 * the address of the released buffer can be extracted:
	 * if (pkt->buffer_info) {
	 *	cmd.data = &pkt->buffer_info;
	 *	cmd.size = sizeof(struct hfi_buffer_info);
	 * }
	 */
	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_end_done(struct hfi_core *hfi, struct hfi_inst *inst,
				 void *packet)
{
	struct hfi_msg_session_end_done_pkt *pkt = packet;

	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_abort_done(struct hfi_core *hfi, struct hfi_inst *inst,
				   void *packet)
{
	struct hfi_msg_sys_session_abort_done_pkt *pkt = packet;

	inst->error = pkt->error_type;
	complete(&inst->done);
}

static void hfi_session_get_seq_hdr_done(struct hfi_core *hfi,
					 struct hfi_inst *inst, void *packet)
{
	struct hfi_msg_session_get_sequence_hdr_done_pkt *pkt = packet;

	/*
	 * output_done.packet_buffer1 = pkt->sequence_header;
	 * output_done.filled_len1 = pkt->header_len;
	 */

	inst->error = pkt->error_type;
	complete(&inst->done);
}

struct hfi_done_handler {
	u32 pkt;
	u32 pkt_sz;
	u32 pkt_sz2;
	void (*done)(struct hfi_core *, struct hfi_inst *, void *);
	bool is_sys_pkt;
};

static const struct hfi_done_handler handlers[] = {
	{.pkt = HFI_MSG_EVENT_NOTIFY,
	 .pkt_sz = sizeof(struct hfi_msg_event_notify_pkt),
	 .done = hfi_event_notify,
	},
	{.pkt = HFI_MSG_SYS_INIT,
	 .pkt_sz = sizeof(struct hfi_msg_sys_init_done_pkt),
	 .done = hfi_sys_init_done,
	 .is_sys_pkt = true,
	},
	{.pkt = HFI_MSG_SYS_PROPERTY_INFO,
	 .pkt_sz = sizeof(struct hfi_msg_sys_property_info_pkt),
	 .done = hfi_sys_property_info,
	 .is_sys_pkt = true,
	},
	{.pkt = HFI_MSG_SYS_RELEASE_RESOURCE,
	 .pkt_sz = sizeof(struct hfi_msg_sys_release_resource_done_pkt),
	 .done = hfi_sys_rel_resource_done,
	 .is_sys_pkt = true,
	},
	{.pkt = HFI_MSG_SYS_PING_ACK,
	 .pkt_sz = sizeof(struct hfi_msg_sys_ping_ack_pkt),
	 .done = hfi_sys_ping_done,
	 .is_sys_pkt = true,
	},
	{.pkt = HFI_MSG_SYS_IDLE,
	 .pkt_sz = sizeof(struct hfi_msg_sys_idle_pkt),
	 .done = hfi_sys_idle_done,
	 .is_sys_pkt = true,
	},
	{.pkt = HFI_MSG_SYS_PC_PREP,
	 .pkt_sz = sizeof(struct hfi_msg_sys_pc_prep_done_pkt),
	 .done = hfi_sys_pc_prepare_done,
	 .is_sys_pkt = true,
	},
	{.pkt = HFI_MSG_SYS_SESSION_INIT,
	 .pkt_sz = sizeof(struct hfi_msg_session_init_done_pkt),
	 .done = hfi_session_init_done,
	},
	{.pkt = HFI_MSG_SYS_SESSION_END,
	 .pkt_sz = sizeof(struct hfi_msg_session_end_done_pkt),
	 .done = hfi_session_end_done,
	},
	{.pkt = HFI_MSG_SESSION_LOAD_RESOURCES,
	 .pkt_sz = sizeof(struct hfi_msg_session_load_resources_done_pkt),
	 .done = hfi_session_load_res_done,
	},
	{.pkt = HFI_MSG_SESSION_START,
	 .pkt_sz = sizeof(struct hfi_msg_session_start_done_pkt),
	 .done = hfi_session_start_done,
	},
	{.pkt = HFI_MSG_SESSION_STOP,
	 .pkt_sz = sizeof(struct hfi_msg_session_stop_done_pkt),
	 .done = hfi_session_stop_done,
	},
	{.pkt = HFI_MSG_SYS_SESSION_ABORT,
	 .pkt_sz = sizeof(struct hfi_msg_sys_session_abort_done_pkt),
	 .done = hfi_session_abort_done,
	},
	{.pkt = HFI_MSG_SESSION_EMPTY_BUFFER,
	 .pkt_sz = sizeof(struct hfi_msg_session_empty_buffer_done_pkt),
	 .done = hfi_session_etb_done,
	},
	{.pkt = HFI_MSG_SESSION_FILL_BUFFER,
	 .pkt_sz = sizeof(struct hfi_msg_session_fbd_uncompressed_plane0_pkt),
	 .pkt_sz2 = sizeof(struct hfi_msg_session_fbd_compressed_pkt),
	 .done = hfi_session_ftb_done,
	},
	{.pkt = HFI_MSG_SESSION_FLUSH,
	 .pkt_sz = sizeof(struct hfi_msg_session_flush_done_pkt),
	 .done = hfi_session_flush_done,
	},
	{.pkt = HFI_MSG_SESSION_PROPERTY_INFO,
	 .pkt_sz = sizeof(struct hfi_msg_session_property_info_pkt),
	 .done = hfi_session_prop_info,
	},
	{.pkt = HFI_MSG_SESSION_RELEASE_RESOURCES,
	 .pkt_sz = sizeof(struct hfi_msg_session_release_resources_done_pkt),
	 .done = hfi_session_rel_res_done,
	},
	{.pkt = HFI_MSG_SESSION_GET_SEQUENCE_HEADER,
	 .pkt_sz = sizeof(struct hfi_msg_session_get_sequence_hdr_done_pkt),
	 .done = hfi_session_get_seq_hdr_done,
	},
	{.pkt = HFI_MSG_SESSION_RELEASE_BUFFERS,
	 .pkt_sz = sizeof(struct hfi_msg_session_release_buffers_done_pkt),
	 .done = hfi_session_rel_buf_done,
	},
};

void hfi_process_watchdog_timeout(struct hfi_core *hfi)
{
	event_sys_error(hfi, EVT_SYS_WATCHDOG_TIMEOUT);
}

u32 hfi_process_msg_packet(struct hfi_core *hfi, struct hfi_pkt_hdr *hdr)
{
	const struct hfi_done_handler *handler;
	struct device *dev = hfi->dev;
	struct hfi_inst *inst;
	bool found = false;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(handlers); i++) {
		handler = &handlers[i];

		if (handler->pkt != hdr->pkt_type)
			continue;

		found = true;
		break;
	}

	if (found == false)
		return hdr->pkt_type;

	if (hdr->size && hdr->size < handler->pkt_sz &&
	    hdr->size < handler->pkt_sz2) {
		dev_err(dev, "bad packet size (%d should be %d, pkt type:%x)\n",
			hdr->size, handler->pkt_sz, hdr->pkt_type);

		return hdr->pkt_type;
	}

	if (handler->is_sys_pkt) {
		inst = NULL;
	} else {
		struct hfi_session_pkt *pkt;

		pkt = (struct hfi_session_pkt *)hdr;
		inst = to_hfi_instance(hfi, pkt->shdr.session_id);

		if (!inst)
			dev_warn(dev, "no valid instance(pkt session_id:%x)\n",
				 pkt->shdr.session_id);

		/*
		 * Event of type HFI_EVENT_SYS_ERROR will not have any session
		 * associated with it
		 */
		if (!inst && hdr->pkt_type != HFI_MSG_EVENT_NOTIFY) {
			dev_err(dev, "got invalid session id:%d\n",
				pkt->shdr.session_id);
			goto invalid_session;
		}
	}

	handler->done(hfi, inst, hdr);

invalid_session:
	return hdr->pkt_type;
}
