project(
	'vulkan_gbm',
	'c',
	version: '0.1.0',
	license: 'MIT',
	meson_version: '>=0.56.0',
	default_options: [
		'c_std=c23',
		'warning_level=3',
		'werror=true',
	],
)

little_endian = target_machine.endian() == 'little'
big_endian = target_machine.endian() == 'big'
add_project_arguments([
	'-DCPU_LITTLE_ENDIAN=@0@'.format(little_endian.to_int()),
	'-DCPU_BIG_ENDIAN=@0@'.format(big_endian.to_int()),
], language: 'c')

shared_library(
	'vulkan_gbm',
	files('gbm_vulkan.c'),
	include_directories : ['.'],
	dependencies : [
		dependency('libdrm', version: '>=2.4.122'),
		dependency('vulkan', version: '>=1.2.182'),
	],
	install : true,
	install_dir: join_paths(get_option('libdir'), 'gbm'),
	name_prefix : '',
)
