/*
 * Copyright (c) 2015-2016 Hanspeter Portner (dev@open-music-kontrollers.ch)
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
#include <varchunk.h>

typedef struct _plughandle_t plughandle_t;

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	varchunk_t *rb;
	bool rolling;
};

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
		const double bar_beat = TIMELY_BAR_BEAT(timely);

		size_t size;
		const LV2_Atom_Event *ev;
		while( (ev = varchunk_read_request(handle->rb, &size)) )
		{
			if(ev->time.frames > frames)
				break;

			const LV2_Atom *atom = &ev->body;

			if(handle->ref)
				handle->ref = lv2_atom_forge_frame_time(&handle->forge, frames);
			if(handle->ref)
				handle->ref = lv2_atom_forge_write(&handle->forge, atom, lv2_atom_total_size(atom));

			varchunk_read_advance(handle->rb);
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
	mlock(handle, sizeof(plughandle_t));

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

	timely_mask_t mask = TIMELY_MASK_SPEED
		| TIMELY_MASK_BAR_BEAT_WHOLE;
	timely_init(&handle->timely, handle->map, rate, mask, _cb, handle);
	lv2_atom_forge_init(&handle->forge, handle->map);

	handle->rb = varchunk_new(0x10000, false); // 64K
	if(!handle->rb)
	{
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
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	uint32_t last_t = 0;

	const uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	handle->ref = lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;

		if(!timely_advance(&handle->timely, obj, last_t, ev->time.frames))
		{
			if(handle->rolling)
			{
				const size_t ev_size= sizeof(LV2_Atom_Event) + ev->body.size;
				LV2_Atom_Event *dst;
				if( (dst = varchunk_write_request(handle->rb, ev_size)) )
				{
					memcpy(dst, ev, ev_size);
					varchunk_write_advance(handle->rb, ev_size);
				}
			}
			else // !rolling, aka through mode
			{
				if(handle->ref)
					handle->ref = lv2_atom_forge_frame_time(&handle->forge, ev->time.frames);
				if(handle->ref)
					handle->ref = lv2_atom_forge_write(&handle->forge, &obj->atom, lv2_atom_total_size(&obj->atom));
			}
		}

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

	varchunk_free(handle->rb);
	munlock(handle, sizeof(plughandle_t));
	free(handle);
}

const LV2_Descriptor orbit_quantum = {
	.URI						= ORBIT_QUANTUM_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= NULL,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= NULL
};
