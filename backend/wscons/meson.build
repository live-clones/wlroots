platform = target_machine.system()
if platform != 'openbsd'
	subdir_done()
endif

wlr_files += files('atKeynames.h')
wlr_files += files('bsd_KbdMap.h')
wlr_files += files('backend.c')
features += { 'wscons-backend': true }
