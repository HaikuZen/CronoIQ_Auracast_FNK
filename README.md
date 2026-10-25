# CronoIQ

A smart clock for the Freenove FNK0115 (ESP32-S3, 800×480 RGB IPS panel +
GT911 capacitive touch), built on ESP-IDF + LVGL v9. Configuration lives on
an SD card, so Wi-Fi credentials, timezone, and weather provider settings
never need a firmware rebuild.

The UI is a top tab bar with four pages:

- **Clock** — weekday (row 1), date (row 2), a 7-segment-style `HH:MM:ss`
  clock in the middle, a Wi-Fi/NTP status row at the bottom, and the current
  weather condition icon in the top-right corner.
- **Weather** — current conditions header (icon, temperature, conditions,
  location) plus a horizontally scrollable row of forecast day cards, using
  [Visual Crossing](https://www.visualcrossing.com/) as the first supported
  provider. Tap a day card to drill into that day's hour-by-hour forecast
  (time, icon, temperature); tap "< Back" to return to the day list.
- **Smart Lights** — a status card per configured light (name, on/off,
  brightness, an approximate color swatch), polled over UDP, using
  [WiZ](https://www.wizconnected.com/) as the first supported brand. Tap a
  card to select it and open a control panel below: a power switch, a
  brightness slider, a dropdown of WiZ's 32 built-in dynamic scenes (Ocean,
  Party, Fireplace, ...), and a row of color presets. Controls are
  brand-aware — a light whose brand isn't implemented yet still shows its
  status card, but selecting it shows a "no controls available" message
  instead of the WiZ panel.
- **Indoor** — readings from an optional Bosch **BME690** sensor processed by
  Bosch's BSEC library: an air-quality card (IAQ index 0-500, its category
  from "Excellent" to "Extremely polluted", a colored bar, and BSEC's
  calibration status), plus tiles for temperature, humidity, pressure, CO2
  equivalent, TVOC equivalent and gas resistance.
  - **History:** a **Now | History** switch at the top of the page shows a
    line graph of temperature, humidity, pressure, IAQ or CO2 over the last
    **3 hours** (1-minute points) or **24 hours** (5-minute points), with
    the current, minimum and maximum values.
  - IAQ and CO2 are recorded only once BSEC's calibration has started
    (accuracy ≥ 1).
  - History is kept in RAM only and starts empty after each reboot.

The **⚙ button in the Clock page's bottom-left corner** opens a full-screen
**Settings** screen, which isn't one of the tabs. It has:
- Wi-Fi network and password, typed on an on-screen keyboard
- timezone, from a list of common zones
- weather units and refresh interval
- the indoor sensor's on/off switch and temperature offset
- an Info section: firmware version, IP address, where the config came
  from, free memory and uptime

**Save & restart** stores the changes and reboots to apply them.

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
- Optional: a **BME690** breakout, sharing the GT911 touch controller's I2C
  bus. The bus runs on IO19/IO20, which the board breaks out on connector
  **P4**. P4 has no power pins, so take power from **P5**:

  | BME690 | FNK0115 |
  |---|---|
  | VCC | P5 pin 2 (3.3V) |
  | GND | P5 pin 1 (GND) |
  | SDA | P4 pin 3 (IO19) |
  | SCL | P4 pin 4 (IO20) |
  | SDO | GND → address `0x76`, or 3.3V → `0x77` |
  | CS | 3.3V (I2C mode; most breakouts already do this) |

  Pin 1 is the one next to the ▲ on the silkscreen. **Don't** use P5's
  IO17/IO18 as an I2C port: IO18 is the GT911's interrupt line. The bus
  only has the ESP32's weak internal pull-ups, so prefer a breakout with
  its own 4.7-10kΩ pull-ups and keep the leads short. The sensor is
  auto-detected at boot and re-probed every 10s if it's missing, so it can
  also be plugged in later.

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
card mounts and `config_crono.json` is read, Wi-Fi connects, NTP syncs, the
Visual Crossing forecast renders on both pages, and tapping a day card on
the Weather page loads and shows that day's hourly forecast. If you hit a build
error on a different ESP-IDF/LVGL/esp_lvgl_port version combination, see
`CLAUDE.md`'s "Build / flash" section for the version-sensitive spots and
the issues already found and fixed there.

### Bosch BSEC (for the BME690)

