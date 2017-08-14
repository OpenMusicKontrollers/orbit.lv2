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
#include <props.h>
#include <varchunk.h>

#define MAX_NPROPS 1

typedef enum _quantum_mode_t quantum_mode_t;
typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

struct _plugstate_t {
	int32_t mode;
};

enum _quantum_mode_t {
	MODE_FLOOR = 0,
	MODE_ROUND = 1,
	MODE_CEIL  = 2
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Ref ref;

	LV2_Log_Log *log;
	LV2_Log_Logger logger;

	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	plugstate_t state;
	plugstate_t stash;
	PROPS_T(props, MAX_NPROPS);

	varchunk_t *rb;
	bool rolling;
};

static const props_def_t defs [MAX_NPROPS] = {
	{
		.property = ORBIT_URI"#quantum_mode",
		.offset = offsetof(plugstate_t, mode),
		.type = LV2_ATOM__Int,
	}
};

static void
_cb(timely_t *timely, int64_t frames, LV2_URID type, void *data)
{
	plughandle_t *handle = data;

	if(type == TIMELY_URI_SPEED(timely))
	{
		handle->rolling = TIMELY_SPEED(timely) > 0.f ? true : false;

		if(!handle->rolling) // drain ringbuffer at transport stop
		{
			size_t size;
			const LV2_Atom_Event *ev;
			while( (ev = varchunk_read_request(handle->rb, &size)) )
			{
				const LV2_Atom *atom = &ev->body;

				if(handle->ref)
					handle->ref = lv2_atom_forge_frame_time(&handle->forge, frames);
				if(handle->ref)
					handle->ref = lv2_atom_forge_write(&handle->forge, atom, lv2_atom_total_size(atom));

				varchunk_read_advance(handle->rb);
			}
		}
	}
	else if(type == TIMELY_URI_BAR_BEAT(timely))
	{
		const double beats = TIMELY_BAR(timely) * TIMELY_BEATS_PER_BAR(timely)
			+ TIMELY_BAR_BEAT(timely);

		size_t size;
		const LV2_Atom_Event *ev;
		while( (ev = varchunk_read_request(handle->rb, &size)) )
		{
			if(ev->time.beats > beats)
				break; // event is for next beat

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
		else if(!strcmp(features[i]->URI, LV2_LOG__log))
			handle->log= features[i]->data;
	}

	if(!handle->map)
	{
		fprintf(stderr, "%s: Host does not support urid:map\n", descriptor->URI);
		free(handle);
		return NULL;
	}

	if(handle->log)
		lv2_log_logger_init(&handle->logger, handle->map, handle->log);

	timely_mask_t mask = TIMELY_MASK_SPEED
		| TIMELY_MASK_BAR_BEAT_WHOLE;
	timely_init(&handle->timely, handle->map, rate, mask, _cb, handle);
	lv2_atom_forge_init(&handle->forge, handle->map);

	if(!props_init(&handle->props, descriptor->URI,
		defs, MAX_NPROPS, &handle->state, &handle->stash,
		handle->map, handle))
	{
		fprintf(stderr, "failed to initialize property structure\n");
		free(handle);
		return NULL;
	}

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
	timely_t *timely = &handle->timely;
	uint32_t last_t = 0;

	const uint32_t capacity = handle->event_out->atom.size;
	LV2_Atom_Forge_Frame frame;
	lv2_atom_forge_set_buffer(&handle->forge, (uint8_t *)handle->event_out, capacity);
	handle->ref = lv2_atom_forge_sequence_head(&handle->forge, &frame, 0);

	props_idle(&handle->props, &handle->forge, 0, &handle->ref);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;

		if(  !timely_advance(timely, obj, last_t, ev->time.frames)
			&& !props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref) )
		{
			if(handle->rolling)
			{
				const size_t ev_size= sizeof(LV2_Atom_Event) + ev->body.size;
				LV2_Atom_Event *dst;
				if( (dst = varchunk_write_request(handle->rb, ev_size)) )
				{
					const double beats = TIMELY_BAR(timely) * TIMELY_BEATS_PER_BAR(timely)
						+ TIMELY_BAR_BEAT(timely);

					memcpy(dst, ev, ev_size);
					dst->time.beats = beats;
					switch((quantum_mode_t)handle->state.mode)
					{
						case MODE_FLOOR:
							dst->time.beats = floor(dst->time.beats) + 1.0;
							break;
						case MODE_ROUND:
							dst->time.beats = round(dst->time.beats) + 1.0;
							break;
						case MODE_CEIL:
							dst->time.beats = ceil(dst->time.beats);
							break;
					}

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

	timely_advance(timely, NULL, last_t, nsamples);

	if(handle->ref)
		lv2_atom_forge_pop(&handle->forge, &frame);
	else
	{
		lv2_atom_sequence_clear(handle->event_out);

		if(handle->log)
			lv2_log_trace(&handle->logger, "forge buffer overflow\n");
	}
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	varchunk_free(handle->rb);
	munlock(handle, sizeof(plughandle_t));
	free(handle);
}

static LV2_State_Status
_state_save(LV2_Handle instance, LV2_State_Store_Function store,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = instance;

	return props_save(&handle->props, store, state, flags, features);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = instance;

	return props_restore(&handle->props, retrieve, state, flags, features);
}

static const LV2_State_Interface state_iface = {
	.save = _state_save,
	.restore = _state_restore
};

static const void*
extension_data(const char* uri)
{
	if(!strcmp(uri, LV2_STATE__interface))
		return &state_iface;

	return NULL;
}

const LV2_Descriptor orbit_quantum = {
	.URI						= ORBIT_QUANTUM_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= NULL,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data 
};
