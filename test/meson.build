wayland_client = dependency('wayland-client', kwargs: wayland_kwargs)

test(
	'box',
	executable('test-box', 'test_box.c', dependencies: wlroots),
)

test(
	'surface',
	executable('test-surface',
		[
			'test_surface.c',
			'../util/shm.c',
			protocols_code['viewporter'],
			protocols_client_header['viewporter'],
		],
		dependencies: [wlroots, wayland_client]),
)

benchmark(
	'scene',
	executable('bench-scene', 'bench_scene.c', dependencies: wlroots),
	timeout: 30,
)
