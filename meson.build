project('orbit.lv2', 'c', default_options : [
	'buildtype=release',
	'warning_level=1',
	'werror=false',
	'b_lto=false',
	'c_std=c11'])

source_root = meson.source_root()
build_root = meson.build_root()

add_project_arguments('-D_GNU_SOURCE', language : 'c')

conf_data = configuration_data()
cc = meson.get_compiler('c')

cp = find_program('cp')
lv2_validate = find_program('lv2_validate', native : true, required : false)
sord_validate = find_program('sord_validate', native : true, required : false)
lv2lint = find_program('lv2lint', required : false)
clone = [cp, '@INPUT@', '@OUTPUT@']

m_dep = cc.find_library('m')
lv2_dep = dependency('lv2', version : '>=1.14.0')
zlib_dep = dependency('zlib', version : '>=1.2.0',
	static : meson.is_cross_build() and false) #FIXME
dsp_deps = [m_dep, lv2_dep, zlib_dep]

props_inc = include_directories('props.lv2')
netatom_inc = include_directories('netatom.lv2')
timely_inc = include_directories('timely.lv2')
varchunk_inc = include_directories('varchunk')
inc_dir = [props_inc, netatom_inc, timely_inc, varchunk_inc]

inst_dir = join_paths(get_option('libdir'), 'lv2', meson.project_name())

dsp_srcs = ['orbit.c',
	'orbit_beatbox.c',
	'orbit_click.c',
	'orbit_looper.c',
	'orbit_pacemaker.c',
	'orbit_quantum.c',
	'orbit_subspace.c',
	'orbit_monitor.c',
	'orbit_timecapsule.c']

c_args = ['-fvisibility=hidden',
	'-ffast-math',
	'-Wno-unused-variable']

if host_machine.system() == 'windows'
	dsp_deps += cc.find_library('ws2_32')
elif host_machine.system() == 'darwin'
	# nothing
else
	# nothing
endif

mod = shared_module('orbit', dsp_srcs,
	c_args : c_args,
	include_directories : inc_dir,
	name_prefix : '',
	dependencies : dsp_deps,
	install : true,
	install_dir : inst_dir)

version = run_command('cat', 'VERSION').stdout().strip().split('.')
conf_data.set('MAJOR_VERSION', version[0])
conf_data.set('MINOR_VERSION', version[1])
conf_data.set('MICRO_VERSION', version[2])

suffix = mod.full_path().strip().split('.')[-1]
conf_data.set('MODULE_SUFFIX', '.' + suffix)

manifest_ttl = configure_file(input : 'manifest.ttl.in', output : 'manifest.ttl',
	configuration : conf_data,
	install : true,
	install_dir : inst_dir)
dsp_ttl = custom_target('orbit_ttl',
	input : 'orbit.ttl',
	output : 'orbit.ttl',
	command : clone,
	install : true,
	install_dir : inst_dir)

if lv2_validate.found() and sord_validate.found()
	test('LV2 validate', lv2_validate,
		args : [manifest_ttl, dsp_ttl])
endif

if lv2lint.found()
	test('LV2 lint', lv2lint,
		args : ['-I', join_paths(build_root, ''),
			'http://open-music-kontrollers.ch/lv2/orbit#beatbox',
			'http://open-music-kontrollers.ch/lv2/orbit#click',
			'http://open-music-kontrollers.ch/lv2/orbit#looper',
			'http://open-music-kontrollers.ch/lv2/orbit#pacemaker',
			'http://open-music-kontrollers.ch/lv2/orbit#quantum',
			'http://open-music-kontrollers.ch/lv2/orbit#subspace',
			'http://open-music-kontrollers.ch/lv2/orbit#monitor',
			'http://open-music-kontrollers.ch/lv2/orbit#timecapsule'])
endif
