glesv2 = dependency('glesv2', required: 'gles2' in renderers)
glesv3 = dependency('glesv2', required: 'gles3' in renderers)

if not ((glesv2.found() or glesv3.found()) and internal_features['egl'])
	subdir_done()
endif

glslang = find_program('glslang', 'glslangValidator', native: true, required: false, disabler: true)

if glesv2.found()
    features += { 'gles2-renderer': true }
    wlr_deps += glesv2
endif
if glesv3.found()
    features += { 'gles3-renderer': true }
    wlr_deps += glesv3
endif

wlr_files += files(
	'pass.c',
	'pixel_format.c',
	'renderer.c',
	'texture.c',
)

subdir('shaders')
