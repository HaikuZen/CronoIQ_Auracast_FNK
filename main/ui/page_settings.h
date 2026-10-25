#pragma once

#include "app_config.h"

// Full-screen Settings overlay (not a tab), opened from the gear button on
// the Clock page. Edits Wi-Fi credentials, timezone, weather units/refresh
// interval and the indoor sensor, plus a read-only Info section. "Save &
// restart" writes through config_store_save() (flash copy + SD card if
// present) and reboots so every service picks the new values up.

// Keeps a copy of the running config to pre-fill the form. Call once from
// ui_manager_create().
void page_settings_init(const AppConfig &cfg);

// Builds and shows the overlay on the active screen. Must be called on the
// LVGL task (e.g. from an event callback). No-op if already open.
void page_settings_open(void);
