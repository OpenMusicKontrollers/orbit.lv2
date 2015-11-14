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

typedef enum _punchmode_t punchmode_t;
typedef struct _plughandle_t plughandle_t;

enum _punchmode_t {
	PUNCH_BEAT				= 0,
	PUNCH_BAR					= 1
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	
	struct {
		LV2_URID orbit_path;
	} uris;
	
	timely_t timely;
	LV2_Worker_Schedule *sched;
	bool restored; //TODO use properly

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;
	const float *punch;
	const float *mute;
	float *play_position;

	punchmode_t punch_i;
	unsigned count_i;
	float window;
	int64_t offset;

	unsigned play;
	bool rolling;
	uint8_t buf [2][BUF_SIZE];
	LV2_Atom_Event *ev;
	
	char path [1024];
};

static inline void
_play(plughandle_t *handle, int64_t to, uint32_t capacity)
{
	const LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	const int64_t ref = handle->offset - to; // beginning of current period

	while(handle->ev && !lv2_atom_sequence_is_end(&play_seq->body, play_seq->atom.size, handle->ev))
	{
		const int64_t beat_frames = handle->ev->time.beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

		if(beat_frames >= handle->offset)
			break; // event not part of this region

		LV2_Atom_Event *e = lv2_atom_sequence_append_event(handle->event_out,
			capacity, handle->ev);
		if(e)
		{
			e->time.frames = beat_frames - ref;
		}
		else
		{
			break; // overflow
		}

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
		e->time.beats = handle->offset / TIMELY_FRAMES_PER_BEAT(&handle->timely);
	}
	else
	{
		// overflow
	}
}

