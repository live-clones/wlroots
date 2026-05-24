libseat = dependency(
	'libseat',
	version: '>=0.2.0',
	fallback: 'seatd',
	default_options: ['server=disabled', 'man-pages=disabled', 'examples=disabled'],
	required: session_required,
	not_found_message: 'Required for session support.',
)
if not libseat.found()
	subdir_done()
endif

wlr_files += files('session.c')
wlr_deps += [libseat]
features += { 'session': true }

udev = dependency('libudev', required: get_option('udev'))
if udev.found()
	wlr_files += files('udev.c')
	wlr_deps += [udev]
	internal_features += { 'udev': true }
endif
