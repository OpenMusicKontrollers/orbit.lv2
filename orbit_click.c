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

typedef enum _state_t state_t;
typedef struct _wave_t wave_t;
typedef struct _plughandle_t plughandle_t;

enum _state_t {
	STATE_ATTACK,
	STATE_DECAY,
	STATE_OFF
};

struct _wave_t {
	bool is_bar;
	uint32_t wave_offset;  // Current play offset in the wave
	state_t state;        // Current play state
	float *wave;
	uint32_t wave_len;
	uint32_t elapsed_len;  // Frames since the start of the last click
	float *audio;
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

	LV2_Atom_Forge forge;

	position_t pos;
	double frames_per_beat;
	double frames_per_bar;

	const LV2_Atom_Sequence *event_in;

	wave_t beat;
	wave_t bar;

	uint32_t attack_len;
	uint32_t decay_len;
};

static const double attack_s = 0.005;
static const double decay_s  = 0.075;

static inline void
_position_deatomize(plughandle_t *handle, const LV2_Atom_Object *obj, position_t *pos)
{
	const LV2_Atom* name = NULL;
	const LV2_Atom* age  = NULL;

	const LV2_Atom_Float *bar_beat = NULL;
	const LV2_Atom_Long *bar = NULL;
	const LV2_Atom_Int *beat_unit = NULL;
	const LV2_Atom_Float *beats_per_bar = NULL;
	const LV2_Atom_Float *beats_per_minute = NULL;
	const LV2_Atom_Long *frame = NULL;
	const LV2_Atom_Float *frames_per_second = NULL;
	const LV2_Atom_Float *speed = NULL;

	LV2_Atom_Object_Query q [] = {
		{ handle->urid.time_barBeat, (const LV2_Atom **)&bar_beat },
		{ handle->urid.time_bar, (const LV2_Atom **)&bar },
		{ handle->urid.time_beatUnit, (const LV2_Atom **)&beat_unit },
		{ handle->urid.time_beatsPerBar, (const LV2_Atom **)&beats_per_bar },
		{ handle->urid.time_beatsPerMinute, (const LV2_Atom **)&beats_per_minute },
		{ handle->urid.time_frame, (const LV2_Atom **)&frame },
		{ handle->urid.time_framesPerSecond, (const LV2_Atom **)&frames_per_second },
		{ handle->urid.time_speed, (const LV2_Atom **)&speed },
		LV2_ATOM_OBJECT_QUERY_END
	};

	lv2_atom_object_query(obj, q);

	if(beat_unit)
		pos->beat_unit = beat_unit->body;
	if(beats_per_bar)
		pos->beats_per_bar = beats_per_bar->body;
	if(beats_per_minute)
		pos->beats_per_minute = beats_per_minute->body;
	if(frame)
		pos->frame = frame->body;
	if(frames_per_second)
		pos->frames_per_second = frames_per_second->body;
	if(speed)
		pos->speed = speed->body;

	if(bar_beat)
		pos->bar_beat = bar_beat->body;
	else // calculate
		pos->bar_beat = 0.f;

	if(bar)
		pos->bar = bar->body;
	else // calculate
		pos->bar = 0;
}

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate,
	const char *bundle_path, const LV2_Feature *const *features)
{
	int i;
	plughandle_t *handle = calloc(1, sizeof(plughandle_t));
	if(!handle)
		return NULL;

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

	// Initialise instance fields
	position_t *pos = &handle->pos;
	pos->frames_per_second = rate;
	pos->beat_unit = 4;
	pos->beats_per_bar = 4.f;
	pos->beats_per_minute = 120.0f;
	handle->frames_per_beat = 60.f / (pos->beats_per_minute * (pos->beat_unit / 4) ) * pos->frames_per_second;
	handle->frames_per_bar = handle->frames_per_beat * pos->beats_per_bar;

	handle->attack_len = (uint32_t)(attack_s * rate);
	handle->decay_len = (uint32_t)(decay_s * rate);

	// Generate one cycle of a sine wave at the desired frequency
	const double amp = 0.5;

	const double bar_freq = 440.f * 4.f;
	handle->bar.is_bar = true;
	handle->bar.state = STATE_OFF;
	handle->bar.wave_len = rate / bar_freq;
	handle->bar.wave = malloc(handle->bar.wave_len * sizeof(float));
	for(uint32_t i=0; i<handle->bar.wave_len; i++)
		handle->bar.wave[i] = sin(i * 2.f * M_PI * bar_freq / rate) * amp;

	const double beat_freq = 440.0 * 2.f;
	handle->beat.is_bar = false;
	handle->beat.state = STATE_OFF;
	handle->beat.wave_len = rate / beat_freq;
	handle->beat.wave = malloc(handle->beat.wave_len * sizeof(float));
	for(uint32_t i=0; i<handle->beat.wave_len; i++)
		handle->beat.wave[i] = sin(i * 2.f * M_PI * beat_freq / rate) * amp;

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
			handle->bar.audio = (float *)data;
			break;
		case 2:
			handle->beat.audio = (float *)data;
			break;
		default:
			break;
	}
}

