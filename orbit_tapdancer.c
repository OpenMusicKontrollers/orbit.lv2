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

#include <props.h>

#define MAX_NPROPS 5

typedef struct _position_t position_t;
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

struct _plughandle_t {
	LV2_URID_Map *map;

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

		LV2_URID beats_per_minute;
	} urid;

	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;
	position_t pos;

	int64_t last_frame;
	int64_t frames_per_beat;

	double bpm0;
	double bpm1;
	double bpm00;
	double bpm11;
	double Ds;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	int32_t beat_unit;
	int32_t beats_per_bar;
	int32_t stiffness;
	int32_t tap;
	int32_t beats_per_minute;

	PROPS_T(props, MAX_NPROPS);
};

static const props_def_t stat_beat_unit = {
	.property = ORBIT_URI"#tapdancer_beat_unit",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_beats_per_bar = {
	.property = ORBIT_URI"#tapdancer_beats_per_bar",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_filter_stiffness = {
	.property = ORBIT_URI"#tapdancer_filter_stiffness",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_tap = {
	.property = ORBIT_URI"#tapdancer_tap",
	.access = LV2_PATCH__writable,
	.type = LV2_ATOM__Bool,
	.mode = PROP_MODE_STATIC
};

static const props_def_t stat_beats_per_minute = {
	.property = ORBIT_URI"#tapdancer_beats_per_minute",
	.access = LV2_PATCH__readable,
	.type = LV2_ATOM__Int,
	.mode = PROP_MODE_STATIC
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

static inline LV2_Atom_Forge_Ref
_refresh(plughandle_t *handle, int64_t frames)
{
	position_t *pos = &handle->pos;

	//printf("things have changed\n");
	pos->beats_per_minute = round(handle->bpm11);
	pos->speed = pos->beats_per_minute > 0 ? 1.f : 0.f;

	while(pos->bar_beat >= pos->beats_per_bar)
	{
		pos->bar_beat -= pos->beats_per_bar;
		pos->bar += 1;
	}

	return _position_atomize(handle, &handle->forge, frames, pos);
}

static void
_intercept_one(void *data, LV2_Atom_Forge *forge, int64_t frames,
	props_event_t event, props_impl_t *impl)
{
	plughandle_t *handle = data;
	position_t *pos = &handle->pos;

	pos->beat_unit = handle->beat_unit;
	pos->beats_per_bar = handle->beats_per_bar;

	if(handle->ref)
		handle->ref = _refresh(handle, frames);
}

static void
_intercept_stiffness(void *data, LV2_Atom_Forge *forge, int64_t frames,
	props_event_t event, props_impl_t *impl)
{
	plughandle_t *handle = data;

	handle->Ds = 1.0 / handle->stiffness;
}

static void
_intercept_tap(void *data, LV2_Atom_Forge *forge, int64_t frames,
	props_event_t event, props_impl_t *impl)
{
	plughandle_t *handle = data;
	position_t *pos = &handle->pos;

	if(handle->tap) // rising edge
	{
		const int64_t cur_frame = pos->frame + frames;
		handle->frames_per_beat = cur_frame - handle->last_frame;
		handle->last_frame = cur_frame;

		handle->bpm1 = 240.0 / handle->frames_per_beat * pos->frames_per_second
			/ pos->beat_unit;

		pos->bar_beat += 1.0;

		/*
		if(handle->ref)
			handle->ref = _refresh(handle, frames);
		*/
	}
}

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
	}

	if(!handle->map)
	{
		fprintf(stderr,
			"%s: Host does not support urid:map\n", descriptor->URI);
		free(handle);
		return NULL;
	}

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

	if(!props_init(&handle->props, MAX_NPROPS, descriptor->URI, handle->map, handle))
	{
		fprintf(stderr, "failed to initialize property structure\n");
		free(handle);
		return NULL;
	}

	if(  props_register(&handle->props, &stat_beat_unit, PROP_EVENT_WRITE, _intercept_one, &handle->beat_unit)
		&& props_register(&handle->props, &stat_beats_per_bar, PROP_EVENT_WRITE, _intercept_one, &handle->beats_per_bar)
		&& props_register(&handle->props, &stat_filter_stiffness, PROP_EVENT_WRITE, _intercept_stiffness, &handle->stiffness)
		&& props_register(&handle->props, &stat_tap, PROP_EVENT_WRITE, _intercept_tap, &handle->tap)
		&& (handle->urid.beats_per_minute = props_register(&handle->props, &stat_beats_per_minute, PROP_EVENT_NONE, NULL, &handle->beats_per_minute)) )
	{
		props_sort(&handle->props);
	}
	else
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
	position_t *pos = &handle->pos;

	pos->beat_unit = 4;
	pos->beats_per_bar = 4;

	handle->last_frame = 0;
	handle->bpm0 = 0.0;
	handle->bpm1 = 0.0;
	handle->bpm00 = 0.0;
	handle->bpm11 = 0.0;
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	position_t *pos = &handle->pos;

	uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	handle->ref = lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;

		props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref);
	}

	handle->bpm11 = handle->Ds * (handle->bpm0 + handle->bpm1) / 2 + handle->bpm00 * (1.0 - handle->Ds);
	handle->bpm0 = handle->bpm1;
	handle->bpm00 = handle->bpm11;

	bool bpm_has_changed = fabs(pos->beats_per_minute - handle->bpm11) >= 1.0;

	if(bpm_has_changed)
	{
		if(handle->ref)
			handle->ref = _refresh(handle, nsamples - 1);

		handle->beats_per_minute = pos->beats_per_minute;
		if(handle->ref)
			handle->ref = props_set(&handle->props, &handle->forge, nsamples-1, handle->urid.beats_per_minute);
	}

	pos->frame += nsamples;

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

	return props_save(&handle->props, &handle->forge, store, state, flags, features);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = instance;

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

const LV2_Descriptor orbit_tapdancer = {
	.URI						= ORBIT_TAPDANCER_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
