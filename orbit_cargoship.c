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
#include <varchunk.h>

#define BUF_SIZE 0x10000
#define DEFAULT_FILE_NAME "seq.atom"

typedef enum _jobtype_t jobtype_t;
typedef struct _job_t job_t;
typedef struct _plughandle_t plughandle_t;

enum _jobtype_t {
	JOB_PLAY				= 0,
	JOB_RECORD			= 1,
	JOB_SEEK				= 2,
	JOB_OPEN_PLAY		= 3,
	JOB_OPEN_RECORD	= 4,
	JOB_DRAIN				= 5
};

struct _job_t {
	jobtype_t type;
	union {
		double beats;
	};
};

struct _plughandle_t {
	LV2_URID_Map *map;
	LV2_Atom_Forge forge;
	
	struct {
		LV2_URID orbit_path;
		LV2_URID orbit_drain;
	} uris;

	timely_t timely;
	LV2_Worker_Schedule *sched;
	bool restored; //TODO use properly

	const LV2_Atom_Sequence *event_in;
	LV2_Atom_Sequence *event_out;
	const float *record;

	bool record_i;

	double beats_period;
	double beats_upper;

	bool rolling;
	int draining;

	char path [1024];
	FILE *io;

	double seek_beats;

	varchunk_t *to_disk;
	varchunk_t *from_disk;
};

static inline LV2_Worker_Status
_trigger_job(plughandle_t *handle, const job_t *job)
{
	return handle->sched->schedule_work(handle->sched->handle, sizeof(job_t), job);
}

static inline LV2_Worker_Status
_trigger_record(plughandle_t *handle)
{
	const job_t job = {
		.type = JOB_RECORD
	};

	return _trigger_job(handle, &job);
}

static inline LV2_Worker_Status
_trigger_play(plughandle_t *handle)
{
	const job_t job = {
		.type = JOB_PLAY
	};

	return _trigger_job(handle, &job);
}

static inline LV2_Worker_Status
_trigger_seek(plughandle_t *handle, double beats)
{
	const job_t job = {
		.type = JOB_SEEK,
		.beats = beats
	};

	return _trigger_job(handle, &job);
}

static inline LV2_Worker_Status
_trigger_open_play(plughandle_t *handle)
{
	const job_t job = {
		.type = JOB_OPEN_PLAY
	};

	return _trigger_job(handle, &job);
}

static inline LV2_Worker_Status
_trigger_open_record(plughandle_t *handle)
{
	const job_t job = {
		.type = JOB_OPEN_RECORD
	};

	return _trigger_job(handle, &job);
}

static inline LV2_Worker_Status
_trigger_drain(plughandle_t *handle)
{
	const job_t job = {
		.type = JOB_DRAIN
	};

	return _trigger_job(handle, &job);
}

static inline void
_play(plughandle_t *handle, int64_t to, uint32_t capacity)
{
	const LV2_Atom_Event *src;
	size_t len;
	while((src = varchunk_read_request(handle->from_disk, &len)))
	{
		if(handle->draining > 0)
		{
			if(src->body.type == handle->uris.orbit_drain)
			{
				//fprintf(stderr, "_play: draining %i\n", handle->draining);
				handle->draining -= 1;
			}

			varchunk_read_advance(handle->from_disk);
			continue;
		}

		//fprintf(stderr, "_play: %lf %lf\n", src->time.beats, handle->beats_upper);

		if(src->time.beats >= handle->beats_upper)
			break; // event not part of this region

		LV2_Atom_Event *dst = lv2_atom_sequence_append_event(handle->event_out,
			capacity, src);
		if(dst)
		{
			dst->time.frames = round((src->time.beats - handle->beats_period) * TIMELY_FRAMES_PER_BEAT(&handle->timely));

			if(dst->time.frames < 0)
			{
				//fprintf(stderr, "_play: event late %li\n", dst->time.frames);
				dst->time.frames = 0;
			}
		}
		else
			break; // overflow

		varchunk_read_advance(handle->from_disk);
	}
}

