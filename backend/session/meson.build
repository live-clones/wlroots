libseat = dependency(
	'libseat',
	version: '>=0.2.0',
	fallback: 'seatd',
	default_options: ['server=disabled', 'man-pages=disabled', 'examples=disabled'],
	required: session_required,
	not_found_message: 'Required for session support.',
)
udev = dependency('libudev', required: get_option('udev'))
has_netlink = cc.has_header('linux/netlink.h')
has_device_manager = udev.found() or has_netlink
if session_required and not has_device_manager
	error('Session requires either udev or <linux/netlink.h>')
endif
if not (libseat.found() and has_device_manager)
	subdir_done()
endif

wlr_files += files('session.c')
wlr_deps += [libseat]
features += { 'session': true }

if udev.found()
	wlr_files += files('udev.c')
	wlr_deps += [udev]
	internal_features += { 'udev': true }
endif

if has_netlink
	wlr_files += files('linux.c')
endif
