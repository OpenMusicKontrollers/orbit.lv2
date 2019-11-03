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

#define MAX_NPROPS 8

typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

struct _plugstate_t {
	float bar_beat;
	int64_t bar;

	int32_t beat_unit;
	float beats_per_bar;
	float beats_per_minute;

	int64_t frame;
	float frames_per_second;

	float speed;
};

enum {
	MODE_MULTIPLY = 0,
	MODE_DIVIDE = 1
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	LV2_Log_Log *log;
	LV2_Log_Logger logger;

	LV2_URID time_barBeat;
	LV2_URID time_bar;
	LV2_URID time_beatUnit;
	LV2_URID time_beatsPerBar;
	LV2_URID time_beatsPerMinute;
	LV2_URID time_frame;
	LV2_URID time_framesPerSecond;
	LV2_URID time_speed;

	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	plugstate_t state;
	plugstate_t stash;

	PROPS_T(props, MAX_NPROPS);
};

static const props_def_t defs [MAX_NPROPS] = {
	{
		.property = ORBIT_URI"#monitor_barBeat",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, bar_beat),
		.type = LV2_ATOM__Float
	},
	{
		.property = ORBIT_URI"#monitor_bar",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, bar),
		.type = LV2_ATOM__Long
	},
	{
		.property = ORBIT_URI"#monitor_beatUnit",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, beat_unit),
		.type = LV2_ATOM__Int
	},
	{
		.property = ORBIT_URI"#monitor_beatsPerBar",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, beats_per_bar),
		.type = LV2_ATOM__Float
	},
	{
		.property = ORBIT_URI"#monitor_beatsPerMinute",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, beats_per_minute),
		.type = LV2_ATOM__Float
	},
	{
		.property = ORBIT_URI"#monitor_frame",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, frame),
		.type = LV2_ATOM__Long
	},
	{
		.property = ORBIT_URI"#monitor_framesPerSecond",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, frames_per_second),
		.type = LV2_ATOM__Float
	},
	{
		.property = ORBIT_URI"#monitor_speed",
		.access = LV2_PATCH__readable,
		.offset = offsetof(plugstate_t, speed),
		.type = LV2_ATOM__Float
	},
};

static void
_cb(timely_t *timely, int64_t frames, LV2_URID type, void *data)
{
	plughandle_t *handle = data;

	// do nothing
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

	const timely_mask_t mask = 0;
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

	handle->time_barBeat = props_map(&handle->props, ORBIT_URI"#monitor_barBeat");
	handle->time_bar = props_map(&handle->props, ORBIT_URI"#monitor_bar");
	handle->time_beatUnit = props_map(&handle->props, ORBIT_URI"#monitor_beatUnit");
	handle->time_beatsPerBar = props_map(&handle->props, ORBIT_URI"#monitor_beatsPerBar");
	handle->time_beatsPerMinute = props_map(&handle->props, ORBIT_URI"#monitor_beatsPerMinute");
	handle->time_frame = props_map(&handle->props, ORBIT_URI"#monitor_frame");
	handle->time_framesPerSecond = props_map(&handle->props, ORBIT_URI"#monitor_framesPerSecond");
	handle->time_speed = props_map(&handle->props, ORBIT_URI"#monitor_speed");

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

static inline void
_position_atomize(plughandle_t *handle, LV2_Atom_Forge *forge, int64_t frames,
	LV2_Atom_Forge_Ref *ref)
{
	timely_t *timely = &handle->timely;

	handle->state.bar_beat = TIMELY_BAR_BEAT(timely);
	handle->state.bar = TIMELY_BAR(timely);
	handle->state.beat_unit = TIMELY_BEAT_UNIT(timely);
	handle->state.beats_per_bar = TIMELY_BEATS_PER_BAR(timely);
	handle->state.beats_per_minute = TIMELY_BEATS_PER_MINUTE(timely);
	handle->state.frame = TIMELY_FRAME(timely);
	handle->state.frames_per_second = TIMELY_FRAMES_PER_SECOND(timely);
	handle->state.speed = TIMELY_SPEED(timely);

	props_set(&handle->props, forge, frames, handle->time_barBeat, ref);
	props_set(&handle->props, forge, frames, handle->time_bar, ref);
	props_set(&handle->props, forge, frames, handle->time_beatUnit, ref);
	props_set(&handle->props, forge, frames, handle->time_beatsPerBar, ref);
	props_set(&handle->props, forge, frames, handle->time_beatsPerMinute, ref);
	props_set(&handle->props, forge, frames, handle->time_frame, ref);
	props_set(&handle->props, forge, frames, handle->time_framesPerSecond, ref);
	props_set(&handle->props, forge, frames, handle->time_speed, ref);
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	uint32_t last_t = 0;

	const uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	handle->ref = lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	props_idle(&handle->props, &handle->forge, 0, &handle->ref);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;

		if(timely_advance(&handle->timely, obj, last_t, ev->time.frames))
		{
			// nothing to do
		}
		else
		{
			props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref);
		}

		last_t = ev->time.frames;
	}

	timely_advance(&handle->timely, NULL, last_t, nsamples);

	_position_atomize(handle, &handle->forge, nsamples - 1, &handle->ref);

	if(handle->ref)
	{
		lv2_atom_forge_pop(&handle->forge, &frame);
	}
	else
	{
		lv2_atom_sequence_clear(handle->event_out);

		if(handle->log)
			lv2_log_trace(&handle->logger, "forge buffer overflow\n");
	}
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

const LV2_Descriptor orbit_monitor = {
	.URI						= ORBIT_MONITOR_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= NULL,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
