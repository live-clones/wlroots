vulkan_custom_shaders_src = [
	'triangle.vert',
	'triangle.frag',
]

vulkan_shader_sources = []
foreach shader : vulkan_custom_shaders_src
	if shader == 'triangle.vert'
		name = 'custom_triangle_vert_data'
	else
		name = 'custom_triangle_frag_data'
	endif
	args = [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', name]
	if glslang_version.version_compare('>=11.0.0')
		args += '--quiet'
	endif
	header = custom_target(
		'custom-render-pass-vulkan-' + shader + '_spv',
		output: shader + '.h',
		input: shader,
		command: args,
	)
	vulkan_shader_sources += [header]
endforeach
