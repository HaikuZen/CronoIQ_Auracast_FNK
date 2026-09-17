#pragma once

#include "lvgl.h"
#include "smart_lights_service.h"

lv_obj_t *page_smart_lights_create(lv_obj_t *parent);

// Rebuilds the light status cards. Must be called with the LVGL port
// already locked.
void page_smart_lights_update(const std::vector<LightStatus> &lights);
