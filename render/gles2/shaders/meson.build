embed = find_program('./embed.sh', native: true)

glslang_version_info = run_command(glslang, '--version', check: true).stdout()
glslang_version = glslang_version_info.split('\n')[0].split(':')[-1]

# New enough glslang versions print to stderr and don't need the wrapper script
check_cmd = []
if glslang_version.version_compare('<16.4.0')
	check = find_program('./check.sh', native: true)
	check_cmd = [check]
endif

shaders = [
	'common.vert',
	'quad.frag',
	'tex_rgba.frag',
	'tex_rgbx.frag',
	'tex_external.frag',
]

foreach name : shaders
	custom_target(
		'gles2-' + name,
		input: name,
		output: name + '_check',
		command: check_cmd + [glslang, '@INPUT@'],
		capture: true,
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
