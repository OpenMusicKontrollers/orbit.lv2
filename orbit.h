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

#ifndef _ORBIT_LV2_H
#define _ORBIT_LV2_H

#include <stdint.h>
#if !defined(_WIN32)
#	include <sys/mman.h>
#else
#	define mlock(...)
#	define munlock(...)
#endif

#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/time/time.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include <lv2/lv2plug.in/ns/ext/state/state.h>
#include <lv2/lv2plug.in/ns/ext/worker/worker.h>
#include <lv2/lv2plug.in/ns/ext/log/log.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/lv2core/lv2.h>

#define ORBIT_URI										"http://open-music-kontrollers.ch/lv2/orbit"

#define ORBIT_PATH_URI							ORBIT_URI"#Path"

// plugin uris
#define ORBIT_LOOPER_URI						ORBIT_URI"#looper"
#define ORBIT_CLICK_URI							ORBIT_URI"#click"
#define ORBIT_PACEMAKER_URI					ORBIT_URI"#pacemaker"
#define ORBIT_BEATBOX_URI						ORBIT_URI"#beatbox"
#define ORBIT_SUBSPACE_URI					ORBIT_URI"#subspace"
#define ORBIT_MONITOR_URI						ORBIT_URI"#monitor"
#define ORBIT_TIMECAPSULE_URI				ORBIT_URI"#timecapsule"
#define ORBIT_QUANTUM_URI						ORBIT_URI"#quantum"

extern const LV2_Descriptor orbit_looper;
extern const LV2_Descriptor orbit_click;
extern const LV2_Descriptor orbit_pacemaker;
extern const LV2_Descriptor orbit_beatbox;
extern const LV2_Descriptor orbit_subspace;
extern const LV2_Descriptor orbit_monitor;
extern const LV2_Descriptor orbit_timecapsule;
extern const LV2_Descriptor orbit_quantum;

#endif // _ORBIT_LV2_H
