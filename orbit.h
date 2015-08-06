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

#ifndef _ORBIT_LV2_H
#define _ORBIT_LV2_H

#include <stdint.h>

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

#define ORBIT_URI										"http://open-music-kontrollers.ch/lv2/orbit"

// plugin uris
#define ORBIT_LOOPER_URI						ORBIT_URI"#looper"
#define ORBIT_PACEMAKER_URI					ORBIT_URI"#pacemaker"

extern const LV2_Descriptor orbit_looper;
extern const LV2_Descriptor orbit_pacemaker;

typedef struct _position_t position_t;

struct _position_t {
	float bar_beat;
	int64_t bar;

	double beat;
	uint32_t beat_unit;
	float beats_per_bar;
	float beats_per_minute;

	int64_t frame;
	float frames_per_second;

	float speed;
};

#endif // _ORBIT_LV2_H
