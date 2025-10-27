embed = find_program('./embed.sh', native: true)

shaders = [
	'gles2_common.vert',
	'gles2_quad.frag',
	'gles2_tex_rgba.frag',
	'gles2_tex_rgbx.frag',
	'gles2_tex_external.frag',
]

foreach name : shaders
	custom_target(
		'gles-' + name,
		input: name,
		output: name + '_check',
		command: [glslang, '@INPUT@'],
		build_by_default: true,
	)

	output = name.underscorify() + '_src.h'
	var = name.underscorify() + '_src'
	wlr_files += custom_target(
		output,
		command: [embed, var],
		input: name,
		output: output,
		feed: true,
		capture: true,
	)
endforeach
