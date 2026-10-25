#pragma once

#include <cstddef>

#include "app_config.h"

// Starts the Wi-Fi station and connects using the given credentials.
// Reconnects automatically on disconnect. Safe to call with an empty SSID —
// it will simply never report connected.
void wifi_manager_start(const WifiConfig &cfg);

bool wifi_manager_is_connected(void);

// Current station IPv4 address as dotted-quad text, or "-" when not
// connected.
void wifi_manager_get_ip_str(char *buf, size_t len);
