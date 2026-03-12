test(
	'box',
	executable('test-box', 'test_box.c', dependencies: wlroots),
)

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