static inline void
_rec(plughandle_t *handle, const LV2_Atom_Event *src)
{
	// add event to ring buffer
	LV2_Atom_Event *dst;
	size_t len = sizeof(LV2_Atom_Event) + src->body.size;
	if((dst = varchunk_write_request(handle->to_disk, len)))
	{
		dst->time.beats = handle->beats_upper;
		dst->body.type = src->body.type;
		dst->body.size = src->body.size;
		memcpy(LV2_ATOM_BODY(&dst->body), LV2_ATOM_BODY_CONST(&src->body), src->body.size);
		varchunk_write_advance(handle->to_disk, len);
	}
}

static inline void
_reposition_play(plughandle_t *handle, double beats)
{
	handle->draining += 1;
	_trigger_drain(handle);
	_trigger_seek(handle, beats);
}

static inline void
_reposition_rec(plughandle_t *handle, double beats)
{
	_trigger_seek(handle, beats);
}

static inline double
_beats(timely_t *timely)
{
	double beats = TIMELY_BAR(timely)
		* TIMELY_BEATS_PER_BAR(timely)
		+ TIMELY_BAR_BEAT(timely)
		+ timely->offset.beat / TIMELY_FRAMES_PER_BEAT(timely);

	return beats;
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
		double beats = _beats(&handle->timely);

		if(handle->record_i)
			_reposition_rec(handle, beats);
		else
			_reposition_play(handle, beats);
	}
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

	char *tmp = make_path->path(make_path->handle, DEFAULT_FILE_NAME);
	strcpy(handle->path, tmp);
	free(tmp);
	
	handle->uris.orbit_path = handle->map->map(handle->map->handle, ORBIT_PATH_URI);
	handle->uris.orbit_drain = handle->map->map(handle->map->handle, ORBIT_DRAIN_URI);

	handle->to_disk = varchunk_new(BUF_SIZE);
	handle->from_disk = varchunk_new(BUF_SIZE);

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
			handle->record = (const float *)data;
			break;
		default:
			break;
	}
}

static void
activate(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	handle->rolling = false;
	handle->restored = true;
	handle->seek_beats = 0.0;

	handle->beats_period = 0.0;
	handle->beats_upper = 0.0;
}

static void
run(LV2_Handle instance, uint32_t nsamples)
{
	plughandle_t *handle = instance;
	uint32_t capacity = handle->event_out->atom.size;

	bool record_i = floor(*handle->record);
	bool record_has_changed = record_i != handle->record_i;
	handle->record_i = record_i;

	// trigger pre triggers
	if(handle->restored || record_has_changed)
	{
		if(record_i)
			_trigger_open_record(handle);
		else
			_trigger_open_play(handle);

		handle->restored = false;
	}

	lv2_atom_sequence_clear(handle->event_out);

	handle->beats_period = handle->beats_upper;

	int64_t last_t = 0;
	LV2_ATOM_SEQUENCE_FOREACH(handle->event_in, ev)
	{
		const LV2_Atom_Object *obj = (const LV2_Atom_Object *)&ev->body;
		const int time_event = timely_advance(&handle->timely, obj, last_t, ev->time.frames);
		handle->beats_upper = _beats(&handle->timely);

		if(record_i && !time_event && handle->rolling)
			_rec(handle, ev); // dont' record time position signals
	
		if(!record_i && handle->rolling)
			_play(handle, ev->time.frames, capacity);

		last_t = ev->time.frames;
	}

	timely_advance(&handle->timely, NULL, last_t, nsamples);
	handle->beats_upper = _beats(&handle->timely);

	if(!record_i && handle->rolling)
		_play(handle, nsamples, capacity);

	// trigger post triggers
	if(handle->rolling)
	{
		if(record_i)
			_trigger_record(handle);
		else
			_trigger_play(handle);
	}
}

static void
deactivate(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	if(handle->io)
	{
		fclose(handle->io);
		handle->io = NULL;
	}
}

