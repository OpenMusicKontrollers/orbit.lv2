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

#define MAX_NPROPS 8

typedef struct _plugstate_t plugstate_t;
typedef struct _plughandle_t plughandle_t;

struct _plugstate_t {
	int32_t bar_enabled;
	int32_t beat_enabled;
	int32_t bar_note;
	int32_t beat_note;
	int32_t bar_channel;
	int32_t beat_channel;
	int32_t bar_led;
	int32_t beat_led;
};

struct _plughandle_t {
	struct {
		LV2_URID midi_event;
		LV2_URID bar_led;
		LV2_URID beat_led;
	} urid;

	LV2_URID_Map *map;
	LV2_Atom_Forge forge;

	timely_t timely;

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;

	plugstate_t state;
	plugstate_t stash;

	int32_t bar_note_old;
	int32_t beat_note_old;
	int32_t bar_channel_old;
	int32_t beat_channel_old;
	bool bar_on;
	bool beat_on;
	int bar_count;
	int beat_count;

	PROPS_T(props, MAX_NPROPS);
	
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
_bar_intercept(void *data, int64_t frames, props_impl_t *impl)
{
	plughandle_t *handle = data;

	if(handle->bar_on)
	{
		_note_off(handle, frames, handle->bar_channel_old, handle->bar_note_old);
		handle->bar_on = false;
	}

	handle->bar_note_old = handle->state.bar_note;
	handle->bar_channel_old = handle->state.bar_channel;
}

static void
_beat_intercept(void *data, int64_t frames, props_impl_t *impl)
{
	plughandle_t *handle = data;

	if(handle->beat_on)
	{
		_note_off(handle, frames, handle->beat_channel_old, handle->beat_note_old);
		handle->beat_on = false;
	}

	handle->beat_note_old = handle->state.beat_note;
	handle->beat_channel_old = handle->state.beat_channel;
}

static const props_def_t defs [MAX_NPROPS] = {
	{
		.property = ORBIT_URI"#beatbox_bar_enabled",
		.offset = offsetof(plugstate_t, bar_enabled),
		.type = LV2_ATOM__Bool,
		.event_cb = _bar_intercept
	},
	{
		.property = ORBIT_URI"#beatbox_beat_enabled",
		.offset = offsetof(plugstate_t, beat_enabled),
		.type = LV2_ATOM__Bool,
		.event_cb = _beat_intercept
	},
	{
		.property = ORBIT_URI"#beatbox_bar_note",
		.offset = offsetof(plugstate_t, bar_note),
		.type = LV2_ATOM__Int,
		.event_cb = _bar_intercept
	},
	{
		.property = ORBIT_URI"#beatbox_beat_note",
		.offset = offsetof(plugstate_t, beat_note),
		.type = LV2_ATOM__Int,
		.event_cb = _beat_intercept
	},
	{
		.property = ORBIT_URI"#beatbox_bar_channel",
		.offset = offsetof(plugstate_t, bar_channel),
		.type = LV2_ATOM__Int,
		.event_cb = _bar_intercept
	},
	{
		.property = ORBIT_URI"#beatbox_beat_channel",
		.offset = offsetof(plugstate_t, beat_channel),
		.type = LV2_ATOM__Int,
		.event_cb = _beat_intercept
	},
	{
		.property = ORBIT_URI"#beatbox_bar_led",
		.offset = offsetof(plugstate_t, bar_led),
		.access = LV2_PATCH__readable,
		.type = LV2_ATOM__Bool,
	},
	{
		.property = ORBIT_URI"#beatbox_beat_led",
		.offset = offsetof(plugstate_t, beat_led),
		.access = LV2_PATCH__readable,
		.type = LV2_ATOM__Bool,
	}
};

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
				_note_off(handle, frames, handle->state.bar_channel, handle->state.bar_note);
				handle->bar_on = false;
			}

			if(handle->beat_on)
			{
				_note_off(handle, frames, handle->state.beat_channel, handle->state.beat_note);
				handle->beat_on = false;
			}
		}
	}
	else if(type == TIMELY_URI_BAR_BEAT(timely))
	{
		if(handle->rolling)
		{
			bool is_bar_start = fmod(TIMELY_BAR_BEAT_RAW(timely), TIMELY_BEATS_PER_BAR(timely)) == 0.f;

			if(handle->beat_on)
			{
				_note_off(handle, frames, handle->state.beat_channel, handle->state.beat_note);
				handle->beat_on = false;
			}

			if(handle->state.beat_enabled && (handle->state.bar_enabled ? !is_bar_start : true))
			{
				_note_on(handle, frames, handle->state.beat_channel, handle->state.beat_note);
				handle->beat_on = true;

				// toggle LED
				handle->state.beat_led = 1;
				handle->beat_count = TIMELY_FRAMES_PER_SECOND(&handle->timely) * 60
					/ TIMELY_BEATS_PER_MINUTE(&handle->timely) / 2;
				if(handle->ref)
					props_set(&handle->props, &handle->forge, frames, handle->urid.beat_led, &handle->ref);
			}
		}
	}
	else if(type == TIMELY_URI_BAR(timely))
	{
		if(handle->rolling)
		{
			if(handle->bar_on)
			{
				_note_off(handle, frames, handle->state.bar_channel, handle->state.bar_note);
				handle->bar_on = false;
			}

			if(handle->state.bar_enabled)
			{
				_note_on(handle, frames, handle->state.bar_channel, handle->state.bar_note);
				handle->bar_on = true;

				// toggle LED
				handle->state.bar_led = 1;
				handle->bar_count = TIMELY_FRAMES_PER_SECOND(&handle->timely) * 60
					/ TIMELY_BEATS_PER_MINUTE(&handle->timely) / 2;
				if(handle->ref)
					props_set(&handle->props, &handle->forge, frames, handle->urid.bar_led, &handle->ref);
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

	handle->urid.midi_event = handle->map->map(handle->map->handle, LV2_MIDI__MidiEvent);

	timely_mask_t mask = TIMELY_MASK_BAR_BEAT_WHOLE
		| TIMELY_MASK_BAR_WHOLE
		| TIMELY_MASK_SPEED;
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

	handle->urid.bar_led = props_map(&handle->props, ORBIT_URI"#beatbox_bar_led");
	handle->urid.beat_led = props_map(&handle->props, ORBIT_URI"#beatbox_beat_led");

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

	props_idle(&handle->props, &handle->forge, 0, &handle->ref);

	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		if(!timely_advance(&handle->timely, obj, last_t, ev->time.frames))
			props_advance(&handle->props, &handle->forge, ev->time.frames, obj, &handle->ref);

		last_t = ev->time.frames;
	}

	timely_advance(&handle->timely, NULL, last_t, nsamples);

	if(handle->state.bar_led)
	{
		handle->bar_count -= nsamples;
		if(handle->bar_count < 0)
		{
			handle->state.bar_led = 0;
			if(handle->ref)
				props_set(&handle->props, &handle->forge, nsamples-1, handle->urid.bar_led, &handle->ref);
		}
	}

	if(handle->state.beat_led)
	{
		handle->beat_count -= nsamples;
		if(handle->beat_count < 0)
		{
			handle->state.beat_led = 0;
			if(handle->ref)
				props_set(&handle->props, &handle->forge, nsamples-1, handle->urid.beat_led, &handle->ref);
		}
	}

	if(handle->ref)
		lv2_atom_forge_pop(&handle->forge, &frame);
	else
		lv2_atom_sequence_clear(handle->event_out);
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

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

const LV2_Descriptor orbit_beatbox = {
	.URI						= ORBIT_BEATBOX_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= NULL,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
