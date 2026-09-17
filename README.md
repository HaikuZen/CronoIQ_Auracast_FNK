# CronoIQ

A smart clock for the Freenove FNK0115 (ESP32-S3, 800×480 RGB IPS panel +
GT911 capacitive touch), built on ESP-IDF + LVGL v9. Configuration lives on
an SD card, so Wi-Fi credentials, timezone, and weather provider settings
never need a firmware rebuild.

The UI is a top tab bar with two pages:

- **Clock** — weekday (row 1), date (row 2), a 7-segment-style `HH:MM:ss`
  clock in the middle, a Wi-Fi/NTP status row at the bottom, and the current
  weather condition icon in the top-right corner.
- **Weather** — current conditions header (icon, temperature, conditions,
  location) plus a horizontally scrollable row of forecast day cards, using
  [Visual Crossing](https://www.visualcrossing.com/) as the first supported
  provider.

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
  }
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

## Project layout

```
main/
  TFT_Config.h, board.h, lvgl_display.{h,cpp}  — FNK0115 panel/touch/LVGL bring-up
  sd_card.{h,cpp}                              — SD card mount (SPI/FAT)
  app_config.{h,cpp}                           — config_crono.json parsing (cJSON)
  wifi_manager.{h,cpp}                         — Wi-Fi STA + reconnect
  time_manager.{h,cpp}                         — SNTP + local time formatting
  weather_service.{h,cpp}                      — Visual Crossing HTTP client + JSON parsing
  main.cpp                                     — app_main: wires everything together
  ui/
    seven_segment.{h,cpp}   — LED-style 7-segment digit/colon widget (no font assets)
    weather_icons.{h,cpp}   — vector-drawn weather icons (no image assets)
    page_clock.{h,cpp}      — Clock page
    page_weather.{h,cpp}    — Weather page
    ui_manager.{h,cpp}      — top tabview, ties pages + 1Hz clock tick together
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
  (runs on the LVGL task, no locking needed). Weather fetches run on a
  separate FreeRTOS task and push updates into the UI through
  `ui_manager_update_weather`, which takes the `esp_lvgl_port` lock itself.
- Extending to another weather provider: add a case in `app_config.cpp`'s
  provider handling and a new fetch/parse path in `weather_service.cpp`
  behind `cfg.provider`; `WeatherData` is provider-agnostic so the UI layer
  needs no changes.

## Known gaps / next steps

- `display.brightness_pct` is parsed but not yet wired to a PWM-dimmed
  backlight (the FNK0115 bring-up drives `TFT_BL` as a plain digital pin).
- Only Visual Crossing is implemented; other providers are stubbed at the
  config schema level only.
- The SD card SPI pinout and the full clock+weather flow are confirmed on
  one physical FNK0115L_4_3_IPS unit; if you're on the 5.0" variant
  (FNK0115Q_5_0_IPS) or a different card slot wiring, re-verify rather than
  assume.