static void
cleanup(LV2_Handle instance)
{
	plughandle_t *handle = instance;

	if(handle->to_disk)
		varchunk_free(handle->to_disk);
	if(handle->from_disk)
		varchunk_free(handle->from_disk);

	if(handle->io)
		fclose(handle->io);

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
		path = DEFAULT_FILE_NAME;

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

	const job_t *job = body;
	switch(job->type)
	{
		case JOB_PLAY:
		{
			while(!feof(handle->io))
			{
				// read event header from disk
				LV2_Atom_Event ev1;
				if(fread(&ev1, sizeof(LV2_Atom_Event), 1, handle->io) == 1)
				{
					size_t len = sizeof(LV2_Atom_Event) + ev1.body.size;
					LV2_Atom_Event *ev2;
					if((ev2 = varchunk_write_request(handle->from_disk, len)))
					{
						// clone event data
						ev2->time.beats = ev1.time.beats;
						ev2->body.type = ev1.body.type;
						ev2->body.size = ev1.body.size;

						// read event body from disk
						if(ev1.body.size == 0)
						{
							//fprintf(stderr, "play: beats %lf %u\n", ev1.time.beats, ev1.body.size);
							handle->seek_beats = ev1.time.beats;
							varchunk_write_advance(handle->from_disk, len);
						}
						else if(fread(LV2_ATOM_BODY(&ev2->body), ev1.body.size, 1, handle->io) == 1)
						{
							//fprintf(stderr, "play: beats %lf %u\n", ev1.time.beats, ev1.body.size);
							handle->seek_beats = ev1.time.beats;
							varchunk_write_advance(handle->from_disk, len);
						}
						else
						{
							if(!feof(handle->io))
								fprintf(stderr, "play: reading event body failed.\n");
						}
					}
					else
					{
						fseek(handle->io, -sizeof(LV2_Atom_Event), SEEK_CUR); // rewind

						break; // ringbuffer full
					}
				}
				else
				{
					if(!feof(handle->io))
						fprintf(stderr, "play: reading event header failed.\n");
				}
			}

			break;
		}
		case JOB_RECORD:
		{
			const LV2_Atom_Event *ev;
			size_t len;
			while((ev = varchunk_read_request(handle->to_disk, &len)))
			{
				if(fwrite(ev, len, 1, handle->io) == 1)
				{
					//fprintf(stderr, "record: beats %lf %u\n", ev->time.beats, ev->body.size);
					handle->seek_beats = ev->time.beats;
					varchunk_read_advance(handle->to_disk);
				}
				else
				{
					fprintf(stderr, "record: writing event failed.\n");
					break;
				}
			}

			break;
		}
		case JOB_SEEK:
		{
			fprintf(stderr, "seek: request to %lf\n", job->beats);

			// rewind to start of file
			if(job->beats < handle->seek_beats)
			{
				fprintf(stderr, "seek rewind\n");
				fseek(handle->io, 0, SEEK_SET);
			}

			while(!feof(handle->io))
			{
				// read event header from disk
				LV2_Atom_Event ev1;
				if(fread(&ev1, sizeof(LV2_Atom_Event), 1, handle->io) == 1)
				{
					// check whether event precedes current seek
					if(ev1.time.beats < job->beats)
					{
						// skip event
						if(ev1.body.size > 0)
							fseek(handle->io, ev1.body.size, SEEK_CUR); //FIXME check return

						continue;
					}

					// unread event header
					fseek(handle->io, -sizeof(LV2_Atom_Event), SEEK_CUR); //FIXME check return
					fprintf(stderr, "seek: seeked to %lf\n", ev1.time.beats);

					break;
				}
				else
				{
					if(!feof(handle->io))
						fprintf(stderr, "seek: reading event header failed.\n");
				}
			}

			break;
		}
		case JOB_DRAIN:
		{
			// inject drain marker event
			LV2_Atom_Event *ev;
			const size_t len = sizeof(LV2_Atom_Event);
			if((ev = varchunk_write_request(handle->from_disk, len)))
			{
				ev->time.beats = 0.0; //XXX
				ev->body.type = handle->uris.orbit_drain;
				ev->body.size = 0;

				varchunk_write_advance(handle->from_disk, len);
			}
			else
				fprintf(stderr, "drain: ringbuffer overflow\n");

			break;
		}
		case JOB_OPEN_PLAY:
		{
			fprintf(stderr, "open for playback: %s\n", handle->path);
			if(handle->io)
				fclose(handle->io);
			if(!(handle->io = fopen(handle->path, "r+")))
				fprintf(stderr, "failed to open file.\n");

			break;
		}
		case JOB_OPEN_RECORD:
		{
			fprintf(stderr, "open for recording: %s\n", handle->path);
			if(handle->io)
				fclose(handle->io);
			if(!(handle->io = fopen(handle->path, "w+")))
				fprintf(stderr, "failed to open file.\n");

			break;
		}
	}

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
	.deactivate			= deactivate,
	.cleanup				= cleanup,
	.extension_data	= extension_data
};
