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

#define MAX_NPROPS 7

#define BUF_SIZE 0x1000000 // 16 MB
#define BUF_PERCENT (100.f / (BUF_SIZE - sizeof(LV2_Atom)))

typedef enum _punchmode_t punchmode_t;
typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

enum _punchmode_t {
	PUNCH_BEAT				= 0,
	PUNCH_BAR					= 1
};

struct _plugstate_t {
	int32_t punch;
	int32_t width;
	int32_t mute;
	int32_t switsch;

	float play_capacity;
	float rec_capacity;
	float position;
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	struct {
		LV2_URID play_capacity;
		LV2_URID rec_capacity;
		LV2_URID position;
		LV2_URID beat_time;
		LV2_URID recording;
	} urid;
	
	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	plugstate_t state;
	plugstate_t stash;

	float window;
	int64_t offset;
	int64_t last;

	PROPS_T(props, MAX_NPROPS);

	unsigned play;
	bool rolling;
	uint8_t buf [2][BUF_SIZE];
	LV2_Atom_Event *ev;
};

static inline void
_window_refresh(plughandle_t *handle)
{
	timely_t *timely = &handle->timely;

	if(handle->state.punch == PUNCH_BEAT)
		handle->window = 100.f / (handle->state.width * TIMELY_FRAMES_PER_BEAT(timely));
	else if(handle->state.punch == PUNCH_BAR)
		handle->window = 100.f / (handle->state.width * TIMELY_FRAMES_PER_BAR(timely));
}

static void
_intercept(void *data, LV2_Atom_Forge *forge, int64_t frames,
	props_event_t event, props_impl_t *impl)
{
	plughandle_t *handle = data;

	_window_refresh(handle);
}

static const props_def_t stat_punch = {
	.property = ORBIT_URI"#cargoship_punch",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC,
	.event_mask = PROP_EVENT_WRITE,
	.event_cb = _intercept
};

static const props_def_t stat_width = {
	.property = ORBIT_URI"#cargoship_width",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC,
	.event_mask = PROP_EVENT_WRITE,
	.event_cb = _intercept
};

