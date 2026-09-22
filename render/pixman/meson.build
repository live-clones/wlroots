wlr_files += files(
	'pass.c',
	'pixel_format.c',
	'renderer.c',
)

gen_pixel_format += run_target(
	'gen-pixman-pixel-format',
	command: [files('gen_pixel_format.sh'), files('pixel_format_table.c')],
)
