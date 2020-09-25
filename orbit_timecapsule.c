/*
 * Copyright (c) 2015-2016 Hanspeter Portner (dev@open-music-kontrollers.ch)
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the Artistic License 2.0 as published by
 * The Perl Foundation.
 *
 * This source is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * Artistic License 2.0 for more details.
 *
 * You should have received a copy of the Artistic License 2.0
 * along the source as a COPYING file. If not, obtain it from
 * http://www.perlfoundation.org/artistic_license_2_0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <zlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

#include <orbit.h>
#include <timely.h>
#include <props.h>
#include <varchunk.h>

#define NETATOM_IMPLEMENTATION
#include <netatom.lv2/netatom.h>

#define MAX_NPROPS 5
#define MAGIC_SIZE 8
#define MAX_BUF 8192

typedef struct _item_t item_t;
typedef enum _job_type_t job_type_t;
typedef struct _job_t job_t;
typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

struct _item_t {
	union {
		uint64_t u;
		double d;
	} beats;
	uint32_t size;
} __attribute__((packed));

enum _job_type_t {
	TC_JOB_DRAIN,
	TC_JOB_REPOSITION_PLAY,
	TC_JOB_READ,
	TC_JOB_REPOSITION_REC,
	TC_JOB_WRITE,
	TC_JOB_CHANGE_PATH
};

struct _job_t {
	job_type_t type;
	double beats;
	union {
		LV2_Atom atom [0];
		char file_path [0];
	};
};

struct _plugstate_t {
	int32_t mute;
	int32_t record;
	int32_t mute_toggle;
	int32_t record_toggle;
	char file_path [PATH_MAX];
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_URID_Unmap *unmap;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	LV2_Log_Log *log;
	LV2_Log_Logger logger;

	struct {
		LV2_URID mute;
		LV2_URID record;
		LV2_URID mute_toggle;
		LV2_URID record_toggle;
	} urid;
	
	timely_t timely;

	plugstate_t state;
	plugstate_t stash;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	int64_t offset;

	PROPS_T(props, MAX_NPROPS);

	bool rolling;

	LV2_Worker_Schedule *sched;

	netatom_t *netatom;
	varchunk_t *to_dsp;
	varchunk_t *to_worker;

	uint8_t buf [MAX_BUF];

	char path [PATH_MAX];
	gzFile gzfile;
	bool draining;
	char file_path [PATH_MAX];
};

static const char magic [MAGIC_SIZE] = "netatom"; //FIXME use

static const char *reading_mode = "rb";
static const char *writing_mode = "wb9";

static inline void
_wakeup(plughandle_t *handle)
{
	const int32_t dummy = 0;

	const LV2_Worker_Status status = handle->sched->schedule_work(
		handle->sched->handle, sizeof(int32_t), &dummy);
	if( (status != LV2_WORKER_SUCCESS) && handle->log)
	{
		lv2_log_trace(&handle->logger, "%s: work:schedule failed\n", __func__);
	}
}

static inline void
_request_read(plughandle_t *handle)
{
	const size_t tot_size = sizeof(job_t);

	job_t *job;
	if((job = varchunk_write_request(handle->to_worker, tot_size)))
	{
		job->type = TC_JOB_READ;

		varchunk_write_advance(handle->to_worker, tot_size);
		_wakeup(handle);
	}
	else if(handle->log)
	{
		lv2_log_trace(&handle->logger, "%s: ringbuffer overflow\n", __func__);
	}
}

static inline void
_reposition_play(plughandle_t *handle, double beats)
{
	const size_t tot_size = sizeof(job_t);

	job_t *job;
	if((job = varchunk_write_request(handle->to_worker, tot_size)))
	{
		job->type = TC_JOB_REPOSITION_PLAY;
		job->beats = beats;

		varchunk_write_advance(handle->to_worker, tot_size);
		_wakeup(handle);
		handle->draining = true;
	}
	else if(handle->log)
	{
		lv2_log_trace(&handle->logger, "%s: ringbuffer overflow\n", __func__);
	}
}

static inline void
_reposition_rec(plughandle_t *handle, double beats)
{
	const size_t tot_size = sizeof(job_t);

	job_t *job;
	if((job = varchunk_write_request(handle->to_worker, tot_size)))
	{
		job->type = TC_JOB_REPOSITION_REC;
		job->beats = beats;

		varchunk_write_advance(handle->to_worker, tot_size);
		_wakeup(handle);
		handle->draining = true;
	}
	else if(handle->log)
	{
		lv2_log_trace(&handle->logger, "%s: ringbuffer overflow\n", __func__);
	}
}

static void
_mute_intercept(void *data, int64_t frames, props_impl_t *impl)
{
	plughandle_t *handle = data;

	if(handle->state.mute_toggle)
	{
		handle->state.mute_toggle = false;
		handle->state.mute = !handle->state.mute;

		props_set(&handle->props, &handle->forge, frames, handle->urid.mute_toggle, &handle->ref);
		props_set(&handle->props, &handle->forge, frames, handle->urid.mute, &handle->ref);
	}
}

static void
_record_intercept(void *data, int64_t frames, props_impl_t *impl)
{
	plughandle_t *handle = data;

	if(handle->state.record_toggle)
	{
		handle->state.record_toggle = false;
		handle->state.record = !handle->state.record;

		props_set(&handle->props, &handle->forge, frames, handle->urid.record_toggle, &handle->ref);
		props_set(&handle->props, &handle->forge, frames, handle->urid.record, &handle->ref);
	}

	const double beats = handle->offset / TIMELY_FRAMES_PER_BEAT(&handle->timely);
	if(!isfinite(beats))
		return;

	if(handle->state.record)
		_reposition_rec(handle, beats);
	else
		_reposition_play(handle, beats);
}

static void
_path_intercept(void *data, int64_t frames, props_impl_t *impl)
{
	plughandle_t *handle = data;

	const size_t len = strlen(handle->state.file_path) + 1;
	const size_t tot_size = sizeof(job_t) + len;
	const double beats = handle->offset / TIMELY_FRAMES_PER_BEAT(&handle->timely);
	if(!isfinite(beats))
		return;

	job_t *job;
	if((job = varchunk_write_request(handle->to_worker, tot_size)))
	{
		job->beats = beats;
		job->type = TC_JOB_CHANGE_PATH;
		snprintf(job->file_path, len, "%s", handle->state.file_path);

		varchunk_write_advance(handle->to_worker, tot_size);
		_wakeup(handle);
	}
	else if(handle->log)
	{
		lv2_log_trace(&handle->logger, "%s: ringbuffer overflow\n", __func__);
	}
}

static const props_def_t defs [MAX_NPROPS] = {
	{
		.property = ORBIT_URI"#timecapsule_mute",
		.offset = offsetof(plugstate_t, mute),
		.type = LV2_ATOM__Bool,
		.event_cb = _mute_intercept
	},
	{
		.property = ORBIT_URI"#timecapsule_record",
		.offset = offsetof(plugstate_t, record),
		.type = LV2_ATOM__Bool,
		.event_cb = _record_intercept
	},
	{
		.property = ORBIT_URI"#timecapsule_mute_toggle",
		.offset = offsetof(plugstate_t, mute_toggle),
		.type = LV2_ATOM__Bool,
		.event_cb = _mute_intercept
	},
	{
		.property = ORBIT_URI"#timecapsule_record_toggle",
		.offset = offsetof(plugstate_t, record_toggle),
		.type = LV2_ATOM__Bool,
		.event_cb = _record_intercept
	},
	{
		.property = ORBIT_URI"#timecapsule_file_path",
		.offset = offsetof(plugstate_t, file_path),
		.type = LV2_ATOM__Path,
		.event_cb = _path_intercept,
		.max_size = PATH_MAX
	}
};

static inline void
_play(plughandle_t *handle, int64_t to)
{
	bool consumed = false;

	const int64_t rel = handle->offset - to; // beginning of current period

	const job_t *job;
	size_t tot_size;
	while((job = varchunk_read_request(handle->to_dsp, &tot_size)))
	{
		switch(job->type)
		{
			case TC_JOB_WRITE:
			{
				if(handle->draining)
					break; // ignore while draining

				const int64_t beat_frames = job->beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

				if(beat_frames >= handle->offset)
					goto skip; // event not part of this period

				int64_t frames = beat_frames - rel;
				if(frames < 0)
					frames = 0; //FIXME

				//printf("%li %u\n", frames, job->atom->size);

				if(handle->ref)
					handle->ref = lv2_atom_forge_frame_time(&handle->forge, frames);
				if(handle->ref)
					handle->ref = lv2_atom_forge_write(&handle->forge, job->atom, lv2_atom_total_size(job->atom));
			} break;

			case TC_JOB_DRAIN:
			{
				if(handle->draining)
					handle->draining = false;
			} break;

			case TC_JOB_CHANGE_PATH:
			case TC_JOB_REPOSITION_PLAY:
			case TC_JOB_READ:
			case TC_JOB_REPOSITION_REC:
			{
				// nothing to do
			} break;
		}

		varchunk_read_advance(handle->to_dsp);
		consumed = true;
		continue;

skip:
		break;
	}

	if(consumed)
		_request_read(handle);
}

static inline void
_rec(plughandle_t *handle, const LV2_Atom_Event *ev)
{
	const LV2_Atom *atom = &ev->body;
	const size_t atom_size = lv2_atom_total_size(atom);
	const size_t tot_size = sizeof(job_t) + atom_size;

	job_t *job;
	if((job = varchunk_write_request(handle->to_worker, tot_size)))
	{
		job->type = TC_JOB_WRITE;
		job->beats = handle->offset / TIMELY_FRAMES_PER_BEAT(&handle->timely);
		memcpy(job->atom, atom, atom_size);

		varchunk_write_advance(handle->to_worker, tot_size);
		_wakeup(handle);
	}
	else if(handle->log)
	{
		lv2_log_trace(&handle->logger, "%s: ringbuffer overflow\n", __func__);
	}
}

static void
_cb(timely_t *timely, int64_t frames, LV2_URID type, void *data)
{
	plughandle_t *handle = data;

	if(type == TIMELY_URI_SPEED(timely))
	{
		handle->rolling = TIMELY_SPEED(timely) > 0.f ? true : false;
	}
	else if(type == TIMELY_URI_BAR_BEAT(timely))
	{
		const double beats = (double)TIMELY_BAR(timely) * TIMELY_BEATS_PER_BAR(timely)
			+ TIMELY_BAR_BEAT(timely);
		if(!isfinite(beats))
			return;

		handle->offset = beats * TIMELY_FRAMES_PER_BEAT(timely);

		if(handle->state.record)
			_reposition_rec(handle, beats);
		else
			_reposition_play(handle, beats);
	}
}

static inline void
_close_disk(plughandle_t *handle)
{
	if(handle->gzfile)
	{
		gzclose(handle->gzfile);
		handle->gzfile = NULL;
	}
}

static inline int
_read_header(plughandle_t *handle, double *beats, uint32_t *size)
{
	item_t itm;

	if(gzfread(&itm, sizeof(item_t), 1, handle->gzfile) != 1)
	{
		int errnum;
		const char *err = gzerror(handle->gzfile, &errnum);
		if( (errnum != Z_OK) && handle->log)
		{
			lv2_log_error(&handle->logger, "%s: gzfread failed: %s\n", __func__, err);
		}
		return -1;
	}

	itm.beats.u = be64toh(itm.beats.u);
	itm.size = be32toh(itm.size);

	if(beats)
		*beats = itm.beats.d;
	if(size)
		*size = itm.size;

	return 0;
}

static inline void
_reopen_disk(plughandle_t *handle, bool writing, double beats)
{
	_close_disk(handle);

	z_off_t offset = 0;

	if(beats > 0.0)
	{
		handle->gzfile = gzopen(handle->file_path, reading_mode);
		if(!handle->gzfile)
		{
			if(handle->log)
			{
				lv2_log_error(&handle->logger, "%s: gzopen failed: %s '%s'\n",
					__func__, handle->file_path, strerror(errno));
			}
			goto stage_2;
		}

		double _beats;
		uint32_t _size;
		while(_read_header(handle, &_beats, &_size) == 0)
		{
			if(_beats >= beats) // found point of interest
			{
				break;
			}
			else
			{
				const int res = gzseek(handle->gzfile, _size, SEEK_CUR); // skip item payload
				if( (res == -1) && handle->log)
				{
					lv2_log_error(&handle->logger, "%s: gzseek failed: %s '%s'\n",
						__func__, handle->file_path, strerror(errno));
					break;
				}
			}

			offset = gztell(handle->gzfile);
		}

		_close_disk(handle);
	}

stage_2:
	handle->gzfile = gzopen(handle->file_path, writing ? writing_mode : reading_mode);
	if(!handle->gzfile)
	{
		if(handle->log)
		{
			lv2_log_error(&handle->logger, "%s: gzopen failed: %s '%s'\n",
				__func__, handle->file_path, strerror(errno));
		}
		return;
	}

	if(offset > 0)
	{
		const int res = gzseek(handle->gzfile, offset, SEEK_SET);
		if( (res == -1) && handle->log)
		{
			lv2_log_error(&handle->logger, "%s: gzseek failed: %s '%s'\n",
				__func__, handle->file_path, strerror(errno));
		}
	}
}

static inline int
_write_to(plughandle_t *handle, double beats, const LV2_Atom *atom)
{
	//printf("_write\n");
	if(!handle->gzfile)
		return -1;

	memcpy(handle->buf, atom, lv2_atom_total_size(atom));

	size_t rx_size;
	const uint8_t *rx_body = netatom_serialize(handle->netatom, (LV2_Atom *)handle->buf, MAX_BUF, &rx_size);
	if(rx_body)
	{
		item_t itm = {
			.beats.d = beats,
			.size = rx_size
		};
		itm.beats.u = htobe64(itm.beats.u);
		itm.size = htobe32(itm.size);

		const int written = gzfwrite(&itm, sizeof(item_t), 1, handle->gzfile)
			+ gzfwrite(rx_body, rx_size, 1, handle->gzfile);

		if( (written != 2) && handle->log)
		{
			int errnum;
			const char *err = gzerror(handle->gzfile, &errnum);
			lv2_log_error(&handle->logger, "%s: gsfwrite failed: %s\n", __func__, err);
		}

		return 0;
	}
	else if(handle->log)
	{
		lv2_log_error(&handle->logger, "%s: netatom_serialize failed\n", __func__);
	}

	return -1;
}

static inline int
_read_from(plughandle_t *handle)
{
	//printf("_read\n");
	if(!handle->gzfile)
		return -1;

	double beats;
	uint32_t tx_size;
	if(_read_header(handle, &beats, &tx_size) != 0)
		return -1;

	job_t *job;
	const uint32_t tot_size = sizeof(job_t) + tx_size;
	if((job = varchunk_write_request(handle->to_dsp, tot_size)))
	{
		job->type = TC_JOB_WRITE;
		job->beats = beats;

		if(gzfread(job->atom, tx_size, 1, handle->gzfile) != 1)
		{
			int errnum;
			const char *err = gzerror(handle->gzfile, &errnum);
			if( (errnum != Z_OK) && handle->log)
			{
				lv2_log_error(&handle->logger, "%s: gzfread failed: %s\n", __func__, err);
			}
			return -1;
		}

		const LV2_Atom *atom = netatom_deserialize(handle->netatom, (uint8_t *)job->atom, tx_size);
		if(atom)
		{
			const uint32_t atom_size = lv2_atom_total_size(atom);
			memcpy(job->atom, atom, atom_size);

			varchunk_write_advance(handle->to_dsp, sizeof(job_t) + atom_size);
			return 0;
		}
		else if(handle->log)
		{
			lv2_log_error(&handle->logger, "%s: netatom_deserialize failed\n", __func__);
		}
	}
	else if(handle->log)
	{
		lv2_log_error(&handle->logger, "%s: ringbuffer overflow\n", __func__);
	}

	return -1;
}

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate,
	const char *bundle_path, const LV2_Feature *const *features)
{
	plughandle_t *handle = calloc(1, sizeof(plughandle_t));
	if(!handle)
		return NULL;
	mlock(handle, sizeof(plughandle_t));

	for(unsigned i=0; features[i]; i++)
	{
		if(!strcmp(features[i]->URI, LV2_URID__map))
			handle->map = features[i]->data;
		else if(!strcmp(features[i]->URI, LV2_URID__unmap))
			handle->unmap = features[i]->data;
		else if(!strcmp(features[i]->URI, LV2_WORKER__schedule))
			handle->sched = features[i]->data;
		else if(!strcmp(features[i]->URI, LV2_LOG__log))
			handle->log= features[i]->data;
	}

	if(!handle->map)
	{
		fprintf(stderr,
			"%s: Host does not support urid:map\n", descriptor->URI);
		free(handle);
		return NULL;
	}

	if(!handle->unmap)
	{
		fprintf(stderr,
			"%s: Host does not support urid:unmap\n", descriptor->URI);
		free(handle);
		return NULL;
	}

	if(!handle->sched)
	{
		fprintf(stderr,
			"%s: Host does not support worker:schedule\n", descriptor->URI);
		free(handle);
		return NULL;
	}

	if(handle->log)
		lv2_log_logger_init(&handle->logger, handle->map, handle->log);

	handle->netatom = netatom_new(handle->map, handle->unmap, true);
	if(!handle->netatom)
	{
		free(handle);
		return NULL;
	}

	handle->to_worker = varchunk_new(0x100000, true); // 1M
	if(!handle->to_worker)
	{
		fprintf(stderr,
			"%s: Failed to initialize ringbuffer\n", descriptor->URI);
		netatom_free(handle->netatom);
		free(handle);
		return NULL;
	}

	handle->to_dsp = varchunk_new(0x100000, true); // 1M
	if(!handle->to_dsp)
	{
		fprintf(stderr,
			"%s: Failed to initialize ringbuffer\n", descriptor->URI);
		varchunk_free(handle->to_worker);
		netatom_free(handle->netatom);
		free(handle);
		return NULL;
	}

	lv2_atom_forge_init(&handle->forge, handle->map);

	timely_mask_t mask = TIMELY_MASK_BAR_BEAT
		//| TIMELY_MASK_BAR
		| TIMELY_MASK_BEAT_UNIT
		| TIMELY_MASK_BEATS_PER_BAR
		| TIMELY_MASK_BEATS_PER_MINUTE
		| TIMELY_MASK_FRAMES_PER_SECOND
		| TIMELY_MASK_SPEED;
	timely_init(&handle->timely, handle->map, rate, mask, _cb, handle);

	if(!props_init(&handle->props, descriptor->URI,
		defs, MAX_NPROPS, &handle->state, &handle->stash,
		handle->map, handle))
	{
		fprintf(stderr, "failed to initialize property structure\n");
		free(handle);
		return NULL;
	}

	handle->urid.mute = props_map(&handle->props, ORBIT_URI"#timecapsule_mute");
	handle->urid.record = props_map(&handle->props, ORBIT_URI"#timecapsule_record");
	handle->urid.mute_toggle = props_map(&handle->props, ORBIT_URI"#timecapsule_mute_toggle");
	handle->urid.record_toggle = props_map(&handle->props, ORBIT_URI"#timecapsule_record_toggle");

	return handle;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void *data)
{
	plughandle_t *handle = (plughandle_t *)instance;

	switch(port)
	{
		case 0:
			handle->event_in = (const LV2_Atom_Sequence *)data;
			break;
		case 1:
			handle->event_out = (LV2_Atom_Sequence *)data;
			break;
		default:
			break;
	}
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;

	const uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	handle->ref = lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	props_idle(&handle->props, &handle->forge, 0, &handle->ref);

	int64_t last_t = 0;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		if(handle->rolling)
			handle->offset += ev->time.frames - last_t;

		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		int handled = timely_advance(&handle->timely, obj, last_t, ev->time.frames);
		if(!handled)
			handled = props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref);

		if(handle->rolling)
		{
			if(!handled && handle->state.record)
				_rec(handle, ev); // dont' record time position signals and patch messages
		
			if(!handle->state.record && !handle->state.mute)
				_play(handle, ev->time.frames);
		}

		last_t = ev->time.frames;
	}

	if(handle->rolling)
		handle->offset += nsamples - last_t;
	timely_advance(&handle->timely, NULL, last_t, nsamples);
	if(handle->rolling && !handle->state.record && !handle->state.mute)
		_play(handle, nsamples);

	if(handle->ref)
		lv2_atom_forge_pop(&handle->forge, &frame);
	else
	{
		lv2_atom_sequence_clear(handle->event_out);

		if(handle->log)
			lv2_log_trace(&handle->logger, "%s: forge buffer overflow\n", __func__);
	}
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	_close_disk(handle);

	if(handle->to_dsp)
		varchunk_free(handle->to_dsp);

	if(handle->to_worker)
		varchunk_free(handle->to_worker);

	if(handle->netatom)
		netatom_free(handle->netatom);

	munlock(handle, sizeof(plughandle_t));
	free(handle);
}

static LV2_State_Status
_state_save(LV2_Handle instance, LV2_State_Store_Function store,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

	return props_save(&handle->props, store, state, flags, features);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

	return props_restore(&handle->props, retrieve, state, flags, features);
}

static const LV2_State_Interface state_iface = {
	.save = _state_save,
	.restore = _state_restore
};

// non-rt thread
static LV2_Worker_Status
_work(LV2_Handle instance,
	LV2_Worker_Respond_Function respond,
	LV2_Worker_Respond_Handle worker,
	uint32_t size,
	const void *body)
{
	plughandle_t *handle = instance;

	const job_t *job;
	size_t tot_size;

	while((job = varchunk_read_request(handle->to_worker, &tot_size)))
	{
		switch(job->type)
		{
			case TC_JOB_REPOSITION_PLAY:
			{
				_reopen_disk(handle, false, job->beats);

				// send drain
				job_t *job2;
				if((job2 = varchunk_write_request(handle->to_dsp, sizeof(job_t))))
				{
					job2->type = TC_JOB_DRAIN;

					varchunk_write_advance(handle->to_dsp, sizeof(job_t));
				}
				else if(handle->log)
				{
					lv2_log_error(&handle->logger, "%s: ringbuffer overflow\n", __func__);
				}
			} // fall-through
			case TC_JOB_READ:
			{
				while(_read_from(handle) == 0)
					;
			} break;

			case TC_JOB_REPOSITION_REC:
			{
				_reopen_disk(handle, true, job->beats);

				// send drain
				job_t *job2;
				if((job2 = varchunk_write_request(handle->to_dsp, sizeof(job_t))))
				{
					job2->type = TC_JOB_DRAIN;

					varchunk_write_advance(handle->to_dsp, sizeof(job_t));
				}
				else if(handle->log)
				{
					lv2_log_error(&handle->logger, "%s: ringbuffer overflow\n", __func__);
				}
			} break;

			case TC_JOB_WRITE:
			{
				_write_to(handle, job->beats, job->atom);
			} break;

			case TC_JOB_CHANGE_PATH:
			{
				_close_disk(handle);
				strncpy(handle->file_path, job->file_path, PATH_MAX);
				_reopen_disk(handle, false, job->beats); // open readonly by default FIXME
			} break;

			case TC_JOB_DRAIN:
			{
				// nothing to do
			}	break;
		}

		varchunk_read_advance(handle->to_worker);
	}

	return LV2_WORKER_SUCCESS;
}

// rt-thread
static LV2_Worker_Status
_work_response(LV2_Handle instance, uint32_t size, const void *body)
{
	plughandle_t *handle = instance;

	// nothing

	return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface work_iface = {
	.work = _work,
	.work_response = _work_response,
	.end_run = NULL
};

static const void *
extension_data(const char *uri)
{
	if(!strcmp(uri, LV2_STATE__interface))
		return &state_iface;
	else if(!strcmp(uri, LV2_WORKER__interface))
		return &work_iface;
	return NULL;
}

const LV2_Descriptor orbit_timecapsule = {
	.URI						= ORBIT_TIMECAPSULE_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= NULL,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
