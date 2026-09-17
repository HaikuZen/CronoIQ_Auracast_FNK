#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app_config.h"

struct LightStatus {
    std::string name;
    std::string brand;
    std::string ip;
    uint16_t udp_port = 38899;
    bool online = false;
    bool power = false;      // on/off
    uint8_t brightness = 0;  // 0-100 ("dimming")
    // Approximate display color: the light's actual r/g/b when in color
    // mode, or a fixed warm-white swatch when it's in a tunable-white/CCT
    // mode (we don't render actual Kelvin values). Meaningless when
    // !online or !power.
    uint8_t r = 0, g = 0, b = 0;
    // WiZ dynamic scene currently active (1-32, see kSceneNames in
    // page_smart_lights.cpp), or 0 when the light is in plain color/CCT
    // mode rather than running a scene.
    uint8_t scene_id = 0;
};

using SmartLightsUpdateCb = std::function<void(const std::vector<LightStatus> &)>;

// Spawns a background task that periodically polls each configured light
// for its current status (WiZ over UDP in this release) and invokes
// on_update with the full list after every poll cycle. Does nothing (logs a
// warning) if no lights are configured. A light whose brand isn't
// implemented yet is still included in the callback, reported as offline,
// so the UI can show it rather than silently drop it.
void smart_lights_service_start(const std::vector<SmartLightConfig> &lights, SmartLightsUpdateCb on_update);

// Fire-and-forget WiZ control commands: a single non-blocking UDP send, no
// reply is read. Safe to call directly from an LVGL event callback (unlike
// the polling above, which does a full send+wait-for-reply round trip and
// stays on its own background task) — the next poll cycle will pick up
// whatever the light's new actual state is. brightness_pct is clamped to
// WiZ's practical range [10, 100].
void wiz_set_power(const std::string &ip, uint16_t udp_port, bool on);
void wiz_set_brightness(const std::string &ip, uint16_t udp_port, uint8_t brightness_pct);
void wiz_set_color(const std::string &ip, uint16_t udp_port, uint8_t r, uint8_t g, uint8_t b);
// scene_id is 1-32, matching WiZ's built-in scene list (see kSceneNames in
// page_smart_lights.cpp).
void wiz_set_scene(const std::string &ip, uint16_t udp_port, uint8_t scene_id);
