#pragma once

#include "lvgl.h"

// A row of LED-style 7-segment digits/colons, built purely out of LVGL
// rectangle objects (no custom font/image assets needed). Segments not lit
// stay visible at low opacity, like an unpowered LED digit.
//
// Creates the widget sized for the given digit height; call
// sseg_time_set_text afterwards (and on every update) with a string made of
// '0'-'9' and ':' characters only, e.g. "23:59:07".
lv_obj_t *sseg_time_create(lv_obj_t *parent, int32_t digit_height, lv_color_t on_color, lv_color_t off_color);

void sseg_time_set_text(lv_obj_t *cont, const char *text);
