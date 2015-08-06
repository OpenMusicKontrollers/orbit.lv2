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

#define BUF_SIZE 0x1000000 // 16 MB
#define BUF_PERCENT (100.f / BUF_SIZE)

typedef enum _plugmode_t plugmode_t;
typedef enum _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

enum _plugmode_t {
	MODE_PLAY				= 0,
	MODE_RECORD			= 1,
	MODE_REPLACE		= 2,
	MODE_SUBSTITUTE	= 3
};

enum _plugstate_t {
	STATE_PAUSED		= 0,
	STATE_ROLLING		= 1
};

struct _plughandle_t {
	LV2_URID_Map *map;

	struct {
		LV2_URID time_position;
		LV2_URID time_barBeat;
		LV2_URID time_bar;
		LV2_URID time_beat;
		LV2_URID time_beatUnit;
		LV2_URID time_beatsPerBar;
		LV2_URID time_beatsPerMinute;
		LV2_URID time_frame;
		LV2_URID time_framesPerSecond;
		LV2_URID time_speed;
	} urid;

	LV2_Atom_Forge forge;

	position_t pos;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;
	const float *mode;
	const float *state;
	float *capacity [2];
	float *position;
	
	plugmode_t mode_i;
	plugstate_t state_i;
	uint32_t cap_i [2];

	int play;
	int64_t last_i;
	uint8_t buf [2][BUF_SIZE];
	int64_t offset;
	LV2_Atom_Event *ev;
};

static inline void
_position_deatomize(plughandle_t *handle, const LV2_Atom_Object *obj, position_t *pos)
{
	const LV2_Atom* name = NULL;
	const LV2_Atom* age  = NULL;

	const LV2_Atom_Float *bar_beat = NULL;
	const LV2_Atom_Long *bar = NULL;
	const LV2_Atom_Double *beat = NULL;
	const LV2_Atom_Int *beat_unit = NULL;
	const LV2_Atom_Float *beats_per_bar = NULL;
	const LV2_Atom_Float *beats_per_minute = NULL;
	const LV2_Atom_Long *frame = NULL;
	const LV2_Atom_Float *frames_per_second = NULL;
	const LV2_Atom_Float *speed = NULL;

	LV2_Atom_Object_Query q [] = {
		{ handle->urid.time_barBeat, (const LV2_Atom **)&bar_beat },
		{ handle->urid.time_bar, (const LV2_Atom **)&bar },
		{ handle->urid.time_beat, (const LV2_Atom **)&beat },
		{ handle->urid.time_beatUnit, (const LV2_Atom **)&beat_unit },
		{ handle->urid.time_beatsPerBar, (const LV2_Atom **)&beats_per_bar },
		{ handle->urid.time_beatsPerMinute, (const LV2_Atom **)&beats_per_minute },
		{ handle->urid.time_frame, (const LV2_Atom **)&frame },
		{ handle->urid.time_framesPerSecond, (const LV2_Atom **)&frames_per_second },
		{ handle->urid.time_speed, (const LV2_Atom **)&speed },
		LV2_ATOM_OBJECT_QUERY_END
	};

	lv2_atom_object_query(obj, q);

	if(bar_beat)
		pos->bar_beat = bar_beat->body;
	if(bar)
		pos->bar = bar->body;
	if(beat)
		pos->beat = beat->body;
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
	handle->urid.time_beat = handle->map->map(handle->map->handle,
		LV2_TIME__beat);
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
			handle->event_in = (const LV2_Atom_Sequence *)data;
			break;
		case 1:
			handle->event_out = (LV2_Atom_Sequence *)data;
			break;
		case 2:
			handle->mode = (const float *)data;
			break;
		case 3:
			handle->state = (const float *)data;
			break;
		case 4:
			handle->capacity[0] = (float *)data;
			break;
		case 5:
			handle->capacity[1] = (float *)data;
			break;
		case 6:
			handle->position = (float *)data;
			break;
		default:
			break;
	}
}

static void
activate(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	handle->mode_i = MODE_PLAY;
	handle->state_i = STATE_PAUSED;
	handle->offset = 0;
	handle->play = 0;
}

static inline void
_play(plughandle_t *handle, uint32_t capacity, uint32_t nsamples)
{
	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	while(!lv2_atom_sequence_is_end(&play_seq->body, play_seq->atom.size, handle->ev))
	{
		if(handle->ev->time.frames >= handle->offset + nsamples)
			break; // event not part of this period

		LV2_Atom_Event *e = lv2_atom_sequence_append_event(handle->event_out,
			capacity, handle->ev);
		if(e)
		{
			e->time.frames -= handle->offset;
			if(e->time.frames < 0)
				e->time.frames = 0; // underflow
		}
		else
			break; // overflow

		handle->ev = lv2_atom_sequence_next(handle->ev);
	}
}

