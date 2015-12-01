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

typedef struct _plughandle_t plughandle_t;

struct _plughandle_t {
	struct {
		LV2_URID midi_event;
	} urid;

	LV2_URID_Map *map;
	LV2_Atom_Forge forge;

	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	const float *bar_enabled;
	const float *beat_enabled;
	const float *bar_note;
	const float *beat_note;
	const float *bar_channel;
	const float *beat_channel;
	LV2_Atom_Sequence *event_out;

	bool bar_enabled_b;
	bool beat_enabled_b;
	uint8_t bar_note_i;
	uint8_t beat_note_i;
	uint8_t bar_channel_i;
	uint8_t beat_channel_i;
	bool bar_on;
	bool beat_on;
	
	bool rolling;
	LV2_Atom_Forge_Ref ref;
};
	
static inline void
_note(plughandle_t *handle, uint8_t frames, uint8_t cmd, uint8_t channel, uint8_t note)
{
	const uint8_t midi [3] = {cmd | channel, note, 0x7f};

	LV2_Atom_Forge_Ref ref = handle->ref;

	if(ref)
		ref = lv2_atom_forge_frame_time(&handle->forge, frames);
	if(ref)
		ref = lv2_atom_forge_atom(&handle->forge, 3, handle->urid.midi_event);
	if(ref)
		ref = lv2_atom_forge_raw(&handle->forge, midi, 3);
	if(ref)
		lv2_atom_forge_pad(&handle->forge, 3);

	handle->ref = ref;
}

static inline void
_note_on(plughandle_t *handle, int64_t frames, uint8_t channel, uint8_t note)
{
	_note(handle, frames, LV2_MIDI_MSG_NOTE_ON, channel, note);
}

static inline void
_note_off(plughandle_t *handle, int64_t frames, uint8_t channel, uint8_t note)
{
	_note(handle, frames, LV2_MIDI_MSG_NOTE_OFF, channel , note);
}

static void
_cb(timely_t *timely, int64_t frames, LV2_URID type, void *data)
{
	plughandle_t *handle = data;

	if(type == TIMELY_URI_SPEED(timely))
	{
		handle->rolling = TIMELY_SPEED(timely) > 0.f ? true : false;

		if(!handle->rolling)
		{
			if(handle->bar_on)
			{
				_note_off(handle, frames, handle->bar_channel_i, handle->bar_note_i);
				handle->bar_on = false;
			}

			if(handle->beat_on)
			{
				_note_off(handle, frames, handle->beat_channel_i, handle->beat_note_i);
				handle->beat_on = false;
			}
		}
	}
	else if(type == TIMELY_URI_BAR_BEAT(timely))
	{
		if(handle->rolling)
		{
			bool is_bar_start = fmod(TIMELY_BAR_BEAT(timely), TIMELY_BEATS_PER_BAR(timely)) == 0.f;

			if(handle->beat_on)
			{
				_note_off(handle, frames, handle->beat_channel_i, handle->beat_note_i);
				handle->beat_on = false;
			}

			if(handle->beat_enabled_b && (handle->bar_enabled_b ? !is_bar_start : true))
			{
				_note_on(handle, frames, handle->beat_channel_i, handle->beat_note_i);
				handle->beat_on = true;
			}
		}
	}
	else if(type == TIMELY_URI_BAR(timely))
	{
		if(handle->rolling)
		{
			if(handle->bar_on)
			{
				_note_off(handle, frames, handle->bar_channel_i, handle->bar_note_i);
				handle->bar_on = false;
			}

			if(handle->bar_enabled_b)
			{
				_note_on(handle, frames, handle->bar_channel_i, handle->bar_note_i);
				handle->bar_on = true;
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

	handle->urid.midi_event = handle->map->map(handle->map->handle, LV2_MIDI__MidiEvent);

	timely_mask_t mask = TIMELY_MASK_BAR_BEAT_WHOLE
		| TIMELY_MASK_BAR_WHOLE
		| TIMELY_MASK_SPEED;
	timely_init(&handle->timely, handle->map, rate, mask, _cb, handle);
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
			handle->event_in = (const LV2_Atom_Sequence *)data;
			break;
		case 1:
			handle->bar_enabled = (const float *)data;
			break;
		case 2:
			handle->beat_enabled = (const float *)data;
			break;
		case 3:
			handle->bar_note = (const float *)data;
			break;
		case 4:
			handle->beat_note = (const float *)data;
			break;
		case 5:
			handle->bar_channel = (const float *)data;
			break;
		case 6:
			handle->beat_channel = (const float *)data;
			break;
		case 7:
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

	const bool bar_enabled_b = *handle->bar_enabled != 0.f;
	const uint8_t bar_note_i = floor(*handle->bar_note);
	const uint8_t bar_channel_i = floor(*handle->bar_channel);
	if(  (bar_enabled_b != handle->bar_enabled_b)
		|| (bar_note_i != handle->bar_note_i)
		|| (bar_channel_i != handle->bar_channel_i) )
	{
		_note_off(handle, 0, handle->bar_channel_i, handle->bar_note_i);

		handle->bar_enabled_b = bar_enabled_b;
		handle->bar_note_i = bar_note_i;
		handle->bar_channel_i = bar_channel_i;
	}

	const bool beat_enabled_b = *handle->beat_enabled != 0.f;
	const uint8_t beat_note_i = floor(*handle->beat_note);
	const uint8_t beat_channel_i = floor(*handle->beat_channel);
	if(  (beat_enabled_b != handle->beat_enabled_b)
		|| (beat_note_i != handle->beat_note_i)
		|| (beat_channel_i != handle->beat_channel_i) )
	{
		 _note_off(handle, 0, handle->beat_channel_i, handle->beat_note_i);

		handle->beat_enabled_b = beat_enabled_b;
		handle->beat_note_i = beat_note_i;
		handle->beat_channel_i = beat_channel_i;
	}

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		timely_advance(&handle->timely, obj, last_t, ev->time.frames);

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

	free(handle);
}

const LV2_Descriptor orbit_beatbox = {
	.URI						= ORBIT_BEATBOX_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= NULL,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= NULL
};
