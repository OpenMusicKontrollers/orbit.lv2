## Orbit

### An LV2 time event manipulation plugin bundle

#### Build status

[![build status](https://gitlab.com/OpenMusicKontrollers/orbit.lv2/badges/master/build.svg)](https://gitlab.com/OpenMusicKontrollers/orbit.lv2/commits/master)

### Binaries

For GNU/Linux (64-bit, 32-bit, armv7), Windows (64-bit, 32-bit) and MacOS
(64/32-bit univeral).

To install the plugin bundle on your system, simply copy the __orbit.lv2__
folder out of the platform folder of the downloaded package into your
[LV2 path](http://lv2plug.in/pages/filesystem-hierarchy-standard.html).

<!--
#### Stable release

* [orbit.lv2-0.16.0.zip](https://dl.open-music-kontrollers.ch/orbit.lv2/stable/orbit.lv2-0.16.0.zip) ([sig](https://dl.open-music-kontrollers.ch/orbit.lv2/stable/orbit.lv2-0.16.0.zip.sig))
-->

#### Unstable (nightly) release

* [orbit.lv2-latest-unstable.zip](https://dl.open-music-kontrollers.ch/orbit.lv2/unstable/orbit.lv2-latest-unstable.zip) ([sig](https://dl.open-music-kontrollers.ch/orbit.lv2/unstable/orbit.lv2-latest-unstable.zip.sig))

### Sources

<!--
#### Stable release

* [orbit.lv2-0.16.0.tar.xz](https://git.open-music-kontrollers.ch/lv2/orbit.lv2/snapshot/orbit.lv2-0.16.0.tar.xz)
-->

#### Git repository

* <https://git.open-music-kontrollers.ch/lv2/orbit.lv2>

<!--
### Packages

* [ArchLinux](https://www.archlinux.org/packages/community/x86_64/orbit.lv2/)
-->

### Bugs and feature requests

* [Gitlab](https://gitlab.com/OpenMusicKontrollers/orbit.lv2)
* [Github](https://github.com/OpenMusicKontrollers/orbit.lv2)

### Plugins

#### Beatbox

Creates MIDI events based on LV2 time position events (bars and beats),
e.g. to drive a drum machine. Bars and beats can be disabled/enabled
separately.

#### Click
	
Synthesizes click tracks based on LV2 time position events (bars and beats).
Bars and beats can be disabled/enabled separately.

#### Looper

Loops arbitrary LV2 atom events on a ping-pong buffer. E.g. loops MIDI,
OSC or anything else that can be packed into LV2 atoms with sample
accuracy. Needs to be driven by LV2 time position events.

#### Pacemaker

Creates LV2 time position events from scratch to drive other plugins.

#### Quantum

Quantizes incoming events to whole beats.

#### Subspace
	
Subdivide or multiply incoming time signals by whole fractions, e.g. to
speed up time x2, x3, ... or slow it down to x1/2, x1/3, ...

#### Timecapsule
	
Record/Playback of arbitrary LV2 atoms to/from memory. Record all incoming atom
messages with sample accuracy and play them back later from memory. Stored atom
event data is part of the plugin state and thus preserved across instantiations.

### Dependencies

* [LV2](http://lv2plug.in) (LV2 Plugin Standard)

### Build / install

	git clone https://git.open-music-kontrollers.ch/lv2/orbit.lv2
	cd orbit.lv2
	meson build
	cd build
	ninja -j4
	sudo ninja install

### License

Copyright (c) 2015-2016 Hanspeter Portner (dev@open-music-kontrollers.ch)

This is free software: you can redistribute it and/or modify
it under the terms of the Artistic License 2.0 as published by
The Perl Foundation.

This source is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
Artistic License 2.0 for more details.

You should have received a copy of the Artistic License 2.0
along the source as a COPYING file. If not, obtain it from
<http://www.perlfoundation.org/artistic_license_2_0>.
