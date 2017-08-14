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
#include <props.h>

#define MAX_NPROPS 5

typedef struct _position_t position_t;
typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

struct _position_t {
	float bar_beat;
	int64_t bar;

	uint32_t beat_unit;
	float beats_per_bar;
	float beats_per_minute;

	int64_t frame;
	float frames_per_second;

	float speed;
};

struct _plugstate_t {
	int32_t beat_unit;
	int32_t beats_per_bar;
	int32_t beats_per_minute;
	int32_t rolling;
	int32_t rewind;
};

struct _plughandle_t {
	LV2_URID_Map *map;

	LV2_Log_Log *log;
	LV2_Log_Logger logger;

	struct {
		LV2_URID time_position;
		LV2_URID time_barBeat;
		LV2_URID time_bar;
		LV2_URID time_beatUnit;
		LV2_URID time_beatsPerBar;
		LV2_URID time_beatsPerMinute;
		LV2_URID time_frame;
		LV2_URID time_framesPerSecond;
		LV2_URID time_speed;

		LV2_URID rolling;
	} urid;

	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	position_t pos;

	double frames_per_bar;
	double frames_per_beat;
	double rel;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	plugstate_t state;
	plugstate_t stash;

	PROPS_T(props, MAX_NPROPS);
};

static inline LV2_Atom_Forge_Ref
_position_atomize(plughandle_t *handle, LV2_Atom_Forge *forge, int64_t frames,
	position_t *pos)
{
	LV2_Atom_Forge_Frame frame;

	LV2_Atom_Forge_Ref ref = lv2_atom_forge_frame_time(forge, frames);
	if(ref)
		ref = lv2_atom_forge_object(forge, &frame, 0, handle->urid.time_position);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_barBeat);
	if(ref)
		ref = lv2_atom_forge_float(forge, pos->bar_beat);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_bar);
	if(ref)
		ref = lv2_atom_forge_long(forge, pos->bar);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_beatUnit);
	if(ref)
		ref = lv2_atom_forge_int(forge, pos->beat_unit);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_beatsPerBar);
	if(ref)
		ref = lv2_atom_forge_float(forge, pos->beats_per_bar);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_beatsPerMinute);
	if(ref)
		ref = lv2_atom_forge_float(forge, pos->beats_per_minute);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_frame);
	if(ref)
		ref = lv2_atom_forge_long(forge, pos->frame);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_framesPerSecond);
	if(ref)
		ref = lv2_atom_forge_float(forge, pos->frames_per_second);

	if(ref)
		ref = lv2_atom_forge_key(forge, handle->urid.time_speed);
	if(ref)
		ref = lv2_atom_forge_float(forge, pos->speed);

	if(ref)
		lv2_atom_forge_pop(forge, &frame);

	return ref;
}

static void
_intercept(void *data, int64_t frames, props_impl_t *impl)
{
	plughandle_t *handle = data;
	position_t *pos = &handle->pos;

	// update mirror props
	pos->beat_unit = handle->state.beat_unit;
	pos->beats_per_bar = handle->state.beats_per_bar;
	pos->beats_per_minute = handle->state.beats_per_minute;
	pos->speed = handle->state.rolling ? 1.f : 0.f;

	// derive current position as bar_beat
	pos->bar_beat = handle->rel / handle->frames_per_bar * pos->beats_per_bar;

	if( (impl->property == handle->urid.rolling) && handle->state.rewind) // start/stop rolling
	{
		pos->frame = 0; // reset frame pointer
		pos->bar = 0; // reset bar
		pos->bar_beat = 0.f; // reset beat
		handle->rel = 0.0;
	}

	if(pos->bar_beat >= pos->beats_per_bar) // move to end of bar if beat overflows bar
	{
		double integral;
		float frac = modf(pos->bar_beat, &integral);
		(void)integral;
		pos->bar_beat = pos->beats_per_bar - 1.f + frac;
	}

	if(handle->ref)
		handle->ref = _position_atomize(handle, &handle->forge, frames, pos);

	// update frames_per_beat and frames_per_bar
	handle->frames_per_beat = 240.0 / (pos->beats_per_minute * pos->beat_unit) * pos->frames_per_second;
	handle->frames_per_bar = handle->frames_per_beat * pos->beats_per_bar;
}