static const props_def_t stat_mute = {
	.property = ORBIT_URI"#cargoship_mute",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Bool,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_switch = {
	.property = ORBIT_URI"#cargoship_switch",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Bool,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_play_capacity = {
	.property = ORBIT_URI"#cargoship_play_capacity",
	.access = LV2_PATCH__readable,
	.type = LV2_ATOM__Float,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_rec_capacity = {
	.property = ORBIT_URI"#cargoship_rec_capacity",
	.access = LV2_PATCH__readable,
	.type = LV2_ATOM__Float,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_position = {
	.property = ORBIT_URI"#cargoship_position",
	.access = LV2_PATCH__readable,
	.type = LV2_ATOM__Float,
	.mode = PROP_MODE_STATIC
};

static inline void
_play(plughandle_t *handle, int64_t to, uint32_t capacity)
{
	const LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	const int64_t rel = handle->offset - to; // beginning of current period

	while(handle->ev && !lv2_atom_sequence_is_end(&play_seq->body, play_seq->atom.size, handle->ev))
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
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	LV2_Atom_Event *e = lv2_atom_sequence_append_event(rec_seq, BUF_SIZE, ev);
	if(e)
	{
		e->time.beats = handle->offset / TIMELY_FRAMES_PER_BEAT(&handle->timely);
	}
	else
	{
		// overflow
	}
}

static inline void
_reposition_play(plughandle_t *handle)
{
	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	LV2_ATOM_SEQUENCE_FOREACH(play_seq, ev)
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
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	LV2_ATOM_SEQUENCE_FOREACH(rec_seq, ev)
	{
		const int64_t beat_frames = ev->time.beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

		if(beat_frames >= handle->offset)
		{
			// truncate sequence here
			rec_seq->atom.size = (uintptr_t)ev - (uintptr_t)&rec_seq->body;
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
		double beats = (double)TIMELY_BAR(timely) * TIMELY_BEATS_PER_BAR(timely)
			+ TIMELY_BAR_BEAT_RAW(timely);

		if(handle->state.punch == PUNCH_BEAT)
			handle->offset = fmod(beats, handle->state.width) * TIMELY_FRAMES_PER_BEAT(timely);
		else if(handle->state.punch == PUNCH_BAR)
			handle->offset = fmod(beats, handle->state.width * TIMELY_BEATS_PER_BAR(timely))
				* TIMELY_FRAMES_PER_BEAT(timely);

		if(handle->state.switsch && (handle->offset == 0) )
			handle->play ^= 1;

		if(beats == 0.0) // clear sequence buffers when transport is rewound
		{
			LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
			LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

			lv2_atom_sequence_clear(play_seq);
			lv2_atom_sequence_clear(rec_seq);
		}

		_reposition_rec(handle);
		_reposition_play(handle);
	}

	_window_refresh(handle);
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
	}

	if(!handle->map)
	{
		fprintf(stderr,
			"%s: Host does not support urid:map\n", descriptor->URI);
		free(handle);
		return NULL;
	}

	handle->urid.beat_time = handle->map->map(handle->map->handle, LV2_ATOM__beatTime);
	handle->urid.recording = handle->map->map(handle->map->handle, ORBIT_URI"#recording");

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

	if(!props_init(&handle->props, MAX_NPROPS, descriptor->URI, handle->map, handle))
	{
		fprintf(stderr, "failed to initialize property structure\n");
		free(handle);
		return NULL;
	}

	if(  !props_register(&handle->props, &stat_punch, &handle->state.punch, &handle->stash.punch)
		|| !props_register(&handle->props, &stat_width, &handle->state.width, &handle->stash.width)
		|| !props_register(&handle->props, &stat_mute, &handle->state.mute, &handle->stash.mute)
		|| !props_register(&handle->props, &stat_switch, &handle->state.switsch, &handle->stash.switsch)

		|| !(handle->urid.play_capacity = props_register(&handle->props, &stat_play_capacity, &handle->state.play_capacity, &handle->stash.play_capacity))
		|| !(handle->urid.rec_capacity = props_register(&handle->props, &stat_rec_capacity, &handle->state.rec_capacity, &handle->stash.rec_capacity))
		|| !(handle->urid.position = props_register(&handle->props, &stat_position, &handle->state.position, &handle->stash.position)) )
	{
		fprintf(stderr, "failed to register properties\n");
		free(handle);
		return NULL;
	}

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

	handle->offset = 0.f;
	handle->play = 0;
	handle->rolling = false;

	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	play_seq->atom.type = handle->forge.Sequence;
	play_seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
	play_seq->body.unit = handle->urid.beat_time;
	play_seq->body.pad = 0;

	rec_seq->atom.type = handle->forge.Sequence;
	rec_seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
	rec_seq->body.unit = handle->urid.beat_time;
	rec_seq->body.pad = 0;
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

	int64_t last_t = 0;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		if(handle->rolling)
			handle->offset += ev->time.frames - last_t;

		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		int handled = timely_advance(&handle->timely, obj, last_t, ev->time.frames);
		if(!handled)
			handled = props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref);

		if(!handled && handle->rolling)
			_rec(handle, ev); // dont' record time position signals and patch messages
	
		if(!handle->state.mute && handle->rolling)
			_play(handle, ev->time.frames, capacity);

		last_t = ev->time.frames;
	}

	if(handle->rolling)
		handle->offset += nsamples - last_t;
	timely_advance(&handle->timely, NULL, last_t, nsamples);
	if(!handle->state.mute && handle->rolling)
		_play(handle, nsamples, capacity);

	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	const float play_capacity = BUF_PERCENT * play_seq->atom.size;
	const float rec_capacity = BUF_PERCENT * rec_seq->atom.size;
	const float position = handle->offset * handle->window;

	if(handle->ref && (fabsf(play_capacity - handle->state.play_capacity) > 0.1) ) //FIXME
	{
		handle->state.play_capacity = play_capacity;
		props_set(&handle->props, &handle->forge, nsamples-1, handle->urid.play_capacity, &handle->ref);
	}
	if(handle->ref && (fabsf(rec_capacity - handle->state.rec_capacity) > 0.1) ) //FIXME
	{
		handle->state.rec_capacity = rec_capacity;
		props_set(&handle->props, &handle->forge, nsamples-1, handle->urid.rec_capacity, &handle->ref);
	}
	if(handle->ref && (fabsf(position - handle->state.position) > 0.1) ) //FIXME
	{
		handle->state.position = position;
		props_set(&handle->props, &handle->forge, nsamples-1, handle->urid.position, &handle->ref);
	}

	if(handle->ref)
		lv2_atom_forge_pop(&handle->forge, &frame);
	else
		lv2_atom_sequence_clear(handle->event_out);
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

	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	store(state, handle->urid.recording, &rec_seq->body, rec_seq->atom.size,
		rec_seq->atom.type, flags); //TODO check return

	return props_save(&handle->props, &handle->forge, store, state, flags, features);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	uint32_t _flags;
	size_t _size;
	uint32_t _type;
	const LV2_Atom_Sequence_Body *body = retrieve(state, handle->urid.recording,
		&_size, &_type, &_flags);

	if(body)
	{
		rec_seq->atom.size = _size;
		rec_seq->atom.type = _type;
		memcpy(&rec_seq->body, body, rec_seq->atom.size);
	}

	return props_restore(&handle->props, &handle->forge, retrieve, state, flags, features);
}

static const LV2_State_Interface state_iface = {
	.save = _state_save,
	.restore = _state_restore
};

static const void *
extension_data(const char *uri)
{
	if(!strcmp(uri, LV2_STATE__interface))
		return &state_iface;
	return NULL;
}

const LV2_Descriptor orbit_cargoship = {
	.URI						= ORBIT_CARGOSHIP_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