The BME690's air-quality output needs Bosch's proprietary BSEC 3.x library,
which this repo **does not include**: Bosch's license doesn't allow
redistributing it. Download it from Bosch Sensortec, unzip it, and point the
build at the folder that contains `release_bin/` and `examples/`. The build
looks for it in this order:

1. `idf.py -DBSEC_DIR=/path/to/bsec_v3 build`
2. the `BSEC_DIR` environment variable
3. `../bsec_v3` next to this project's parent directory, e.g.
   `~/Scaricati/bsec_v3` for a checkout at `~/Scaricati/CYD/CronoIQ_Auracast_FNK`

`components/bsec/CMakeLists.txt` takes `libalgobsec.a` for the ESP32-S3,
the BME690 IAQ config `bme690_iaq_33v_3s_4d` (3.3V supply, 3s low-power
sampling, 4-day calibration window) and the BSD-licensed `bme69x` driver
from that folder. If the folder isn't found, the build prints a CMake
warning and still succeeds, and the Indoor page says the firmware was built
without BSEC.

## Configuration — `config_crono.json`

Copy `sdcard/config_crono.json` onto the root of a FAT32-formatted SD card,
fill in your values, and insert it before powering the board.

**The SD card and a copy in internal flash are kept in sync, and whichever
was changed last wins.** The flash copy lives on a LittleFS partition at
`/flash/config_crono.json`. At every boot:

1. **Card edited on a PC since the last sync:** the card wins. It's used,
   and copied to flash.
2. **Card unchanged since the last sync:** the flash copy wins, because it
   may hold changes made on the Settings screen while the card was out.
   It's written back to the card if the two differ.
3. **No card, or its file is missing or malformed:** the last flash copy
   is used, so the board keeps its Wi-Fi, weather, lights and sensor
   settings without a card.
4. **No flash copy either** (a card has never been read): the board boots
   with empty defaults. The clock runs on unsynced local time, and Wi-Fi
   and weather stay off.

The Settings screen saves to flash, and to the card too if one is inserted.
It only changes the keys it edits, so everything else in the file (weather
API key, location, lights, and so on) is kept. A malformed file on the card
is never copied, so a broken edit can't overwrite the working copy in
flash.

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
    "update_interval_min": 60,
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
  ],
  "env_sensor": {
    "enabled": true,
    "i2c_address": "auto",
    "temp_offset_c": 0.15
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
| `weather.forecast_days` | How many forecast day cards (and days of hourly drill-down data) to fetch and show (1–15). Directly drives API cost — see below. |
| `weather.update_interval_min` | Minutes between weather refreshes. Directly drives API cost — see below. |
| `weather.location.name` | Freeform label shown on the Weather page. |
| `weather.location.latitude` / `longitude` | Decimal degrees, passed straight to the API. |
| `display.brightness_pct` | Reserved for future backlight-dimming support (currently informational only — backlight is driven full-on). |
| `smart_lights[].name` | Freeform label shown on the Smart Lights page. |
| `smart_lights[].brand` | Only `"WiZ"` is implemented in this release; other values are still shown on the page, always reported as offline. |
| `smart_lights[].ip` | The light's IP address on your LAN (a static/reserved DHCP lease is strongly recommended — there's no discovery, this is used directly). |
| `smart_lights[].udp_port` | UDP port the light listens on; WiZ's default is `38899`. |
| `env_sensor.enabled` | `false` skips probing for a BME690. Optional; defaults to `true`. |
| `env_sensor.i2c_address` | `"0x76"`, `"0x77"` (or `118`/`119`), or anything else such as `"auto"` to probe both. Optional; defaults to auto. |
| `env_sensor.temp_offset_c` | °C subtracted from the sensor's temperature to make up for heat from the board and display; humidity is corrected to match. The default `0.15` is Bosch's figure for a bare sensor. A sensor mounted close to the FNK0115 will read high, so compare it against a reference thermometer and raise this. |

### Visual Crossing API cost