static const props_def_t defs [MAX_NPROPS] = {
	{
		.property = ORBIT_URI"#pacemaker_beat_unit",
		.offset = offsetof(plugstate_t, beat_unit),
		.type = LV2_ATOM__Int,
		.event_cb = _intercept
	},
	{
		.property = ORBIT_URI"#pacemaker_beats_per_bar",
		.offset = offsetof(plugstate_t, beats_per_bar),
		.type = LV2_ATOM__Int,
		.event_cb = _intercept
	},
	{
		.property = ORBIT_URI"#pacemaker_beats_per_minute",
		.offset = offsetof(plugstate_t, beats_per_minute),
		.type = LV2_ATOM__Int,
		.event_cb = _intercept
	},
	{
		.property = ORBIT_URI"#pacemaker_rolling",
		.offset = offsetof(plugstate_t, rolling),
		.type = LV2_ATOM__Bool,
		.event_cb = _intercept
	},
	{
		.property = ORBIT_URI"#pacemaker_rewind",
		.offset = offsetof(plugstate_t, rewind),
		.type = LV2_ATOM__Bool,
	}
};

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate,
	const char *bundle_path, const LV2_Feature *const *features)
{
	int i;
	plughandle_t *handle = calloc(1, sizeof(plughandle_t));
	if(!handle)
		return NULL;
	mlock(handle, sizeof(plughandle_t));

	handle->pos.frames_per_second = rate;

	for(i=0; features[i]; i++)
	{
		if(!strcmp(features[i]->URI, LV2_URID__map))
			handle->map = features[i]->data;
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

	if(handle->log)
		lv2_log_logger_init(&handle->logger, handle->map, handle->log);

	handle->urid.time_position = handle->map->map(handle->map->handle,
		LV2_TIME__Position);
	handle->urid.time_barBeat = handle->map->map(handle->map->handle,
		LV2_TIME__barBeat);
	handle->urid.time_bar = handle->map->map(handle->map->handle,
		LV2_TIME__bar);
	handle->urid.time_beatUnit = handle->map->map(handle->map->handle,
		LV2_TIME__beatUnit);
	handle->urid.time_beatsPerBar = handle->map->map(handle->map->handle,
		LV2_TIME__beatsPerBar);
	handle->urid.time_beatsPerMinute = handle->map->map(handle->map->handle,
		LV2_TIME__beatsPerMinute);
	handle->urid.time_frame = handle->map->map(handle->map->handle,
		LV2_TIME__frame);
	handle->urid.time_framesPerSecond = handle->map->map(handle->map->handle,
		LV2_TIME__framesPerSecond);
	handle->urid.time_speed = handle->map->map(handle->map->handle,
		LV2_TIME__speed);

	lv2_atom_forge_init(&handle->forge, handle->map);

	if(!props_init(&handle->props, descriptor->URI,
		defs, MAX_NPROPS, &handle->state, &handle->stash,
		handle->map, handle))
	{
		fprintf(stderr, "failed to initialize property structure\n");
		free(handle);
		return NULL;
	}

	handle->urid.rolling = props_map(&handle->props, ORBIT_URI"#pacemaker_rolling");

	return handle;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void *data)
{
	plughandle_t *handle = instance;

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
	position_t *pos = &handle->pos;

	pos->frame = 0;
	pos->bar = 0;
	pos->bar_beat = 0.f;

	pos->beat_unit = 4;
	pos->beats_per_bar = 4.f;
	pos->beats_per_minute = 120.0f;

	handle->frames_per_beat = 240.0 / (pos->beats_per_minute * pos->beat_unit) * pos->frames_per_second;
	handle->frames_per_bar = handle->frames_per_beat * pos->beats_per_bar;
	handle->rel = 0.0;
}

static inline void
_advance(plughandle_t *handle, uint32_t from, uint32_t to)
{
	position_t *pos = &handle->pos;
	const uint32_t nsamples = to - from;

	if(pos->speed > 0.f)
	{
		// update frame position
		pos->frame += nsamples * pos->speed;

		// update rel position
		handle->rel += nsamples;
		if(handle->rel >= handle->frames_per_bar)
		{
			pos->bar += 1;
			handle->rel -= handle->frames_per_bar;
		}
	}
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	uint32_t last_t = 0;
	plughandle_t *handle = instance;
	position_t *pos = &handle->pos;

	uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	handle->ref = lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	props_idle(&handle->props, &handle->forge, 0, &handle->ref);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;

		_advance(handle, last_t, ev->time.frames);
		props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref);

		last_t = ev->time.frames;
	}
	_advance(handle, last_t, nsamples);

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
	plughandle_t *handle = instance;

	return props_save(&handle->props, store, state, flags, features);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = instance;

	return props_restore(&handle->props, retrieve, state, flags, features);
}

static const LV2_State_Interface state_iface = {
	.save = _state_save,
	.restore = _state_restore
};

static const void*
extension_data(const char* uri)
{
	if(!strcmp(uri, LV2_STATE__interface))
		return &state_iface;

	return NULL;
}

const LV2_Descriptor orbit_pacemaker = {
	.URI						= ORBIT_PACEMAKER_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
