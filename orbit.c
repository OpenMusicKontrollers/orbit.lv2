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

#include <orbit.h>

LV2_SYMBOL_EXPORT const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch(index)
	{
		case 0:
			return &orbit_looper;
		case 1:
			return &orbit_click;
		case 2:
			return &orbit_pacemaker;
		case 3:
			return &orbit_beatbox;
		case 4:
			return &orbit_subspace;
		case 5:
			return &orbit_timecapsule;
		case 6:
			return &orbit_quantum;
		case 7:
			return &orbit_monitor;
		default:
			return NULL;
	}
}
