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

typedef struct _plughandle_t plughandle_t;

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

	LV2_Atom_Forge forge;
	position_t pos;

	double frames_per_bar;
	double frames_per_beat;
	double rel;

	LV2_Atom_Sequence *event_out;
	const float *beat_unit;
	const float *beats_per_bar;
	const float *beats_per_minute;
	const float *rolling;
	const float *rewind;

	int beat_unit_i;
	int beats_per_bar_i;
	int beats_per_minute_i;
	bool rolling_i;
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

	return handle;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void *data)
{
	plughandle_t *handle = (plughandle_t *)instance;

	switch(port)
	{
		case 0:
			handle->event_out = (LV2_Atom_Sequence *)data;
			break;
		case 1:
			handle->beat_unit = (const float *)data;
			break;
		case 2:
			handle->beats_per_bar = (const float *)data;
			break;
		case 3:
			handle->beats_per_minute = (const float *)data;
			break;
		case 4:
			handle->rolling = (const float *)data;
			break;
		case 5:
			handle->rewind = (const float *)data;
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

	handle->rel = 0.f;

	pos->frame = 0;
	pos->bar = 0;
	pos->bar_beat = 0.f;

	pos->beat_unit = 4;
	pos->beats_per_bar = 4.f;
	pos->beats_per_minute = 120.0f;

	handle->frames_per_beat = 240.f / (pos->beats_per_minute * pos->beat_unit) * pos->frames_per_second;
	handle->frames_per_bar = handle->frames_per_beat * pos->beats_per_bar;
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

	int beat_unit_i = floor(*handle->beat_unit);
	int beats_per_bar_i = floor(*handle->beats_per_bar);
	int beats_per_minute_i = floor(*handle->beats_per_minute);
	bool rolling_i = floor(*handle->rolling);
	bool rewind_i = floor(*handle->rewind);

	int needs_update = 0;

	if(beat_unit_i != handle->beat_unit_i)
		needs_update = 1;
	if(beats_per_bar_i != handle->beats_per_bar_i)
		needs_update = 1;
	if(beats_per_minute_i != handle->beats_per_minute_i)
		needs_update = 1;
	if(rolling_i != handle->rolling_i)
		needs_update = 1;

	if(needs_update)
	{
		// derive position as fractional bar
		double bar_frac = handle->rel / handle->frames_per_bar;

		pos->beat_unit = beat_unit_i;
		pos->beats_per_bar = beats_per_bar_i;
		pos->beats_per_minute = beats_per_minute_i;
		pos->speed = rolling_i ? 1.f : 0.f;

		if(rolling_i && !handle->rolling_i && rewind_i) // start rolling
		{
			bar_frac = 0.f;
			pos->frame = 0; // reset frame pointer
			pos->bar = 0; // reset bar
			handle->rel = 0;
		}
		pos->bar_beat = pos->beats_per_bar * bar_frac;

		_position_atomize(handle, &handle->forge, pos);

		handle->beat_unit_i = beat_unit_i;
		handle->beats_per_bar_i = beats_per_bar_i;
		handle->beats_per_minute_i = beats_per_minute_i;
		handle->rolling_i = rolling_i;

		// update frames_per_beat and frames_per_bar
		handle->frames_per_beat = 240.f / (pos->beats_per_minute * pos->beat_unit) * pos->frames_per_second;
		handle->frames_per_bar = handle->frames_per_beat * pos->beats_per_bar;
	}

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

	lv2_atom_forge_pop(&handle->forge, &frame);
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	free(handle);
}

const LV2_Descriptor orbit_pacemaker = {
	.URI						= ORBIT_PACEMAKER_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= NULL
};
