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

#define BUF_SIZE 0x1000000 // 16 MB
#define BUF_PERCENT (100.f / BUF_SIZE)

typedef enum _punchmode_t punchmode_t;
typedef struct _plughandle_t plughandle_t;

enum _punchmode_t {
	PUNCH_BEAT				= 0,
	PUNCH_BAR					= 1
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	
	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;
	const float *punch;
	const float *width;
	const float *mute;
	float *play_capacity;
	float *rec_capacity;
	float *play_position;

	punchmode_t punch_i;
	unsigned count_i;
	unsigned width_i;
	float window;
	int64_t offset;

	unsigned play;
	bool rolling;
	uint8_t buf [2][BUF_SIZE];
	LV2_Atom_Event *ev;
};

static inline void
_play(plughandle_t *handle, int64_t to, uint32_t capacity)
{
	const LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	const int64_t ref = handle->offset - to; // beginning of current period

	while(handle->ev && !lv2_atom_sequence_is_end(&play_seq->body, play_seq->atom.size, handle->ev))
	{
		if(handle->ev->time.frames >= handle->offset)
			break; // event not part of this region

		LV2_Atom_Event *e = lv2_atom_sequence_append_event(handle->event_out,
			capacity, handle->ev);
		if(e)
		{
			e->time.frames -= ref;
		}
		else
			break; // overflow

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
		e->time.frames = handle->offset;
	}
	else
		; // overflow
}

static inline void
_reposition_play(plughandle_t *handle)
{
	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	LV2_ATOM_SEQUENCE_FOREACH(play_seq, ev)
	{
		if(ev->time.frames >= handle->offset)
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
		if(ev->time.frames >= handle->offset)
		{
			// truncate sequence here
			rec_seq->atom.size = (uintptr_t)ev - (uintptr_t)&rec_seq->body;
			return;
		}
	}
}

static inline void
_window_refresh(plughandle_t *handle)
{
	timely_t *timely = &handle->timely;

	if(handle->punch_i == PUNCH_BEAT)
		handle->window = 100.f / (handle->width_i * TIMELY_FRAMES_PER_BEAT(timely));
	else if(handle->punch_i == PUNCH_BAR)
		handle->window = 100.f / (handle->width_i * TIMELY_FRAMES_PER_BAR(timely));
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
			+ TIMELY_BAR_BEAT(timely);

		if(handle->punch_i == PUNCH_BEAT)
			handle->offset = fmod(beats, handle->width_i) * TIMELY_FRAMES_PER_BEAT(timely);
		else if(handle->punch_i == PUNCH_BAR)
			handle->offset = fmod(beats, handle->width_i * TIMELY_BEATS_PER_BAR(timely))
				* TIMELY_FRAMES_PER_BEAT(timely);

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

	timely_mask_t mask = TIMELY_MASK_BAR_BEAT
		//| TIMELY_MASK_BAR
		| TIMELY_MASK_BEAT_UNIT
		| TIMELY_MASK_BEATS_PER_BAR
		| TIMELY_MASK_BEATS_PER_MINUTE
		| TIMELY_MASK_FRAMES_PER_SECOND
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
			handle->event_out = (LV2_Atom_Sequence *)data;
			break;
		case 2:
			handle->punch = (const float *)data;
			break;
		case 3:
			handle->width = (const float *)data;
			break;
		case 4:
			handle->mute = (const float *)data;
			break;
		case 5:
			handle->play_capacity = (float *)data;
			break;
		case 6:
			handle->rec_capacity = (float *)data;
			break;
		case 7:
			handle->play_position = (float *)data;
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
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	uint32_t capacity = handle->event_out->atom.size;

	punchmode_t punch_i = floor(*handle->punch);
	bool punch_has_changed = punch_i != handle->punch_i;
	handle->punch_i = punch_i;

	int64_t width_i = floor(*handle->width);
	bool width_has_changed = width_i != handle->width_i;
	handle->width_i = width_i;

	if(punch_has_changed || width_has_changed)
		_window_refresh(handle);

	bool mute_i = floor(*handle->mute);

	lv2_atom_sequence_clear(handle->event_out);

	int64_t last_t = 0;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		if(handle->rolling)
			handle->offset += ev->time.frames - last_t;

		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		const int time_event = timely_advance(&handle->timely, obj, last_t, ev->time.frames);

		if(time_event && handle->rolling)
			_rec(handle, ev); // dont' record time position signals
	
		if(!mute_i && handle->rolling)
			_play(handle, ev->time.frames, capacity);

		last_t = ev->time.frames;
	}

	if(handle->rolling)
		handle->offset += nsamples - last_t;
	timely_advance(&handle->timely, NULL, last_t, nsamples);
	if(!mute_i && handle->rolling)
		_play(handle, nsamples, capacity);

	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	*handle->play_capacity = BUF_PERCENT * play_seq->atom.size;
	*handle->rec_capacity = BUF_PERCENT * rec_seq->atom.size;
	*handle->play_position = handle->offset * handle->window;
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	free(handle);
}

const LV2_Descriptor orbit_looper = {
	.URI						= ORBIT_LOOPER_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= NULL
};
