custom_pass_embed = find_program('../../../../render/gles2/shaders/embed.sh', native: true)
triangle_vert = custom_target(
	'custom-render-pass-triangle-vert',
	command: [custom_pass_embed, 'custom_triangle_vert'],
	input: 'triangle.vert',
	output: 'triangle_vert.h',
	feed: true,
	capture: true,
)
triangle_frag = custom_target(
	'custom-render-pass-triangle-frag',
	command: [custom_pass_embed, 'custom_triangle_frag'],
	input: 'triangle.frag',
	output: 'triangle_frag.h',
	feed: true,
	capture: true,
)

gles2_shader_sources = [
	triangle_vert,
	triangle_frag,
]
