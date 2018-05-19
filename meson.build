project('orbit.lv2', 'c', default_options : [
	'buildtype=release',
	'warning_level=1',
	'werror=false',
	'b_lto=true',
	'c_std=c11'])

add_project_arguments('-D_GNU_SOURCE', language : 'c')

conf_data = configuration_data()
cc = meson.get_compiler('c')

cp = find_program('cp')
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
	'orbit_cargoship.c',
	'orbit_click.c',
	'orbit_looper.c',
	'orbit_pacemaker.c',
	'orbit_quantum.c',
	'orbit_subspace.c',
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

configure_file(input : 'manifest.ttl.in', output : 'manifest.ttl',
	configuration : conf_data,
	install : true,
	install_dir : inst_dir)
custom_target('orbit_ttl',
	input : 'orbit.ttl',
	output : 'orbit.ttl',
	command : clone,
	install : true,
	install_dir : inst_dir)
