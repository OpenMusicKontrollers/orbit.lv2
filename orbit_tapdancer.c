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

#include <lv2_osc.h>

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
	} urid;

	osc_forge_t oforge;

	LV2_Atom_Forge forge;
	position_t pos;

	int64_t last_frame;
	int64_t frames_per_beat;
	bool tapped;

	double bpm0;
	double bpm1;
	double bpm00;
	double bpm11;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;
	const float *beat_unit;
	const float *beats_per_bar;
	const float *stiffness;
	float *beats_per_minute;
};

static inline void
_position_atomize(plughandle_t *handle, LV2_Atom_Forge *forge, position_t *pos)
{
	LV2_Atom_Forge_Ref ref;
	LV2_Atom_Forge_Frame frame;

	lv2_atom_forge_frame_time(forge, 0);
	lv2_atom_forge_object(forge, &frame, 0, handle->urid.time_position);

	lv2_atom_forge_key(forge, handle->urid.time_barBeat);
	lv2_atom_forge_float(forge, pos->bar_beat);

	lv2_atom_forge_key(forge, handle->urid.time_bar);
	lv2_atom_forge_long(forge, pos->bar);

	lv2_atom_forge_key(forge, handle->urid.time_beatUnit);
	lv2_atom_forge_int(forge, pos->beat_unit);

	lv2_atom_forge_key(forge, handle->urid.time_beatsPerBar);
	lv2_atom_forge_float(forge, pos->beats_per_bar);

	lv2_atom_forge_key(forge, handle->urid.time_beatsPerMinute);
	lv2_atom_forge_float(forge, pos->beats_per_minute);

	lv2_atom_forge_key(forge, handle->urid.time_frame);
	lv2_atom_forge_long(forge, pos->frame);

	lv2_atom_forge_key(forge, handle->urid.time_framesPerSecond);
	lv2_atom_forge_float(forge, pos->frames_per_second);

	lv2_atom_forge_key(forge, handle->urid.time_speed);
	lv2_atom_forge_float(forge, pos->speed);

	lv2_atom_forge_pop(forge, &frame);
}

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate,
	const char *bundle_path, const LV2_Feature *const *features)
{
	int i;
	plughandle_t *handle = calloc(1, sizeof(plughandle_t));
	if(!handle)
		return NULL;

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
	osc_forge_init(&handle->oforge, handle->map);

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
		case 2:
			handle->beat_unit = (const float *)data;
			break;
		case 3:
			handle->beats_per_bar = (const float *)data;
			break;
		case 4:
			handle->stiffness = (const float *)data;
			break;
		case 5:
			handle->beats_per_minute = (float *)data;
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

typedef struct _evs_t evs_t;
struct _evs_t {
	plughandle_t *handle;
	const LV2_Atom_Event *ev;
};

static void
_message(const char *path, const char *fmt, const LV2_Atom_Tuple *tup, void *data)
{
	const evs_t *evs = data;
	plughandle_t *handle = evs->handle;
	position_t *pos = &handle->pos;
	const LV2_Atom_Event *ev = evs->ev;

	if(!strcmp(path, "/tap"))
	{
		const int64_t cur_frame = pos->frame + ev->time.frames;
		handle->frames_per_beat = cur_frame - handle->last_frame;
		handle->last_frame = cur_frame;

		handle->bpm1 = 240.0 / handle->frames_per_beat * pos->frames_per_second
			/ pos->beat_unit;

		pos->bar_beat += 1.0;
		handle->tapped = true;
	}
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	position_t *pos = &handle->pos;

	uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	handle->tapped = false;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *atom = (const LV2_Atom_Object *)&ev->body;

		const evs_t evs = {
			.handle = handle,
			.ev = ev
		};
		osc_atom_event_unroll(&handle->oforge, atom, NULL, NULL, _message, (void *)&evs);
	}

	const double Ds = 1.0 / *handle->stiffness;
	handle->bpm11 = Ds * (handle->bpm0 + handle->bpm1) / 2 + handle->bpm00 * (1.0 - Ds);
	handle->bpm0 = handle->bpm1;
	handle->bpm00 = handle->bpm11;

	bool bpm_has_changed = fabs(pos->beats_per_minute - handle->bpm11) >= 1.0;
	bool beat_unit_changed = pos->beat_unit != *handle->beat_unit;
	bool beats_per_bar_changed = pos->beats_per_bar !=  *handle->beats_per_bar;

	if(handle->tapped || bpm_has_changed || beat_unit_changed || beats_per_bar_changed)
	{
		//printf("things have changed\n");
		pos->beats_per_minute = round(handle->bpm11);
		pos->beat_unit = *handle->beat_unit;
		pos->beats_per_bar = *handle->beats_per_bar;
		pos->speed = pos->beats_per_minute > 0 ? 1.f : 0.f;
		while(pos->bar_beat >= pos->beats_per_bar)
		{
			pos->bar_beat -= pos->beats_per_bar;
			pos->bar += 1;
		}
		_position_atomize(handle, &handle->forge, pos);
	}

	*handle->beats_per_minute = pos->beats_per_minute;

	pos->frame += nsamples;
	
	lv2_atom_forge_pop(&handle->forge, &frame);
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	free(handle);
}

const LV2_Descriptor orbit_tapdancer = {
	.URI						= ORBIT_TAPDANCER_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= NULL
};
