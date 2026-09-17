# CLAUDE.md

Guidance for Claude Code (or any agent) working in this repository.

## What this is

CronoIQ: an ESP-IDF (v5.3+, developed against v6.1) + LVGL v9 smart-clock
firmware for the Freenove FNK0115 (ESP32-S3, 800×480 RGB panel, GT911
touch). Two tab pages: Clock (weekday/date/7-segment time/Wi-Fi+NTP status,
plus a small current-weather icon) and Weather (current conditions +
forecast cards). Configuration (Wi-Fi, NTP, weather provider/location) is
read from `/sdcard/config_crono.json` at boot — see `README.md` for the
schema.

`Agent_FNK0115_new_ESP-IDF.md` in the repo root is the source bring-up
recipe this project's display/touch layer (`main/lvgl_display.{h,cpp}`,
`main/board.h`, `main/TFT_Config.h`) was copied from verbatim as a
known-working pattern. **Do not "clean up" or restructure that file without
re-reading the guide** — its exact struct field ordering/values (RGB panel
timings, `direct_mode` + `avoid_tearing` pairing, GT911 retry loop) are
there because they were previously proven necessary, not incidental style.

One deliberate deviation from the guide's copy: `touch_init()` in
`lvgl_display.cpp` sets `tp_cfg.driver_data` to a
`static esp_lcd_touch_io_gt911_config_t { .dev_addr =
ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS }`, which the guide's original omitted.
Without it, `esp_lcd_touch_new_i2c_gt911()` skips its proper I2C
address-selection reset sequence entirely (logs "I2C address
initialization procedure skipped") and the address the GT911 actually
latches at power-on is left undetermined instead of being driven
deterministically to `0x5D` — a very plausible cause of repeated
`i2c transaction failed` / GT911 read errors seen at boot on real hardware
(all 10 retry attempts failing, not just an occasional flaky one).
**Confirmed fixed on hardware**: after this change, GT911 now initializes
on the *first* attempt (`TouchPad_ID:0x39,0x31,0x31`, no retries, no
"skipped" log line), where before it failed all 10 retries every boot.
Expect a benign `W (...) gpio: conflict found for GPIO[18]` warning right
after — `GPIO[18]` is `TOUCH_INT`, and the address-selection reset dance
legitimately reconfigures it twice (briefly as an output to drive the
address-select line, then as an input for the touch interrupt); ESP-IDF's
GPIO reservation tracking logs that reuse as a "conflict" even though it
isn't one. Don't chase that warning as a bug.

This root cause and fix apply to any ESP-IDF project using
`esp_lcd_touch_gt911`, not just this one — see `Agent_GT911.md` in the repo
root for the portable, project-agnostic writeup (symptom log, fix, why it
works, and a troubleshooting checklist for when it isn't the whole story).

## Build / flash

```sh
source $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

`idf.py build` has been run clean against ESP-IDF v6.1 (target `esp32s3`) —
zero errors, zero warnings in this project's own sources (the
`esp_wifi`/`wpa_supplicant` `CMake Warning`s about private include dirs are
pre-existing upstream IDF noise, not ours; ignore them). It has also been
**flashed and confirmed working on a physical FNK0115L_4_3_IPS**: panel
colors correct, touch switches tabs, SD card mounts and
`config_crono.json` is read (confirming the `main/sd_card.h` SPI pin guess
was right for this unit), Wi-Fi connects, NTP syncs, and Visual Crossing's
forecast renders on both pages. If the build breaks on a different ESP-IDF
version, treat any struct-field or API-signature mismatch as expected
friction, not a sign the architecture is wrong. Three real issues were hit
and fixed while getting to a clean build — know these before re-deriving
them from scratch:

- **cJSON is not a built-in IDF component in v6.1+** — it was pulled out
  into the managed component `espressif/cjson` (component registry). It's
  declared in `main/idf_component.yml`; do **not** add `json` (or `cjson`)
  to `main/CMakeLists.txt`'s `REQUIRES` — managed components declared in a
  component's own `idf_component.yml` are automatically available to that
  component, and `json` isn't a resolvable component name at all anymore
  (this is exactly what broke first: `Failed to resolve component 'json'`).
- **Designated initializers + `-Werror=missing-field-initializers`**:
  ESP-IDF's own struct types gain fields across versions, and a
  partial designated initializer (`{ .a = 1, .b = 2 }` while the struct
  has a third field `.c`) is a hard build error under this project's
  `-Wall -Werror`. `main/lvgl_display.cpp` handles this correctly (per the
  bring-up guide) with a scoped `#pragma GCC diagnostic ignored
  "-Wmissing-field-initializers"`. Everywhere else in this codebase, prefer
  the vendor's own `..._DEFAULT_CONFIG()`/`..._DEFAULT()` macro (see
  `main/sd_card.cpp`'s use of `VFS_FAT_MOUNT_DEFAULT_CONFIG()`) or an empty
  `= {}` and then set fields by name on separate lines — both survive a
  struct gaining new fields; a hand-written partial designated initializer
  does not.
- **The built-in `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE` partition
  (~1.5MB) is too small** for LVGL + Wi-Fi + mbedTLS + cJSON linked
  together (~1.57MB here, and this is only two pages of UI — it'll grow).
  This project uses a custom `partitions.csv` with a 4MB `factory` app
  partition instead (`CONFIG_PARTITION_TABLE_CUSTOM=y` +
  `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`/`CONFIG_PARTITION_TABLE_FILENAME`
  in `sdkconfig.defaults`), which the 16MB flash has ample room for. If you
  change `sdkconfig.defaults`, delete the generated `sdkconfig` and let
  `idf.py build` regenerate it — a stale `sdkconfig` from a previous run
  silently keeps the old partition choice.
- Also disabled: `CONFIG_LV_BUILD_EXAMPLES`/`CONFIG_LV_BUILD_DEMOS` (both
  default `y` in the managed `lvgl/lvgl` component and compile a large
  pile of example/demo widget sources this app never references — link-time
  `--gc-sections` strips them from the final binary either way, so this is
  a build-time win, not a flash-size one).

Two more things worth knowing even though they didn't cause a failure:

- `lv_coord_t` was avoided in `main/ui/*.{h,cpp}` in favor of `int32_t`
  specifically because its availability across LVGL 9.x point releases was
  uncertain when this was written — don't reintroduce it without checking
  the installed `lvgl/lvgl` version's `lv_types.h`.
- `main/CMakeLists.txt`'s `REQUIRES` list was hand-derived from which
  headers each `.cpp` directly includes (not copied from an example) —
  if you add a new direct `#include` of an ESP-IDF component header, add
  that component to `REQUIRES` too.

## Repo-specific conventions

- **C++ throughout the app layer** (not C) — matches the bring-up guide's
  choice; LVGL/esp_lcd/esp-idf C APIs are all C++-callable directly.
- **No bundled font or image assets.** The 7-segment clock
  (`main/ui/seven_segment.cpp`) and the weather icons
  (`main/ui/weather_icons.cpp`) are both drawn from plain LVGL rectangle/
  circle primitives on purpose, so the project has zero binary asset
  pipeline. If you're tempted to swap in a real 7-segment TTF or an icon
  font/spritesheet for higher fidelity, that's a reasonable upgrade — just
  know it's a deliberate absence, not an oversight.
- **Seven-segment digit segments deliberately overlap at every joint**
  (the `ov` offset in `create_digit()` in `seven_segment.cpp`). They used
  to be placed to exactly abut, which looked fine in the simulator but
  showed a visible 1-2px gap at every corner on real hardware — `int32_t`
  truncation in the `h/2`/`t/2`/`t*1.5f` layout math means the touching
  edges don't reliably land on the same pixel, and each segment's own
  rounded-corner radius compounds it into a visible notch. `ov` is sized to
  at least the corner radius so the overlap always fully hides both
  effects. If you touch this layout math, keep (or re-derive) that overlap
  — don't go back to exact abutment.
- **Page background color is a shared constant** —
  `WEATHER_ICON_BG_HEX` in `main/ui/weather_icons.h` (currently
  `0x0D1B2A`). The moon-crescent icon fakes its "cutout" by painting a
  circle in exactly this color, so every full-screen page must keep this
  same background or the crescent will show a visible seam. If you ever add
  a third page, set its background to this constant too.
- **The HTTP body accumulator in `weather_service.cpp`'s
  `http_event_handler` must never gate on `esp_http_client_is_chunked_response()`.**
  It did once (copied from an ESP-IDF example that uses a fixed
  Content-Length-sized buffer, where chunked responses can't be safely
  sized up front) and it silently dropped the entire body whenever Visual
  Crossing sent a chunked response — which it normally does, since a JSON
  forecast payload's length isn't known ahead of time. Symptom was exactly
  `Failed to parse weather JSON` with an empty body and no other clue. We
  append into a dynamically-growing `std::string`, so chunked vs.
  non-chunked makes no difference here — always append.
- **Weather fetch happens off the LVGL thread.** `weather_service.cpp` runs
  its own FreeRTOS task and calls back into `ui_manager_update_weather`,
  which takes the `esp_lvgl_port` lock itself
  (`lvgl_port_lock`/`lvgl_port_unlock`). Any new code that touches LVGL
  objects from outside the LVGL task (i.e. not from an `lv_timer` callback)
  must take that same lock — this is the one concurrency rule in the
  codebase that will crash/corrupt the UI if skipped.
- **NTP and weather both wait for Wi-Fi themselves, on their own
  background tasks.** `time_manager_start()` spawns a task that polls
  `wifi_manager_is_connected()` before calling `esp_netif_sntp_init()`;
  `weather_service_start()`'s task does the same before each fetch. This
  keeps `app_main()` non-blocking — `wifi_manager_start()`,
  `time_manager_start()`, and `weather_service_start()` can all be called
  back-to-back in `main.cpp` regardless of how long the Wi-Fi handshake
  takes. If you add another network-dependent service, follow the same
  pattern rather than assuming Wi-Fi is already up by the time your code
  runs.
- **Config parsing never hard-fails.** `app_config_load` logs and returns
  `false` on any problem (missing card, missing file, bad JSON, missing
  sub-object) and leaves `AppConfig` at its struct-default values; `main.cpp`
  boots regardless. Keep new config fields following that pattern —
  reasonable default + warning log, not an abort.
- **SD card SPI pins are an unverified assumption**
  (`main/sd_card.h`: MISO=13 MOSI=11 SCLK=12 CS=10, chosen because they're
  the only GPIOs the FNK0115 bring-up guide's panel/touch/backlight wiring
  leaves free). If you get real schematic/continuity info for the card
  slot, update that header and remove the caveat from `README.md`.

## Adding a second weather provider

`WeatherConfig.provider` and `WeatherData` are already provider-agnostic.
To add one:

1. Extend the `weather` object schema in `README.md` / `sdcard/config_crono.json`
   / `app_config.cpp` only if the new provider needs fields Visual Crossing
   doesn't have.
2. In `weather_service.cpp`, branch on `cfg.provider` to pick a different
   `build_url`/`parse_response` pair that both still produce a `WeatherData`.
3. Don't touch `main/ui/page_weather.cpp` or `page_clock.cpp` — they only
   ever see the normalized `WeatherData`/`WeatherDay` structs.

## Testing expectations

There is no unit test suite (this is a hardware-driven embedded app with no
host-side build target). "Testing" here means the physical bring-up
checklist in `Agent_FNK0115_new_ESP-IDF.md` §8 (panel colors, no tearing,
touch alignment) plus manually confirming: SD card mounts and
`config_crono.json` parses (check boot log), Wi-Fi connects, NTP syncs
(clock page status row goes green), and a weather fetch succeeds (weather
page populates, clock page's corner icon updates). All of the above has
been confirmed on one physical FNK0115L_4_3_IPS unit. If you change
anything in this list and can't re-run it against real hardware, say so
explicitly rather than claiming the feature still works.