static inline void
_reposition_play(plughandle_t *handle)
{
	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];

	LV2_ATOM_SEQUENCE_FOREACH(play_seq, ev)
	{
		const int64_t beat_frames = ev->time.beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

		if(beat_frames >= handle->offset)
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
		const int64_t beat_frames = ev->time.beats * TIMELY_FRAMES_PER_BEAT(&handle->timely);

		if(beat_frames >= handle->offset)
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

	/*FIXME
	if(handle->punch_i == PUNCH_BEAT)
		handle->window = 100.f / (handle->width_i * TIMELY_FRAMES_PER_BEAT(timely));
	else if(handle->punch_i == PUNCH_BAR)
		handle->window = 100.f / (handle->width_i * TIMELY_FRAMES_PER_BAR(timely));
	*/
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

		/*FIXME
		if(handle->punch_i == PUNCH_BEAT)
			handle->offset = fmod(beats, handle->width_i) * TIMELY_FRAMES_PER_BEAT(timely);
		else if(handle->punch_i == PUNCH_BAR)
			handle->offset = fmod(beats, handle->width_i * TIMELY_BEATS_PER_BAR(timely))
				* TIMELY_FRAMES_PER_BEAT(timely);
		*/

		if(handle->offset == 0)
			handle->play ^= 1;

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
	
	const LV2_State_Make_Path *make_path = NULL;

	for(unsigned i=0; features[i]; i++)
	{
		if(!strcmp(features[i]->URI, LV2_URID__map))
			handle->map = features[i]->data;
		else if(!strcmp(features[i]->URI, LV2_WORKER__schedule))
			handle->sched = features[i]->data;
		else if(!strcmp(features[i]->URI, LV2_STATE__makePath))
			make_path = features[i]->data;
	}

	if(!handle->map)
	{
		fprintf(stderr,
			"%s: Host does not support urid:map\n", descriptor->URI);
		free(handle);
		return NULL;
	}
	if(!handle->sched)
	{
		fprintf(stderr,
			"%s: Host does not support worker:sched\n", descriptor->URI);
		free(handle);
		return NULL;
	}
	if(!make_path)
	{
		fprintf(stderr,
			"%s: Host does not support state:makePath\n", descriptor->URI);
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

	char *tmp = make_path->path(make_path->handle, "dump.atom");
	strcpy(handle->path, tmp);
	free(tmp);
	
	handle->uris.orbit_path = handle->map->map(handle->map->handle,
		ORBIT_PATH_URI);

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
			handle->mute = (const float *)data;
			break;
		case 4:
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

	LV2_Atom_Sequence *play_seq = (LV2_Atom_Sequence *)handle->buf[handle->play];
	LV2_Atom_Sequence *rec_seq = (LV2_Atom_Sequence *)handle->buf[!handle->play];

	play_seq->atom.type = handle->forge.Sequence;
	play_seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
	play_seq->body.unit = 0;
	play_seq->body.pad = 0;

	rec_seq->atom.type = handle->forge.Sequence;
	rec_seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
	rec_seq->body.unit = 0;
	rec_seq->body.pad = 0;
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	uint32_t capacity = handle->event_out->atom.size;

	punchmode_t punch_i = floor(*handle->punch);
	bool punch_has_changed = punch_i != handle->punch_i;
	handle->punch_i = punch_i;

	/* FIXME
	int64_t width_i = floor(*handle->width);
	bool width_has_changed = width_i != handle->width_i;
	handle->width_i = width_i;

	if(punch_has_changed || width_has_changed)
		_window_refresh(handle);
	*/

	bool mute_i = floor(*handle->mute);

	lv2_atom_sequence_clear(handle->event_out);

	int64_t last_t = 0;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		if(handle->rolling)
			handle->offset += ev->time.frames - last_t;

		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		const int time_event = timely_advance(&handle->timely, obj, last_t, ev->time.frames);

		if(!time_event && handle->rolling)
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

	*handle->play_position = handle->offset * handle->window;
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	free(handle);
}

static LV2_State_Status
_state_save(LV2_Handle instance, LV2_State_Store_Function store,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

	const LV2_State_Map_Path *map_path = NULL;

	for(int i=0; features[i]; i++)
		if(!strcmp(features[i]->URI, LV2_STATE__mapPath))
			map_path = features[i]->data;

	if(!map_path)
	{
		fprintf(stderr, "_state_save: LV2_STATE__mapPath not supported.");
		return LV2_STATE_ERR_UNKNOWN;
	}

	const char *abstract = map_path->abstract_path(map_path->handle, handle->path);

	return store(
		state,
		handle->uris.orbit_path,
		abstract,
		strlen(abstract) + 1,
		handle->forge.Path,
		LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static LV2_State_Status
_state_restore(LV2_Handle instance, LV2_State_Retrieve_Function retrieve,
	LV2_State_Handle state, uint32_t flags,
	const LV2_Feature *const *features)
{
	plughandle_t *handle = (plughandle_t *)instance;

	const LV2_State_Map_Path *map_path = NULL;

	for(int i=0; features[i]; i++)
		if(!strcmp(features[i]->URI, LV2_STATE__mapPath))
			map_path = features[i]->data;

	if(!map_path)
	{
		fprintf(stderr, "_state_restore: LV2_STATE__mapPath not supported.");
		return LV2_STATE_ERR_UNKNOWN;
	}

	size_t size;
	uint32_t type;
	uint32_t flags2;
	const char *path = retrieve(
		state,
		handle->uris.orbit_path,
		&size,
		&type,
		&flags2
	);

	// check type
	if(path && (type != handle->forge.Path) )
		return LV2_STATE_ERR_BAD_TYPE;

	if(!path)
		path = "dump.atom";

	const char *absolute = map_path->absolute_path(map_path->handle, path);

	strcpy(handle->path, absolute);
	handle->restored = true;

	return LV2_STATE_SUCCESS;
}

static const LV2_State_Interface state_iface = {
	.save = _state_save,
	.restore = _state_restore
};

// non-rt thread
static LV2_Worker_Status
_work(LV2_Handle instance,
	LV2_Worker_Respond_Function respond,
	LV2_Worker_Respond_Handle target,
	uint32_t size,
	const void *body)
{
	plughandle_t *handle = instance;

	//TODO

	return LV2_WORKER_SUCCESS;
}

// rt-thread
static LV2_Worker_Status
_work_response(LV2_Handle instance, uint32_t size, const void *body)
{
	plughandle_t *handle = instance;

	//TODO

	return LV2_WORKER_SUCCESS;
}

// rt-thread
static LV2_Worker_Status
_end_run(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	//TODO

	return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface work_iface = {
	.work = _work,
	.work_response = _work_response,
	.end_run = _end_run
};

static const void*
extension_data(const char* uri)
{
	if(!strcmp(uri, LV2_STATE__interface))
		return &state_iface;
	else if(!strcmp(uri, LV2_WORKER__interface))
		return &work_iface;

	return NULL;
}

const LV2_Descriptor orbit_cargoship = {
	.URI						= ORBIT_CARGOSHIP_URI,
	.instantiate		= instantiate,
	.connect_port		= connect_port,
	.activate				= activate,
	.run						= run,
	.deactivate			= NULL,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
