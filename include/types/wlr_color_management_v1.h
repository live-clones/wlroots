#ifndef TYPES_WLR_COLOR_MANAGEMENT_V1_H
#define TYPES_WLR_COLOR_MANAGEMENT_V1_H

#include <wlr/types/wlr_color_management_v1.h>

uint32_t transfer_function_try_to_wlr(enum wp_color_manager_v1_transfer_function tf);
uint32_t named_primaries_try_to_wlr(enum wp_color_manager_v1_primaries primaries);

#endif