Visual Crossing bills by **records returned**, not by call count. Per
[their own costing example](https://www.visualcrossing.com/resources/documentation/weather-api/timeline-weather-api/),
a single day with both daily and hourly detail costs **24 records**, while
daily-only detail costs **1 record**. The periodic poll (clock icon +
Weather page's day-card list) only ever requests daily detail — hourly
detail is fetched separately, on demand, only for the one day you actually
tap into on the Weather page. That keeps the recurring cost small and
bounded by `forecast_days` alone:

```
records per periodic poll ≈ forecast_days
records per day (periodic) ≈ forecast_days × (1440 / update_interval_min)
records per day tapped     ≈ 24  (once per tap, not repeated while that
                                   day stays cached — see below)
```

| `forecast_days` | `update_interval_min` | ≈ records/day (periodic only) |
|---|---|---|
| 5 | 60  | 120 |
| 5 | 180 | 40  |
| 10 | 60 | 240 |

Even a busy day of tapping through every forecast day's hourly view a few
times adds only double-digit-to-low-hundreds of records on top of that —
nowhere near the ~24×`forecast_days`-per-poll cost the always-fetch-hourly
approach had. The shipped defaults (`forecast_days: 5`,
`update_interval_min: 60`) now sit comfortably under a typical 1,000
records/day free-tier budget; raising `update_interval_min` or lowering
`forecast_days` still helps if you're on a tighter plan, but neither is
required the way it was before.

A tapped day's hours are cached in memory only until the next periodic
poll replaces the forecast list (`update_interval_min` minutes later) or
you power-cycle the device — re-tapping the same day again within that
window doesn't re-fetch it.

## Project layout

```
main/
  TFT_Config.h, board.h, lvgl_display.{h,cpp}  — FNK0115 panel/touch/LVGL bring-up
  sd_card.{h,cpp}                              — SD card mount (SPI/FAT)
  app_config.{h,cpp}                           — config_crono.json parsing (cJSON)
  config_store.{h,cpp}                         — SD ↔ flash (LittleFS) config sync, fallback, saving
  wifi_manager.{h,cpp}                         — Wi-Fi STA + reconnect
  time_manager.{h,cpp}                         — SNTP + local time formatting
  weather_service.{h,cpp}                      — Visual Crossing HTTP client + JSON parsing
  smart_lights_service.{h,cpp}                 — WiZ UDP polling + JSON parsing
  env_sensor_service.{h,cpp}                   — BME690 on the shared I2C bus + Bosch BSEC
  env_history.{h,cpp}                          — 3h/24h in-RAM history of sensor readings
  main.cpp                                     — app_main: wires everything together
  ui/
    seven_segment.{h,cpp}      — LED-style 7-segment digit/colon widget (no font assets)
    weather_icons.{h,cpp}      — vector-drawn weather icons (no image assets)
    page_clock.{h,cpp}         — Clock page
    page_weather.{h,cpp}       — Weather page
    page_smart_lights.{h,cpp}  — Smart Lights page
    page_environment.{h,cpp}   — Indoor page: Now (readings) / History (graphs)
    page_settings.{h,cpp}      — Settings overlay (opened from the Clock page's ⚙ button)
    ui_manager.{h,cpp}         — top tabview, ties pages + 1Hz clock tick together
components/bsec/            — wraps an external Bosch BSEC download (not in the repo)
sdcard/config_crono.json    — example config to copy onto the SD card
partitions.csv              — custom partition table (4MB app, no OTA, 1MB LittleFS "storage")
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
- **Hourly forecast drill-down, fetched on demand**: the periodic poll
  (`weather_service.cpp`'s `build_periodic_url()`) only requests
  `include=days,current` — cheap, and anchored to an explicit
  `[today, today+forecast_days-1]` date range rather than Visual Crossing's
  open-ended ~15-day default. That date range needs a synced clock, so
  `weather_task()` waits for `time_manager_is_synced()` before ever
  building or sending a periodic request — there is no costlier undated
  fallback for the unsynced case; a slow/stuck NTP sync just defers the
  weather poll a couple seconds at a time instead of firing off repeated
  expensive requests while waiting. Tapping a day card on the Weather page
  triggers a separate
  one-shot fetch (`weather_service_fetch_hours()`) scoped to just that one
  date with `include=days,hours` — the cheapest request that gets the
  drill-down what it needs, and it only happens when you actually look.
  `page_weather.cpp`'s `rebuild_forecast_view()` doesn't open a new page
  for the hourly view; it swaps the same fixed-size forecast area between
  the day-card grid and a "< Back" row + horizontally-scrollable hour-card
  row (with loading/failed states while the on-demand fetch is in flight)
  for the tapped day, so no other layout on the page has to move. See
  "Visual Crossing API cost" below for what this means for `forecast_days`
  and `update_interval_min`.
- **At most one weather HTTP/TLS request runs at a time.** A module-level
  guard in `weather_service.cpp`'s `fetch_raw()` makes the periodic poll
  and an on-demand hourly fetch mutually exclusive — if the user taps a day
  right as the periodic poll fires, one of the two is simply skipped and
  retried (the periodic one on its next interval; the on-demand one reports
  failure and the UI lets the user retry) rather than both running
  simultaneously. This exists because two overlapping mbedTLS/HTTPS
  requests competing for internal RAM is a plausible contributor to an
  observed `ESP_ERR_NO_MEM` crash during Wi-Fi PHY re-enable shortly after
  a weather fetch — see `CLAUDE.md` for the crash detail. Removing hourly
  data from the periodic poll (above) independently shrinks that same
  request's memory footprint, so between the two this crash is less likely
  than before, but **it has not been specifically re-tested against the
  exact conditions that produced it.**
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

## Troubleshooting

### Clock page shows "NTP: not synced" for many minutes

One real-world cause has already been found, fixed, and **confirmed
resolved on hardware**: lwIP's own hostname-based DNS resolution for the
SNTP server was observed never succeeding on a device that could otherwise
reach the network fine (confirmed with `ntpdate -q <server>` from another
host on the same network while the device sat stuck). `time_manager.cpp`
now resolves the NTP hostname itself and configures SNTP with the literal
IP address instead, sidestepping that internal resolution path entirely.
If you still hit this on a different unit, reflash first and check whether
it's already resolved before assuming it's a new problem.

If it still doesn't sync after reflashing, `time_manager.cpp` logs a
diagnostic every minute for the first 10 minutes (then every 10 minutes)
while unsynced, including a DNS lookup of the configured `ntp.server`:

- **`Pre-resolved NTP server "..." to <ip>`** at startup, but sync still
  never lands — DNS is fine (it resolved), so the address itself is
  probably unreachable specifically for NTP. The periodic diagnostic
  re-resolves and retries with a fresh address each check (`pool.ntp.org`
  round-robins across many independent real servers), but if it's still
  stuck after several such retries, suspect outbound **UDP port 123 being
  blocked** by your router or firewall — check your router's
  firewall/outbound rules for UDP/123, or point `ntp.server` at your
  router's own NTP service if it has one.
- **`Could not pre-resolve NTP server "..."` / `DNS lookup for "..."
  FAILED`** — a DNS problem, not NTP-specific. Check the DNS server your
  router/DHCP hands out to the device.

This is a device/network configuration problem in both remaining cases,
not something a firmware update can work around.

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
- **The on-demand hourly forecast fetch is confirmed working on physical
  hardware**, including the fix for a hang that used to trip the ESP-IDF
  task watchdog repeatedly (rendering 24 hour-cards with the full vector
  weather icon — up to ~288 LVGL objects in one synchronous burst while
  holding the UI lock). Tapping a day card now fetches, parses, and
  renders that day's hours (using lightweight color-dot icons for the
  hourly row instead of the full vector icon) without hanging. Not
  specifically exercised yet: a fetch attempted on a dead Wi-Fi connection
  (should report "Couldn't load..." rather than hanging — the code path
  exists but hasn't been triggered on real hardware), and tapping a day
  card at the exact moment the periodic poll fires (the mutual-exclusion
  guard in `fetch_raw()` should make one of the two simply retry).
- **The BME690 is partly confirmed on hardware.** On the physical unit it is
  auto-detected on the shared bus (`BME69x found at 0x77`), BSEC starts,
  and plausible readings are logged within seconds. Still to check:
  the Indoor page itself, touch responsiveness with the sensor attached, IAQ accuracy goes from 0 up to 3
  over the first hours or days, and after a reboot the restored calibration
  (`Restored BSEC calibration state`) brings accuracy back quickly. Expect
  the IAQ value to sit at about 50, dimmed, while accuracy is 0; that's how
  BSEC behaves before calibration, not a fault.
- See "Visual Crossing API cost" above for the (now much smaller) ongoing
  cost — the periodic poll alone comfortably fits a typical 1,000
  records/day free-tier budget at the shipped defaults; heavy manual
  tapping through hourly views adds to that but nowhere near as much as
  the old always-fetch-hourly design did.