static inline void
_rec(plughandle_t *handle, int substitute)
{
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		if(  (obj->atom.type == handle->forge.Object)
			&& (obj->body.otype == handle->urid.time_position) )
		{
			_position_deatomize(handle, obj, &handle->pos);
		}

		LV2_Atom_Event *e = lv2_atom_sequence_append_event(rec_seq, BUF_SIZE, ev);
		if(e)
		{
			e->time.frames += handle->offset;
			if(!substitute)
				handle->last_i = e->time.frames;
		}
		else
			break; // overflow
	}
}

static inline void
_skip(plughandle_t *handle)
{
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		if(  (obj->atom.type == handle->forge.Object)
			&& (obj->body.otype == handle->urid.time_position) )
		{
			_position_deatomize(handle, obj, &handle->pos);
		}
	}
}

static inline void
_rewind_play(plughandle_t *handle)
{
	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	// rewind
	handle->ev = lv2_atom_sequence_begin(&play_seq->body);

	handle->offset = 0;
}

static inline void
_rewind_rec(plughandle_t *handle, int substitute)
{
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	// rewind
	rec_seq->atom.type = handle->forge.Sequence;
	rec_seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
	rec_seq->body.unit = 0; // frames
	rec_seq->body.pad = 0;

	handle->offset = 0;
	if(!substitute)
		handle->last_i = 0;
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	plugmode_t mode_i = floor(*handle->mode);
	plugstate_t state_i = floor(*handle->state);

	//int mode_has_changed = mode_i != handle->mode_i;
	int state_has_changed = state_i != handle->state_i;

	uint32_t capacity = handle->event_out->atom.size;
	lv2_atom_sequence_clear(handle->event_out);

	if(state_i == STATE_ROLLING)
	{
		if(state_has_changed)
		{
			handle->play ^= 1; // swap buffers
			play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
			rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

			_rewind_play(handle);
			_rewind_rec(handle, mode_i == MODE_SUBSTITUTE);
			
			handle->mode_i = mode_i; // mode is only read upon starting to roll
		}

		switch(handle->mode_i)
		{
			case MODE_PLAY: // only playback, no recording
			{
				_play(handle, capacity, nsamples);
				_skip(handle);

				// automatic repeat
				if(lv2_atom_sequence_is_end(&play_seq->body, play_seq->atom.size, handle->ev))
					_rewind_play(handle);

				break;
			}
			case MODE_RECORD: // no playback, only recording
			{
				_rec(handle, 0);

				break;
			}
			case MODE_REPLACE: // playback and recording
			{
				_play(handle, capacity, nsamples);
				_rec(handle, 0);

				break;
			}
			case MODE_SUBSTITUTE: // playback and recording for given region
			{
				_play(handle, capacity, nsamples);
				_rec(handle, 1);

				// automatic repeat
				if(lv2_atom_sequence_is_end(&play_seq->body, play_seq->atom.size, handle->ev)
					&& (handle->offset >= handle->last_i) )
				{
					handle->play ^= 1; // switch buffers
					play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
					rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

					_rewind_play(handle);
					_rewind_rec(handle, 1);
				}

				break;
			}
		}
	}

	uint32_t cap_i = sizeof(LV2_Atom) + play_seq->atom.size;
	if(cap_i != handle->cap_i[0])
	{
		*handle->capacity[0] = BUF_PERCENT * play_seq->atom.size;
		handle->cap_i[0] = cap_i;
	}
	cap_i = sizeof(LV2_Atom) + rec_seq->atom.size;
	if(cap_i != handle->cap_i[1])
	{
		*handle->capacity[1] = BUF_PERCENT * rec_seq->atom.size;
		handle->cap_i[1] = cap_i;
	}

	handle->mode_i = mode_i;
	handle->state_i = state_i;
	handle->offset += nsamples;

	if(state_i == STATE_ROLLING)
	{
		if(handle->offset < handle->last_i)
			*handle->position = 100.f * handle->offset / handle->last_i;
		else
			*handle->position = 100.f;
	}
	else
		*handle->position = 0.f;
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

const LV2_Descriptor orbit_looper = {
	.URI						= ORBIT_LOOPER_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= deactivate,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
