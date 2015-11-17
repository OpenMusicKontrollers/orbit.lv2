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

typedef enum _state_t state_t;
typedef struct _wave_t wave_t;
typedef struct _plughandle_t plughandle_t;

enum _state_t {
	STATE_ON,
	STATE_OFF
};

struct _wave_t {
	state_t state;        // Current play state
	bool enabled;
	uint32_t wave_len;
	uint32_t wave_offset;  // Current play offset in the wave
	float *wave;
	float *audio;
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;

	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	const float *bar_enabled;
	const float *beat_enabled;
	wave_t beat;
	wave_t bar;
	
	bool rolling;
};

static const float attack_s = 0.005;
static const float decay_s  = 0.075;

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
		double integ;
		double fract = modf(TIMELY_BAR_BEAT(timely), &integ);
		if(handle->rolling && (fract == 0.0) )
		{
			bool is_bar_start = fmod(TIMELY_BAR_BEAT(timely), TIMELY_BEATS_PER_BAR(timely)) == 0.f;

			if(handle->beat.enabled && (handle->bar.enabled ? !is_bar_start : true))
			{
				handle->beat.state = STATE_ON;
				handle->beat.wave_offset = 0;
			}
		}
	}
	else if(type == TIMELY_URI_BAR(timely))
	{
		if(handle->rolling)
		{
			if(handle->bar.enabled)
			{
				handle->bar.state = STATE_ON;
				handle->bar.wave_offset = 0;
			}
		}
	}
}

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate,
	const char *bundle_path, const LV2_Feature *const *features)
{
	plughandle_t *handle = calloc(1, sizeof(plughandle_t));
	if(!handle)
		return NULL;

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

	timely_mask_t mask = TIMELY_MASK_BAR_BEAT | TIMELY_MASK_BAR | TIMELY_MASK_SPEED;
	timely_init(&handle->timely, handle->map, rate, mask, _cb, handle);
	lv2_atom_forge_init(&handle->forge, handle->map);

	// Initialise instance fields
	const uint32_t attack_len = (uint32_t)(attack_s * rate);
	const uint32_t decay_len = (uint32_t)(decay_s * rate);

	// Generate one cycle of a sine wave at the desired frequency
	const float amp = 0.5;

	const float bar_freq = 440.f * 4.f;
	handle->bar.wave_len = attack_len + decay_len;
	handle->bar.wave = malloc(handle->bar.wave_len * sizeof(float));
	for(unsigned i=0; i<handle->bar.wave_len; i++)
	{
		handle->bar.wave[i] = sin(i * 2.f * M_PI * bar_freq / rate) * amp;
		if(i < attack_len)
			handle->bar.wave[i] *= (float)i / attack_len;
		else // >= attack_len
			handle->bar.wave[i] *= (float)(handle->bar.wave_len - i) / decay_len;
	}

	const float beat_freq = 440.0 * 2.f;
	handle->beat.wave_len = attack_len + decay_len;
	handle->beat.wave = malloc(handle->beat.wave_len * sizeof(float));
	for(unsigned i=0; i<handle->beat.wave_len; i++)
	{
		handle->beat.wave[i] = sin(i * 2.f * M_PI * beat_freq / rate) * amp;
		if(i < attack_len)
			handle->beat.wave[i] *= (float)i / attack_len;
		else // >= attack_len
			handle->beat.wave[i] *= (float)(handle->beat.wave_len - i) / decay_len;
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
			handle->bar_enabled= (const float *)data;
			break;
		case 2:
			handle->beat_enabled = (const float *)data;
			break;
		case 3:
			handle->bar.audio = (float *)data;
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

	handle->bar.state = STATE_OFF;
	handle->beat.state = STATE_OFF;
}

static inline void
_play(plughandle_t *handle, wave_t *wave, uint32_t begin, uint32_t end)
{
	float *const output = wave->audio;

	for(unsigned i=begin; i<end; i++)
	{
		switch(wave->state)
		{
			case STATE_ON:
			{
				if(wave->wave_offset++ < wave->wave_len)
				{
					output[i] += wave->wave[wave->wave_offset];
					break;
				}
				else
				{
					wave->state = STATE_OFF;
					// fall-through
				}
			}
			case STATE_OFF:
			{
				//output[i] += 0.f;
				break;
			}
		}
	}
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	uint32_t last_t = 0;

	handle->bar.enabled = *handle->bar_enabled != 0.f;
	handle->beat.enabled = *handle->beat_enabled != 0.f;

	// clear audio output buffer
	memset(handle->bar.audio, 0x0, sizeof(float)*nsamples);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		_play(handle, &handle->bar, last_t, ev->time.frames);
		_play(handle, &handle->beat, last_t, ev->time.frames);

		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		timely_advance(&handle->timely, obj, last_t, ev->time.frames);

		last_t = ev->time.frames;
	}

	_play(handle, &handle->bar, last_t, nsamples);
	_play(handle, &handle->beat, last_t, nsamples);
	timely_advance(&handle->timely, NULL, last_t, nsamples);
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	free(handle->bar.wave);
	free(handle->beat.wave);
	free(handle);
}

const LV2_Descriptor orbit_click = {
	.URI						= ORBIT_CLICK_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= NULL
};
