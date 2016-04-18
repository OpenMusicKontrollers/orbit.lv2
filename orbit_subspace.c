/*
 * Copyright (c) 2015 Hanspeter Portner (dev@open-music-kontrollers.ch)
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

#define MAX_NPROPS 2

typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

struct _plugstate_t {
	int32_t mode;
	int32_t factor;
};

enum {
	MODE_MULTIPLY = 0,
	MODE_DIVIDE = 1
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	float bar_beat;
	int32_t beat_unit;
	float beats_per_bar;

	plugstate_t state;
	plugstate_t stash;

	PROPS_T(props, MAX_NPROPS);
};

static inline LV2_Atom_Forge_Ref
_position_atomize(plughandle_t *handle, LV2_Atom_Forge *forge, int64_t frames)
{
	timely_t *timely = &handle->timely;

	// derive time attributes influenced by mode/factor
	float bar_beat = TIMELY_BAR_BEAT(timely);
	int32_t beat_unit = TIMELY_BEAT_UNIT(timely);
	float beats_per_bar = TIMELY_BEATS_PER_BAR(timely);

	if(handle->state.mode == MODE_MULTIPLY)
	{
		bar_beat *= handle->state.factor;
		beat_unit *= handle->state.factor;
		beats_per_bar *= handle->state.factor;
	}
	else
	{
		bar_beat /= handle->state.factor;
		beat_unit /= handle->state.factor;
		beats_per_bar /= handle->state.factor;
	}

	LV2_Atom_Forge_Frame frame;
	LV2_Atom_Forge_Ref ref = lv2_atom_forge_frame_time(forge, frames);
	if(ref)
		ref = lv2_atom_forge_object(forge, &frame, 0, timely->urid.time_position);

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_BAR_BEAT(timely));
	if(ref)
		ref = lv2_atom_forge_float(forge, bar_beat);

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_BAR(timely));
	if(ref)
		ref = lv2_atom_forge_long(forge, TIMELY_BAR(timely));

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_BEAT_UNIT(timely));
	if(ref)
		ref = lv2_atom_forge_int(forge, beat_unit);

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_BEATS_PER_BAR(timely));
	if(ref)
		ref = lv2_atom_forge_float(forge, beats_per_bar);

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_BEATS_PER_MINUTE(timely));
	if(ref)
		ref = lv2_atom_forge_float(forge, TIMELY_BEATS_PER_MINUTE(timely));

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_FRAME(timely));
	if(ref)
		ref = lv2_atom_forge_long(forge, TIMELY_FRAME(timely));

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_FRAMES_PER_SECOND(timely));
	if(ref)
		ref = lv2_atom_forge_float(forge, TIMELY_FRAMES_PER_SECOND(timely));

	if(ref)
		ref = lv2_atom_forge_key(forge, TIMELY_URI_SPEED(timely));
	if(ref)
		ref = lv2_atom_forge_float(forge, TIMELY_SPEED(timely));

	if(ref)
		lv2_atom_forge_pop(forge, &frame);

	return ref;
}

static void
_intercept(void *data, LV2_Atom_Forge *forge, int64_t frames,
	props_event_t event, props_impl_t *impl)
{
	plughandle_t *handle = data;

	if(handle->ref)
		handle->ref = _position_atomize(handle, forge, frames);
}

static const props_def_t stat_mode = {
	.property = ORBIT_URI"#subspace_mode",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC,
	.event_mask = PROP_EVENT_WRITE,
	.event_cb = _intercept
};

static const props_def_t stat_factor = {
	.property = ORBIT_URI"#subspace_factor",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC,
	.event_mask = PROP_EVENT_WRITE,
	.event_cb = _intercept
};

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

	const timely_mask_t mask = 0;
	timely_init(&handle->timely, handle->map, rate, mask, NULL, NULL);
	lv2_atom_forge_init(&handle->forge, handle->map);

	if(!props_init(&handle->props, MAX_NPROPS, descriptor->URI, handle->map, handle))
	{
		fprintf(stderr, "failed to initialize property structure\n");
		free(handle);
		return NULL;
	}

	if(  !props_register(&handle->props, &stat_mode, &handle->state.mode, &handle->stash.mode)
		|| !props_register(&handle->props, &stat_factor, &handle->state.factor, &handle->stash.factor) )
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
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	uint32_t last_t = 0;

	const uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	handle->ref = lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;

		if(timely_advance(&handle->timely, obj, last_t, ev->time.frames))
		{
			if(handle->ref)
				handle->ref = _position_atomize(handle, &handle->forge, ev->time.frames);
		}
		else
		{
			props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref);
		}

		last_t = ev->time.frames;
	}

	timely_advance(&handle->timely, NULL, last_t, nsamples);

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

	return props_save(&handle->props, &handle->forge, store, state, flags, features);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

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

const LV2_Descriptor orbit_subspace = {
	.URI						= ORBIT_SUBSPACE_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= NULL,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
