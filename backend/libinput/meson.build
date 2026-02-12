msg = ['Required for libinput backend support.']
if 'libinput' in backends
	msg += 'Install "libinput" or disable the libinput backend.'
endif

libinput = dependency(
	'libinput',
	version: '>=1.19.0',
	required: 'libinput' in backends,
	not_found_message: '\n'.join(msg),
)

if not (libinput.found() and features['session'])
	subdir_done()
endif

# This condition should be dropped once required libinput version is >=1.31
if libinput.version().version_compare('>=1.30.901')
	add_project_arguments('-DLIBINPUT_HAS_KEYPAD_SLIDE_SWITCH', language: 'c')
endif

wlr_files += files(
	'backend.c',
	'events.c',
	'keyboard.c',
	'pointer.c',
	'switch.c',
	'tablet_pad.c',
	'tablet_tool.c',
	'touch.c',
)

features += { 'libinput-backend': true }
wlr_deps += libinput

internal_config.set10('HAVE_LIBINPUT_BUSTYPE', libinput.version().version_compare('>=1.26.0'))
