# Used to test internal symbols
lib_wlr_internal = static_library(
	versioned_name + '-internal',
	objects: lib_wlr.extract_all_objects(recursive: false),
	dependencies: wlr_deps,
	include_directories: [wlr_inc],
	install: false,
)

test(
	'box',
	executable('test-box', 'test_box.c', dependencies: wlroots),
)

subsurface_test = executable('test-subsurface', 'test_subsurface.c', dependencies: wlroots)
foreach name : [
	'parent-commit',
	'ancestor-desync',
	'ancestor-desync-cached',
	'synchronized-ancestor',
	'synchronized-descendant',
	'ancestor-destroy',
	'parent-destroy',
	'external-lock',
	'direct-modes',
]
	test('subsurface-' + name, subsurface_test, args: name)
endforeach

if features.get('vulkan-renderer')
	test(
		'vulkan_stage_buffer',
		executable(
			'test-vulkan-stage-buffer',
			'test_vulkan_stage_buffer.c',
			link_with: lib_wlr_internal,
			dependencies: wlr_deps,
			include_directories: wlr_inc,
		),
	)
endif

benchmark(
	'scene',
	executable('bench-scene', 'bench_scene.c', dependencies: wlroots),
	timeout: 30,
)

benchmark(
	'render-pass',
	executable('bench-render-pass', 'bench_render_pass.c', dependencies: wlroots),
	timeout: 30,
)
