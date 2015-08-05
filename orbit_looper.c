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

typedef enum _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

enum _plugstate_t {
	STATE_PLAY					= 0,
	STATE_PAUSE					= 1,
	STATE_RECORD				= 2
};

struct _plughandle_t {
	LV2_URID_Map *map;

	LV2_Atom_Forge forge;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;
	const float *state;
	float *capacity;
	float *position;

	plugstate_t state_i;
	uint32_t cap_i;
	int64_t last_i;
	uint8_t buf [BUF_SIZE];
	int64_t offset;
	LV2_Atom_Event *ev;
};

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
			handle->state = (const float *)data;
			break;
		case 3:
			handle->capacity = (float *)data;
			break;
		case 4:
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

	handle->state_i = STATE_PAUSE;
	handle->offset = 0;
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	LV2_Atom_Sequence *seq = (LV2_Atom_Sequence *)handle->buf;

	plugstate_t state_i = floor(*handle->state);

	uint32_t capacity = handle->event_out->atom.size;
	lv2_atom_sequence_clear(handle->event_out);

	switch(state_i)
	{
		case STATE_PAUSE:
		{
			// do nothing

			break;
		}
		case STATE_PLAY:
		{

			if(state_i != handle->state_i) // state has changed since last period
			{
				// rewind
				handle->ev = lv2_atom_sequence_begin(&seq->body);

				handle->offset = 0;
			}
		
			// repeat
			if(lv2_atom_sequence_is_end(&seq->body, seq->atom.size, handle->ev))
			{
				// rewind
				handle->ev = lv2_atom_sequence_begin(&seq->body);

				handle->offset = 0;
			}

			while(!lv2_atom_sequence_is_end(&seq->body, seq->atom.size, handle->ev))
			{
				if(handle->ev->time.frames >= handle->offset + nsamples)
					break; // event not part of this period

				LV2_Atom_Event *e = lv2_atom_sequence_append_event(handle->event_out,
					capacity, handle->ev);
				e->time.frames -= handle->offset;

				handle->ev = lv2_atom_sequence_next(handle->ev);
			}

			break;
		}
		case STATE_RECORD:
		{
			if(state_i != handle->state_i) // state has changed since last period
			{
				// rewind
				seq->atom.type = handle->forge.Sequence;
				seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
				seq->body.unit = 0; // frames
				seq->body.pad = 0;

				handle->offset = 0;
				handle->last_i = 0;
			}

			LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
			{
				LV2_Atom_Event *e = lv2_atom_sequence_append_event(seq, BUF_SIZE, ev);
				e->time.frames += handle->offset;
				handle->last_i = e->time.frames;
			}

			break;
		}
	}

	uint32_t cap_i = sizeof(LV2_Atom) + seq->atom.size;
	if(cap_i != handle->cap_i)
	{
		*handle->capacity = BUF_PERCENT * seq->atom.size;
		handle->cap_i = cap_i;
	}

	handle->state_i = state_i;
	handle->offset += nsamples;

	if(state_i == STATE_PLAY)
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
