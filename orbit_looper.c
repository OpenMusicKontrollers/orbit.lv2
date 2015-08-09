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

typedef enum _punchmode_t punchmode_t;
typedef struct _plughandle_t plughandle_t;

enum _punchmode_t {
	PUNCH_BEAT				= 0,
	PUNCH_BAR					= 1
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
	double frames_per_beat;
	double frames_per_bar;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;
	const float *punch;
	const float *width;
	const float *mute;
	float *play_capacity;
	float *rec_capacity;
	float *play_position;

	punchmode_t punch_i;
	int64_t width_i;

	int play;
	uint8_t buf [2][BUF_SIZE];
	int64_t offset;
	int64_t window;
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

	handle->pos.frames_per_second = rate;
	handle->pos.beat_unit = 4;
	handle->pos.beats_per_bar = 4.f;
	handle->pos.beats_per_minute = 120.f;

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

	handle->offset = 0;
	handle->play = 0;
}

static inline void
_play(plughandle_t *handle, int64_t ref, uint32_t capacity)
{
	const LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	while(handle->ev && !lv2_atom_sequence_is_end(&play_seq->body, play_seq->atom.size, handle->ev))
	{
		if(handle->ev->time.frames > handle->offset)
			break; // event not part of this region

		LV2_Atom_Event *e = lv2_atom_sequence_append_event(handle->event_out,
			capacity, handle->ev);
		if(e)
		{
			e->time.frames = ref;
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
_update_position(plughandle_t *handle)
{
	position_t *pos = &handle->pos;

	handle->frames_per_beat = 60.f / (pos->beats_per_minute * (pos->beat_unit / 4)) * pos->frames_per_second;
	handle->frames_per_bar = handle->frames_per_beat * pos->beats_per_bar;

	switch(handle->punch_i)
	{
		case PUNCH_BAR:
		{
			handle->window = handle->width_i * handle->frames_per_bar;

			handle->offset = 
				((pos->bar % handle->width_i) * pos->beats_per_bar + pos->bar_beat)
				* handle->frames_per_beat;

			break;
		}
		case PUNCH_BEAT:
		{
			handle->window = handle->width_i * handle->frames_per_beat;

			double integral;
			double beat_beat = modf(pos->bar_beat, &integral);
			handle->offset = (((int)integral % handle->width_i) + beat_beat)
				* handle->frames_per_beat; //TODO may overflow if width > beats_per_bar

			break;
		}
	}
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	position_t *pos = &handle->pos;
	uint32_t capacity = handle->event_out->atom.size;

	punchmode_t punch_i = floor(*handle->punch);
	bool punch_has_changed = punch_i != handle->punch_i;
	handle->punch_i = punch_i;

	int64_t width_i = floor(*handle->width);
	bool width_has_changed = width_i != handle->width_i;
	handle->width_i = width_i;

	bool mute_i = floor(*handle->mute);

	if(width_has_changed || punch_has_changed)
		_update_position(handle);

	lv2_atom_sequence_clear(handle->event_out);

	int64_t last_t = 0;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		int64_t frame = ev->time.frames;
		bool is_time_position = false;

		if(  lv2_atom_forge_is_object_type(&handle->forge, obj->atom.type)
			&& (obj->body.otype == handle->urid.time_position) )
		{
			_position_deatomize(handle, obj, pos);
			_update_position(handle);
			_reposition_rec(handle);
			_reposition_play(handle);
			is_time_position = true;
		}

		if(pos->speed > 0.f)
		{
			if(!is_time_position)
				_rec(handle, ev); // don't record time position signals

			for(int64_t i=last_t; i<frame; i++)
			{
				if(!mute_i)
					_play(handle, i, capacity);

				handle->offset += 1;
				if(handle->offset >= handle->window)
				{
					handle->play ^= 1; // swap buffers;
					handle->offset %= handle->window;
					_reposition_rec(handle);
					_reposition_play(handle);
				}
			}
		}

		last_t = frame;
	}

	// play the rest
	if(pos->speed > 0.f)
	{
		for(int64_t i=last_t; i<nsamples; i++)
		{
			if(!mute_i)
				_play(handle, i, capacity);

			handle->offset += 1;
			if(handle->offset >= handle->window)
			{
				handle->play ^= 1; // swap buffers;
				handle->offset %= handle->window;
				_reposition_rec(handle);
				_reposition_play(handle);
			}
		}
	}

	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	*handle->play_capacity = BUF_PERCENT * play_seq->atom.size;
	*handle->rec_capacity = BUF_PERCENT * rec_seq->atom.size;
	*handle->play_position = 100.f * handle->offset / handle->window;
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
