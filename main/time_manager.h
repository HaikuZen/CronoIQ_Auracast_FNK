#pragma once

#include <cstddef>

#include "app_config.h"

// Starts SNTP (via esp_netif_sntp) and applies the configured POSIX TZ.
// Must be called after Wi-Fi is initialised; it is safe to call before a
// connection is actually established — sync just won't happen until then.
void time_manager_start(const NtpConfig &cfg);

// True once at least one successful NTP sync has landed.
bool time_manager_is_synced(void);

// All three write a NUL-terminated string into buf (size len) using the
// current local time. Safe to call before the first sync — they will show
// the epoch / "Thursday, 1970-01-01" until time is set.
void time_manager_get_time_str(char *buf, size_t len);      // "HH:MM:SS"
void time_manager_get_date_str(char *buf, size_t len);       // "14 September 2026"
void time_manager_get_weekday_str(char *buf, size_t len);    // "Monday"
