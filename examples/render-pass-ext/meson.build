gles2_sources = []
pixman_sources = []
vulkan_sources = []

 if features.get('gles2-renderer')
 	subdir('gles2')
 endif

subdir('pixman')

# if features.get('vulkan-renderer')
# 	subdir('vulkan')
# endif

executable(
	'render-pass-ext',
	[
		'triangle_pass.c',
		'render-pass-ext.c',
		gles2_sources,
		pixman_sources,
		vulkan_sources,
	],
	dependencies: [wlroots, libdrm_header],
	build_by_default: get_option('examples'),
)
