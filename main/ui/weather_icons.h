#pragma once

#include <cstdint>

#include "lvgl.h"

// App-wide background color. The moon-crescent icon "cuts out" a circle in
// this exact color, so every screen/page must use it as its background for
// the illusion to hold.
constexpr uint32_t WEATHER_ICON_BG_HEX = 0x0D1B2A;

// Small vector-drawn weather icons (sun/cloud/rain/snow/thunder/fog), built
// out of plain LVGL shapes so the project needs no bundled icon font or
// image assets. `icon_code` is a Visual Crossing icon string (e.g.
// "partly-cloudy-day", "rain", "clear-night", ...); unrecognised codes fall
// back to a generic cloud.
lv_obj_t *weather_icon_create(lv_obj_t *parent, const char *icon_code, int32_t size);

// Rebuilds the icon's contents in place for a new icon_code.
void weather_icon_update(lv_obj_t *icon_obj, const char *icon_code);
