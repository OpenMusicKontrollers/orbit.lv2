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

#include <orbit.h>
#include <timely.h>
#include <props.h>

#define MEGA(x) ((x) << 20)
#define NANO(x) ((x) >> 20)
#define MAX_NPROPS 5

typedef struct _job_t job_t;
typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

struct _job_t {
	bool get;
	size_t size;
	struct {
		void *state;
		void *stash;
	} buf;
};

struct _plugstate_t {
	int32_t mute;
	int32_t record;
	int32_t mute_toggle;
	int32_t record_toggle;
	int32_t memory;
	uint8_t *buf;
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	LV2_Log_Log *log;
	LV2_Log_Logger logger;

	struct {
		LV2_URID beat_time;
		LV2_URID memory;
		LV2_URID mute;
		LV2_URID mute_toggle;
		LV2_URID record;
		LV2_URID record_toggle;
	} urid;
	
	timely_t timely;

	plugstate_t state;
	plugstate_t stash;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	int64_t offset;
	int64_t last;

	PROPS_T(props, MAX_NPROPS);

	unsigned play;
	bool rolling;
	LV2_Atom_Event *ev;
	atomic_flag lock;

	LV2_Worker_Schedule *sched;
	bool requested;
	bool update_memory;
};

static inline void
_seq_spin_lock(plughandle_t *handle)
{
	while(atomic_flag_test_and_set_explicit(&handle->lock, memory_order_acquire))
	{
		// spin
	}
}

static inline bool
_seq_try_lock(plughandle_t *handle)
{
	return atomic_flag_test_and_set_explicit(&handle->lock, memory_order_acquire) == false;
}

static inline void
_seq_unlock(plughandle_t *handle)
{
	atomic_flag_clear_explicit(&handle->lock, memory_order_release);
}

static void
_intercept_toggle(void *data, int64_t frames, props_impl_t *impl)
{
	plughandle_t *handle = data;

	if(handle->state.mute_toggle)
	{
		handle->state.mute_toggle = false;
		handle->state.mute = !handle->state.mute;

		props_set(&handle->props, &handle->forge, frames, handle->urid.mute_toggle, &handle->ref);
		props_set(&handle->props, &handle->forge, frames, handle->urid.mute, &handle->ref);
	}

	if(handle->state.record_toggle)
	{
		handle->state.record_toggle = false;
		handle->state.record = !handle->state.record;

		props_set(&handle->props, &handle->forge, frames, handle->urid.record_toggle, &handle->ref);
		props_set(&handle->props, &handle->forge, frames, handle->urid.record, &handle->ref);
	}
}

static const props_def_t defs [MAX_NPROPS] = {
	{
		.property = ORBIT_URI"#cargoship_mute",
		.offset = offsetof(plugstate_t, mute),
		.type = LV2_ATOM__Bool,
	},
	{
		.property = ORBIT_URI"#cargoship_record",
		.offset = offsetof(plugstate_t, record),
		.type = LV2_ATOM__Bool,
	},
	{
		.property = ORBIT_URI"#cargoship_mute_toggle",
		.offset = offsetof(plugstate_t, mute_toggle),
		.type = LV2_ATOM__Bool,
		.event_cb = _intercept_toggle
	},
	{
		.property = ORBIT_URI"#cargoship_record_toggle",
		.offset = offsetof(plugstate_t, record_toggle),
		.type = LV2_ATOM__Bool,
		.event_cb = _intercept_toggle
	},
	{
		.property = ORBIT_URI"#cargoship_memory",
		.offset = offsetof(plugstate_t, memory),
		.access = LV2_PATCH__readable,
		.type = LV2_ATOM__Int,
	}
};

static inline void
_request(plughandle_t *handle, int32_t size)
{
	if(!handle->requested && (size*2 > MEGA(handle->state.memory)) )
	{
		const job_t job = {
			.get = true,
			.size = handle->state.memory << 1
		};

		LV2_Worker_Status status = handle->sched->schedule_work(
			handle->sched->handle, sizeof(job_t), &job); //FIXME check

		handle->requested = true;
	}
}

static inline void
_play(plughandle_t *handle, int64_t to, uint32_t capacity)
{
	const LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)handle->state.buf;

	const int64_t rel = handle->offset - to; // beginning of current period

	while(handle->ev && !lv2_atom_sequence_is_end(&seq->body, seq->atom.size, handle->ev))
	{
		const int64_t beat_frames = handle->ev->time.beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

		if(beat_frames >= handle->offset)
			break; // event not part of this period

		const int64_t frames = beat_frames - rel;

		// check for time jump! skip out-of-order event, as it probably has already been forged...
		if(frames >= handle->last) //TODO can this be solved more elegantly?
		{
			// append event
			const LV2_Atom *atom = &handle->ev->body;
			if(handle->ref)
				handle->ref = lv2_atom_forge_frame_time(&handle->forge, frames);
			if(handle->ref)
				handle->ref = lv2_atom_forge_raw(&handle->forge, atom, lv2_atom_total_size(atom));
			if(handle->ref)
				lv2_atom_forge_pad(&handle->forge, atom->size);

			handle->last = frames; // advance frame time head
		}

		handle->ev = lv2_atom_sequence_next(handle->ev);
	}
}

