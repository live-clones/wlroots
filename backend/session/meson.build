libseat = dependency(
	'libseat',
	version: '>=0.2.0',
	fallback: 'seatd',
	default_options: ['server=disabled', 'man-pages=disabled', 'examples=disabled'],
	required: session_required,
)

platform = target_machine.system()
if platform == 'linux' or platform == 'freebsd'
	libudev = dependency('libudev', required: session_required)
	if not (libudev.found() and libseat.found())
		subdir_done()
	endif

	wlr_files += files('session_libudev.c')
	wlr_deps += libudev
elif platform == 'openbsd'
	wlr_files += files('session_openbsd.c')
else
	error('Unsupported platform')
endif

wlr_files += files('session.c')
wlr_deps += libseat
features += { 'session': true }

