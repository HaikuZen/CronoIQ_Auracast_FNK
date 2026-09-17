# CronoIQ

A smart clock for the Freenove FNK0115 (ESP32-S3, 800×480 RGB IPS panel +
GT911 capacitive touch), built on ESP-IDF + LVGL v9. Configuration lives on
an SD card, so Wi-Fi credentials, timezone, and weather provider settings
never need a firmware rebuild.

The UI is a top tab bar with three pages:

- **Clock** — weekday (row 1), date (row 2), a 7-segment-style `HH:MM:ss`
  clock in the middle, a Wi-Fi/NTP status row at the bottom, and the current
  weather condition icon in the top-right corner.
- **Weather** — current conditions header (icon, temperature, conditions,
  location) plus a horizontally scrollable row of forecast day cards, using
  [Visual Crossing](https://www.visualcrossing.com/) as the first supported
  provider.
- **Smart Lights** — a status card per configured light (name, on/off,
  brightness, an approximate color swatch), polled over UDP, using
  [WiZ](https://www.wizconnected.com/) as the first supported brand. Tap a
  card to select it and open a control panel below: a power switch, a
  brightness slider, a dropdown of WiZ's 32 built-in dynamic scenes (Ocean,
  Party, Fireplace, ...), and a row of color presets. Controls are
  brand-aware — a light whose brand isn't implemented yet still shows its
  status card, but selecting it shows a "no controls available" message
  instead of the WiZ panel.

## Hardware

- Freenove FNK0115L_4_3_IPS (or FNK0115Q_5_0_IPS) — ESP32-S3, 800×480 RGB
  panel, GT911 touch. Selected in `main/TFT_Config.h`.
- A micro-SD / TF card slot wired over SPI at `MISO=13 MOSI=11 SCLK=12
  CS=10` (see `main/sd_card.h`) — these were originally a guess (the four
  GPIOs the FNK0115 bring-up guide leaves unused by the panel/touch/
  backlight) but have since been **confirmed working on physical
  FNK0115L_4_3_IPS hardware** (card mounts, `config_crono.json` is read
  correctly). If your specific unit's card slot is wired differently, update
  `main/sd_card.h`.

## Building

Requires ESP-IDF v5.3+ (developed against v6.1) targeting `esp32s3`.

```sh
source $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

`idf.py build` succeeds against ESP-IDF v6.1 (0 warnings in this project's
own sources; the binary is ~1.57MB, comfortably inside the 4MB app
partition — see `partitions.csv`). It follows the verified board bring-up
pattern from `Agent_FNK0115_new_ESP-IDF.md` exactly for the display/touch
portion, and has been **confirmed working end-to-end on physical
FNK0115L_4_3_IPS hardware**: panel colors correct, touch switches tabs, SD
card mounts and `config_crono.json` is read, Wi-Fi connects, NTP syncs, and
the Visual Crossing forecast renders on both pages. If you hit a build
error on a different ESP-IDF/LVGL/esp_lvgl_port version combination, see
`CLAUDE.md`'s "Build / flash" section for the version-sensitive spots and
the issues already found and fixed there.

## Configuration — `config_crono.json`

Copy `sdcard/config_crono.json` onto the root of a FAT32-formatted SD card,
fill in your values, and insert it before powering the board. If the card is
missing/unmounted or the file is absent/malformed, the app logs the reason
and boots anyway with empty defaults (clock works off NTP-less local time,
Wi-Fi and weather stay off) rather than refusing to start.

```json
{
  "wifi": {
    "ssid": "YOUR_WIFI_SSID",
    "password": "YOUR_WIFI_PASSWORD"
  },
  "ntp": {
    "server": "pool.ntp.org",
    "timezone": "CET-1CEST,M3.5.0,M10.5.0/3"
  },
  "weather": {
    "provider": "visualcrossing",
    "api_key": "YOUR_VISUALCROSSING_API_KEY",
    "units": "metric",
    "forecast_days": 5,
    "update_interval_min": 30,
    "location": {
      "name": "Rome, IT",
      "latitude": 41.9028,
      "longitude": 12.4964
    }
  },
  "display": {
    "brightness_pct": 100
  },
  "smart_lights": [
    {
      "name": "Living Room",
      "brand": "WiZ",
      "ip": "192.168.1.50",
      "udp_port": 38899
    },
    {
      "name": "Bedroom",
      "brand": "WiZ",
      "ip": "192.168.1.51",
      "udp_port": 38899
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `wifi.ssid` / `wifi.password` | Station credentials. Leave `password` empty for an open network. |
| `ntp.server` | NTP pool/host used for time sync. |
| `ntp.timezone` | POSIX TZ string (handles DST). [List of examples](https://developer.arm.com/documentation/dui0378/g/timezone-and-locale/mapping-and-conversion-tables). |
| `weather.provider` | Only `"visualcrossing"` is implemented in this release; the field exists so other providers can be added later. |
| `weather.api_key` | Visual Crossing API key ([free tier available](https://www.visualcrossing.com/weather-api)). |
| `weather.units` | `"metric"` (°C) or `"us"` (°F). |
| `weather.forecast_days` | How many forecast day cards to show (1–15). |
| `weather.update_interval_min` | Minutes between weather refreshes. |
| `weather.location.name` | Freeform label shown on the Weather page. |
| `weather.location.latitude` / `longitude` | Decimal degrees, passed straight to the API. |
| `display.brightness_pct` | Reserved for future backlight-dimming support (currently informational only — backlight is driven full-on). |
| `smart_lights[].name` | Freeform label shown on the Smart Lights page. |
| `smart_lights[].brand` | Only `"WiZ"` is implemented in this release; other values are still shown on the page, always reported as offline. |
| `smart_lights[].ip` | The light's IP address on your LAN (a static/reserved DHCP lease is strongly recommended — there's no discovery, this is used directly). |
| `smart_lights[].udp_port` | UDP port the light listens on; WiZ's default is `38899`. |

## Project layout

```
main/
  TFT_Config.h, board.h, lvgl_display.{h,cpp}  — FNK0115 panel/touch/LVGL bring-up
  sd_card.{h,cpp}                              — SD card mount (SPI/FAT)
  app_config.{h,cpp}                           — config_crono.json parsing (cJSON)
  wifi_manager.{h,cpp}                         — Wi-Fi STA + reconnect
  time_manager.{h,cpp}                         — SNTP + local time formatting
  weather_service.{h,cpp}                      — Visual Crossing HTTP client + JSON parsing
  smart_lights_service.{h,cpp}                 — WiZ UDP polling + JSON parsing
  main.cpp                                     — app_main: wires everything together
  ui/
    seven_segment.{h,cpp}      — LED-style 7-segment digit/colon widget (no font assets)
    weather_icons.{h,cpp}      — vector-drawn weather icons (no image assets)
    page_clock.{h,cpp}         — Clock page
    page_weather.{h,cpp}       — Weather page
    page_smart_lights.{h,cpp}  — Smart Lights page
    ui_manager.{h,cpp}         — top tabview, ties pages + 1Hz clock tick together
sdcard/config_crono.json    — example config to copy onto the SD card
partitions.csv              — custom partition table (4MB app, no OTA)
```

## Design notes

- **7-segment clock**: rendered by composing plain LVGL rectangles into the
  classic 7-segment layout per digit (`main/ui/seven_segment.cpp`) rather
  than requiring a bundled 7-segment font — unlit segments stay visible at a
  dim shade for the authentic "powered-off LED" look.
- **Weather icons**: likewise built from LVGL primitives (circles/rounded
  rects) rather than a bundled icon font/image set, classified from Visual
  Crossing's `icon` field into sun/moon/cloud/rain/snow/thunder/fog/wind
  shapes. The moon-crescent icon "cuts out" a circle painted in the shared
  app background color (`WEATHER_ICON_BG_HEX` in `weather_icons.h`) — every
  page must use that same background for the illusion to hold.
- **Threading**: the clock page refreshes once a second via an `lv_timer`
  (runs on the LVGL task, no locking needed). Weather fetches and smart
  light polling each run on their own separate FreeRTOS task and push
  updates into the UI through `ui_manager_update_weather` /
  `ui_manager_update_smart_lights`, which take the `esp_lvgl_port` lock
  themselves.
- Extending to another weather provider: add a case in `app_config.cpp`'s
  provider handling and a new fetch/parse path in `weather_service.cpp`
  behind `cfg.provider`; `WeatherData` is provider-agnostic so the UI layer
  needs no changes.
- **Smart Lights / WiZ**: status is polled every 5 seconds over UDP (plain
  `lwip/sockets.h` BSD sockets, no extra component needed — `lwip` is
  already a build dependency). WiZ's `getPilot` request/response is a
  small unauthenticated JSON protocol; no discovery is implemented, so each
  light's IP must be given directly in config (a static/reserved DHCP lease
  is recommended). Extending to another brand follows the same
  provider-agnostic pattern as weather — see `CLAUDE.md`.
- **Smart Lights controls**: tapping a card sends `wiz_set_power` /
  `wiz_set_brightness` / `wiz_set_color` / `wiz_set_scene`
  (`smart_lights_service.cpp`) — a single fire-and-forget UDP send with no
  reply wait, safe to call directly from the LVGL event callback since it's
  not the multi-step round trip the status poll is. The UI doesn't
  optimistically show the new state; it waits for the next 5s poll to
  confirm what actually happened, which is simpler and more honest than
  guessing.
- **WiZ scenes**: the scene dropdown lists WiZ's 32 built-in dynamic scenes
  (`kSceneNames` in `page_smart_lights.cpp`), in the well-known
  scene-ID-order shared by WiZ's own app and third-party integrations —
  option index == scene ID, with index 0 a "Color / Custom" sentinel
  (never sent) representing "not currently running a scene". The dropdown
  reflects the light's actual `sceneId` from its last poll (0 when it's in
  plain color/CCT mode), so it stays in sync with changes made from
  elsewhere (the WiZ app, etc.), not just from this device.

## Known gaps / next steps

- `display.brightness_pct` is parsed but not yet wired to a PWM-dimmed
  backlight (the FNK0115 bring-up drives `TFT_BL` as a plain digital pin).
- Only Visual Crossing is implemented; other providers are stubbed at the
  config schema level only.
- Only WiZ is implemented for Smart Lights; other brands (e.g. Philips Hue,
  which needs a bridge and auth rather than direct UDP) are stubbed at the
  config schema level only, always show as offline, and show a "no
  controls available" message instead of the control panel when selected.
- The SD card SPI pinout and the full clock+weather flow are confirmed on
  one physical FNK0115L_4_3_IPS unit; if you're on the 5.0" variant
  (FNK0115Q_5_0_IPS) or a different card slot wiring, re-verify rather than
  assume. **The Smart Lights feature — status poll, power/brightness/color
  controls, and scene selection — has not yet been tested against a
  real WiZ bulb**: the `getPilot`/`setPilot` request/response shapes and
  the scene-ID-to-name mapping follow WiZ's publicly documented protocol,
  but haven't been confirmed against
  actual hardware the way the rest of this project has.
- **Reported crash, fixed but not yet re-confirmed**: swiping used to reset
  the board with `A stack overflow in task taskLVGL has been detected.`
  The LVGL render task's stack (`lvgl_display.cpp`, `lvgl_port_setup()`)
  was raised from `6144` to `16384` bytes — the Smart Lights page's
  dropdown/sliders/nested scrolling almost certainly pushed a swipe's
  gesture handling past the old, guide-demo-sized stack. See `CLAUDE.md`'s
  "Build / flash" section for the reasoning; please confirm swiping no
  longer crashes after reflashing.