static inline void
_rec(plughandle_t *handle, const LV2_Atom_Event *ev)
{
	LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)handle->state.buf;
	const uint32_t size = seq->atom.size;

	LV2_Atom_Event *e = lv2_atom_sequence_append_event(seq, MEGA(handle->state.memory), ev);
	if(e)
	{
		e->time.beats = handle->offset / TIMELY_FRAMES_PER_BEAT(&handle->timely);
	}
	else
	{
		// overflow
	}

	_request(handle, lv2_atom_total_size(&seq->atom));

	// try to append to stash, too
	if(_seq_try_lock(handle))
	{
		seq = (LV2_Atom_Sequence *)handle->state.buf;
		if(seq->atom.size == size) // only append last event
		{
			e = lv2_atom_sequence_append_event(seq, MEGA(handle->state.memory), ev);
			if(e)
			{
				e->time.beats = handle->offset / TIMELY_FRAMES_PER_BEAT(&handle->timely);
			}
			else
			{
				// overflow
			}
		}
		else // copy whole sequence
		{
			memcpy(handle->stash.buf, handle->state.buf, lv2_atom_total_size(&seq->atom));
		}

		_seq_unlock(handle);
	}
}

static inline void
_reposition_play(plughandle_t *handle)
{
	LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)handle->state.buf;

	LV2_ATOM_SEQUENCE_FOREACH(seq, ev)
	{
		const int64_t beat_frames = ev->time.beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

		if(beat_frames >= handle->offset)
		{
			// reposition here
			handle->ev = ev;
			return;
		}
	}

	handle->ev = NULL;
}

static inline void
_reposition_rec(plughandle_t *handle)
{
	LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)handle->state.buf;

	LV2_ATOM_SEQUENCE_FOREACH(seq, ev)
	{
		const int64_t beat_frames = ev->time.beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

		if(beat_frames >= handle->offset)
		{
			// truncate sequence here
			seq->atom.size = (uintptr_t)ev - (uintptr_t)&seq->body;
			return;
		}
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

		handle->offset = beats * TIMELY_FRAMES_PER_BEAT(timely);

		if(handle->state.record)
			_reposition_rec(handle);
		else
			_reposition_play(handle);
	}
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

	if(!handle->sched)
	{
		fprintf(stderr,
			"%s: Host does not support worker:schedule\n", descriptor->URI);
		free(handle);
		return NULL;
	}

	if(handle->log)
		lv2_log_logger_init(&handle->logger, handle->map, handle->log);

	handle->urid.beat_time = handle->map->map(handle->map->handle, LV2_ATOM__beatTime);

	timely_mask_t mask = TIMELY_MASK_BAR_BEAT
		//| TIMELY_MASK_BAR
		| TIMELY_MASK_BEAT_UNIT
		| TIMELY_MASK_BEATS_PER_BAR
		| TIMELY_MASK_BEATS_PER_MINUTE
		| TIMELY_MASK_FRAMES_PER_SECOND
		| TIMELY_MASK_SPEED
		| TIMELY_MASK_BAR_BEAT_WHOLE;
	timely_init(&handle->timely, handle->map, rate, mask, _cb, handle);
	lv2_atom_forge_init(&handle->forge, handle->map);

	if(!props_init(&handle->props, descriptor->URI,
		defs, MAX_NPROPS, &handle->state, &handle->stash,
		handle->map, handle))
	{
		fprintf(stderr, "failed to initialize property structure\n");
		free(handle);
		return NULL;
	}

	handle->urid.memory = props_map(&handle->props, ORBIT_URI"#cargoship_memory");
	handle->urid.mute = props_map(&handle->props, ORBIT_URI"#cargoship_mute");
	handle->urid.mute_toggle = props_map(&handle->props, ORBIT_URI"#cargoship_mute_toggle");
	handle->urid.record = props_map(&handle->props, ORBIT_URI"#cargoship_record");
	handle->urid.record_toggle = props_map(&handle->props, ORBIT_URI"#cargoship_record_toggle");

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
activate(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	atomic_flag_clear_explicit(&handle->lock, memory_order_relaxed);
	handle->offset = 0.f;
	handle->play = 0;
	handle->rolling = false;

	handle->state.memory = 1; // 1MB
	handle->update_memory = true;

	handle->state.buf = malloc(MEGA(handle->state.memory));
	handle->stash.buf = malloc(MEGA(handle->state.memory));
	mlock(handle->state.buf, MEGA(handle->state.memory));
	mlock(handle->stash.buf, MEGA(handle->state.memory));

	LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)handle->state.buf;

	seq->atom.type = handle->forge.Sequence;
	seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
	seq->body.unit = handle->urid.beat_time;
	seq->body.pad = 0;
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;

	handle->last = 0; // reset frame time head

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
				_play(handle, ev->time.frames, capacity);
		}

		last_t = ev->time.frames;
	}

	if(handle->rolling)
		handle->offset += nsamples - last_t;
	timely_advance(&handle->timely, NULL, last_t, nsamples);
	if(handle->rolling && !handle->state.record && !handle->state.mute)
		_play(handle, nsamples, capacity);

	if(handle->update_memory)
	{
		props_set(&handle->props, &handle->forge, nsamples-1, handle->urid.memory, &handle->ref);
		handle->update_memory = false;
	}

	if(handle->ref)
		lv2_atom_forge_pop(&handle->forge, &frame);
	else
	{
		lv2_atom_sequence_clear(handle->event_out);

		if(handle->log)
			lv2_log_trace(&handle->logger, "forge buffer overflow\n");
	}
}