static void
activate(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	handle->bar.elapsed_len = 0;
	handle->bar.wave_offset = 0;
	handle->bar.state = STATE_OFF;

	handle->beat.elapsed_len = 0;
	handle->beat.wave_offset = 0;
	handle->beat.state = STATE_OFF;
}

static inline void
_clear(plughandle_t *handle, wave_t *wave, uint32_t begin, uint32_t end)
{
	float *const output = wave->audio;

	memset(output, 0x0, (end - begin) * sizeof(float));
}

static inline void
_play(plughandle_t *handle, wave_t *wave, uint32_t begin, uint32_t end)
{
	float *const output = wave->audio;
	position_t *pos = &handle->pos;

	if(pos->speed == 0.f)
		return;

	for(uint32_t i=begin; i<end; i++)
	{
		switch(wave->state)
		{
			case STATE_ATTACK:
			{
				// Amplitude increases from 0..1 until attack_len
				output[i] += wave->wave[wave->wave_offset] *
					wave->elapsed_len / (float)handle->attack_len;
				if(wave->elapsed_len >= handle->attack_len)
					wave->state = STATE_DECAY;

				break;
			}
			case STATE_DECAY:
			{
				// Amplitude decreases from 1..0 until attack_len + decay_len
				output[i] += wave->wave[wave->wave_offset] *
					(1 - ((wave->elapsed_len - handle->attack_len) / (float)handle->decay_len));
				if(wave->elapsed_len >= handle->attack_len + handle->decay_len)
					wave->state = STATE_OFF;

				break;
			}
			case STATE_OFF:
			{
				break;
			}
		}

		// We continuously play the sine wave regardless of envelope
		wave->wave_offset = (wave->wave_offset + 1) % wave->wave_len;

		// Update elapsed time and start attack if necessary
		const uint32_t boundary = wave->is_bar
			? handle->frames_per_bar
			: handle->frames_per_beat;

		if(++wave->elapsed_len == boundary)
		{
			wave->state = STATE_ATTACK;
			wave->elapsed_len = 0;
		}
	}
}

static inline void
_update_position(plughandle_t *handle, wave_t *wave, const LV2_Atom_Object *obj)
{
	position_t *pos = &handle->pos;
	double integral;

	_position_deatomize(handle, obj, pos);

	handle->frames_per_beat = 60.f / (pos->beats_per_minute * (pos->beat_unit / 4)) * pos->frames_per_second;
	handle->frames_per_bar = handle->frames_per_beat * pos->beats_per_bar;

	wave->elapsed_len = wave->is_bar
		? pos->bar_beat * handle->frames_per_beat
		: modf(pos->bar_beat, &integral) * handle->frames_per_beat;

	if(wave->elapsed_len < handle->attack_len)
		wave->state = STATE_ATTACK;
	else if(wave->elapsed_len < handle->attack_len + handle->decay_len)
		wave->state = STATE_DECAY;
	else
		wave->state = STATE_OFF;
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;

	uint32_t last_t = 0;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		_clear(handle, &handle->bar, last_t, ev->time.frames);
		_clear(handle, &handle->beat, last_t, ev->time.frames);
		_play(handle, &handle->bar, last_t, ev->time.frames);
		_play(handle, &handle->beat, last_t, ev->time.frames);

		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		if(  (obj->atom.type == handle->forge.Object)
			&& (obj->body.otype == handle->urid.time_position) )
		{
			_update_position(handle, &handle->bar, obj);
			_update_position(handle, &handle->beat, obj);
		}

		last_t = ev->time.frames;
	}

	_clear(handle, &handle->bar, last_t, nsamples);
	_clear(handle, &handle->beat, last_t, nsamples);
	_play(handle, &handle->bar, last_t, nsamples);
	_play(handle, &handle->beat, last_t, nsamples);
}

static void
deactivate(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	//TODO
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	free(handle);
}

static const void*
extension_data(const char* uri)
{
	return NULL;
}

const LV2_Descriptor orbit_click = {
	.URI						= ORBIT_CLICK_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= deactivate,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