static void
deactivate(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	munlock(handle->state.buf, MEGA(handle->state.memory));
	munlock(handle->stash.buf, MEGA(handle->state.memory));
	free(handle->state.buf);
	free(handle->stash.buf);
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	munlock(handle, sizeof(plughandle_t));
	free(handle);
}

static LV2_State_Status
_state_save(LV2_Handle instance, LV2_State_Store_Function store,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

	const LV2_Atom_Sequence *seq = (const LV2_Atom_Sequence *)handle->stash.buf;

	store(state, handle->forge.Sequence, LV2_ATOM_BODY_CONST(&seq->atom),
		seq->atom.size, seq->atom.type, flags | LV2_STATE_IS_POD); //TODO check

	return props_save(&handle->props, store, state, flags, features);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

	size_t size;
	uint32_t type;
	uint32_t _flags;
	const LV2_Atom_Sequence_Body *body = retrieve(state, handle->forge.Sequence, &size, &type, &_flags);
	if(body && (type == handle->forge.Sequence) )
	{
		const size_t total_size = sizeof(LV2_Atom) + size;

		size_t size2 = 1;
		while( (size2 < total_size) || (size2 < MEGA(1)) )
			size2 <<= 1; // assure size2 to be a power of 2 and > 1MB

		handle->state.memory = NANO(size2);
		handle->update_memory = true;

		handle->state.buf = realloc(handle->state.buf, size2);
		handle->stash.buf = realloc(handle->stash.buf, size2);

		LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)handle->state.buf;
		seq->atom.type = type;
		seq->atom.size = size;
		memcpy(&seq->body, body, size);
		memcpy(handle->stash.buf, handle->state.buf, total_size);
	}

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

	const job_t *job_in = body;
	if(job_in->get)
	{
		const job_t job_out = {
			.get = true,
			.size = job_in->size,
			.buf = {
				.state = malloc(MEGA(job_in->size)),
				.stash = malloc(MEGA(job_in->size))
			}
		};

		LV2_Worker_Status status = respond(worker, sizeof(job_t), &job_out);
	}
	else
	{
		munlock(job_in->buf.state, MEGA(job_in->size));
		munlock(job_in->buf.stash, MEGA(job_in->size));
		free(job_in->buf.state);
		free(job_in->buf.stash);
	}

	return LV2_WORKER_SUCCESS;
}

// rt-thread
static LV2_Worker_Status
_work_response(LV2_Handle instance, uint32_t size, const void *body)
{
	plughandle_t *handle = instance;

	const job_t *job_in = body;

	if(job_in->get)
	{
		// copy old buffers into new ones
		memcpy(job_in->buf.state, handle->state.buf, MEGA(handle->state.memory));
		memcpy(job_in->buf.stash, handle->stash.buf, MEGA(handle->state.memory));

		// fill job to delete old buffers
		const job_t job_out = {
			.get = false,
			.size = handle->state.memory,
			.buf = {
				.state = handle->state.buf,
				.stash = handle->stash.buf
			}
		};

		// send job to delete old buffers
		LV2_Worker_Status status = handle->sched->schedule_work(handle->sched->handle,
			sizeof(job_t), &job_out);

		// swap old and new buffers
		handle->state.memory = job_in->size;
		handle->update_memory = true;

		handle->state.buf = job_in->buf.state;
		handle->stash.buf = job_in->buf.stash;

		handle->requested = false;
	}
	else
	{
		// nothing
	}

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

const LV2_Descriptor orbit_cargoship = {
	.URI						= ORBIT_CARGOSHIP_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= deactivate,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
