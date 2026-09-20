# CLAUDE.md

Guidance for Claude Code (or any agent) working in this repository.

## What this is

CronoIQ: an ESP-IDF (v5.3+, developed against v6.1) + LVGL v9 smart-clock
firmware for the Freenove FNK0115 (ESP32-S3, 800×480 RGB panel, GT911
touch). Three tab pages: Clock (weekday/date/7-segment time/Wi-Fi+NTP
status, plus a small current-weather icon), Weather (current conditions +
forecast cards; tap a day card to drill into that day's hourly forecast,
fetched on demand only for that one day, in the same space, "< Back" to
return), and Smart Lights (a status card per
configured light —
name, on/off, brightness, approximate color — polled over UDP, WiZ being
the first supported brand; tap a card to select it and open a control
panel below with a power switch, brightness slider, a dropdown of WiZ's 32
built-in dynamic scenes, and color presets, brand-aware so an unimplemented
brand shows a "no controls" message instead). Configuration (Wi-Fi, NTP,
weather provider/location, smart
light list) is read from `/sdcard/config_crono.json` at boot — see
`README.md` for the schema.

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

A second deviation, also a real hardware crash fix: `lvgl_port_setup()`'s
`task_stack` was raised from `6144` (the guide's own value, sized for its
trivial one-screen touch/coordinate demo) to `16384`. Reported symptom on
real hardware: swiping reset the board with
`***ERROR*** A stack overflow in task taskLVGL has been detected.` —
`taskLVGL` is exactly the name `esp_lvgl_port` gives this task
(`esp_lvgl_port.c`'s `xTaskCreateWithCaps(lvgl_port_task, "taskLVGL", ...)`),
so this is squarely a stack-too-small crash, not a display/panel/touch bug.
The Smart Lights page added since the guide's demo screen — an
`lv_dropdown` (builds an internal popup list + scrollbar with a heavier
layout pass), sliders, and multiple nested scrollable containers (the
light grid and the control panel both scroll) — is almost certainly what
pushed a swipe's gesture-propagation call chain past 6KB; even
`esp_lvgl_port`'s own `LVGL_PORT_INIT_CONFIG()` default is `7168`, already
above what this project had. **Not yet re-confirmed on hardware** — if a
swipe still crashes after reflashing with this change, the actual overflow
is deeper than assumed here and `task_stack` needs raising further (watch
`esp_lvgl_port`'s task in a stack high-water-mark check, or just double it
again), not reverted.

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
was right for this unit), Wi-Fi connects, NTP syncs, Visual Crossing's
forecast renders on both pages, and tapping a day card on the Weather page
fetches and renders that day's hourly forecast without hanging (see the
task-watchdog hang and its fix below — now confirmed resolved on
hardware). If the build breaks on a different ESP-IDF
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
- **`time_manager.cpp` pre-resolves the NTP hostname itself and configures
  SNTP with the literal IP address, rather than handing SNTP the hostname
  and letting it resolve internally.** This exists because lwIP's own
  hostname-based DNS resolution for the SNTP server (`dns_gethostbyname()`,
  called from inside `sntp_request()` in lwIP's `sntp.c`) was observed
  never succeeding on real hardware — NTP stuck on "not synced"
  indefinitely — **even though DNS and the network path both work fine
  otherwise**, confirmed independently by running `ntpdate -q <server>`
  from another host on the same network while the device sat stuck. The
  exact reason inside lwIP's resolution path was not isolated (would need
  a debugger on the actual failure, not just log inference), but the
  workaround sidesteps it entirely: `resolve_hostname()` in
  `time_manager.cpp` does its own `getaddrinfo()` (the same call our own
  code already used successfully for diagnostics) and passes the resulting
  IP string to `esp_netif_sntp_init()` instead of the hostname —
  `dns_gethostbyname()` recognizes a literal IP immediately, with no DNS
  query involved at all, so whatever was wrong with hostname resolution
  specifically can't matter anymore. **Lifetime gotcha if you touch this**:
  lwIP's `sntp_setservername()` stores only a *pointer* to the server
  string, never a copy, and keeps using that pointer indefinitely
  (including for the hourly re-sync) — that's why the resolved/hostname
  string is copied into the static `s_sntp_server_buf`, not passed as a
  local `std::string::c_str()`, which would dangle once `time_manager_task`
  moved on. If still unsynced after a periodic check (every 1 min for the
  first 10, then every 10 min), the task re-resolves (`pool.ntp.org`
  round-robins across many independent real servers — a different one may
  simply work) and does a full `esp_netif_sntp_deinit()` +
  re-`esp_netif_sntp_init()` with the fresh address, since a stuck SNTP
  client doesn't recover from a bad server on its own. **Confirmed fixed on
  hardware** — reasoned from the reported symptom (device stuck on "not
  synced" despite `ntpdate` proving the network path and DNS both work
  from another host) plus reading lwIP's actual SNTP source rather than a
  debugger attached to the live failure, but the fix itself has since been
  verified: NTP now syncs on the device that was previously stuck.
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
- **Visual Crossing bills by records returned, not by call count — this is
  why hourly data is fetched separately from the periodic poll, not
  bundled into it.** `weather_service.cpp` has two request builders:
  `build_periodic_url()` (`include=days,current` only — ~1 record/day,
  anchored to an explicit `[today, today+forecast_days-1]` date range
  rather than Visual Crossing's open-ended ~15-day default) for the
  recurring background poll, and `build_hourly_url()` (`include=days,hours`
  for one specific date — ~24 records) used only by
  `weather_service_fetch_hours()`, the on-demand fetch
  `page_weather.cpp` triggers when a day card is tapped. This used to be
  one request with `include=days,hours,current` fetched on every periodic
  poll regardless of user interaction — at ~24×`forecast_days`
  records/poll that blew past a typical free-tier 1,000/day budget by a
  wide margin (worse before the date range existed at all: an *undated*
  `include=days,hours` request returns Visual Crossing's full ~15-day
  default window, ~360 records/call, ~17,000/day at a 30min interval).
  Don't merge hourly back into the periodic request "for simplicity" —
  that's the exact regression this history describes. The periodic date
  range needs a roughly-correct local clock, so `build_periodic_url()` now
  has **no unsynced-time fallback at all** — it assumes
  `time_manager_is_synced()` is already true and will build a bogus range
  otherwise. `weather_task()` enforces that: it waits in a
  `while (!time_manager_is_synced())` loop (right after the existing
  Wi-Fi-connected wait, same 2s poll cadence) before ever calling
  `build_periodic_url()`. This replaced an earlier version that instead
  fell back to the costlier open-ended request whenever time wasn't synced
  yet — cheap for one poll, but on a device with a slow/stuck NTP sync
  (see the `time_manager.cpp` bullet below) that meant *many* expensive
  polls in a row instead of a few-second deferred wait. If you touch
  `build_periodic_url()`, don't reintroduce an unsynced-time branch inside
  it — add any new "wait for precondition X" logic to `weather_task()`'s
  wait chain instead, keeping the URL builder itself precondition-free.
  Both URL builders are called fresh each time they're needed (never
  cached), since the date(s) can change between calls.
- **`page_weather.cpp` caches a tapped day's hours in `s_forecast[i].hours`
  for the lifetime of that forecast list** (until the next periodic poll
  replaces `s_forecast` — `page_weather_update()` carries the currently
  *selected* day's cached hours across that replacement by matching
  `iso_date`, but every other day's cache is intentionally dropped, since
  keeping them all around gains little and this is exactly the kind of
  state a stale/late fetch reply could otherwise clobber). Re-tapping the
  same day without an intervening poll reuses the cache
  (`fetch_hours_for_selected_day()` no-ops if `hours` is already
  non-empty); tapping a different day, or the same day again after a poll,
  fetches fresh. `weather_service_fetch_hours()`'s callback runs on its own
  one-shot FreeRTOS task, not the LVGL task, and `page_weather.cpp` takes
  the `esp_lvgl_port` lock itself inside that callback before touching any
  LVGL object or page-scope state — this is the first page (not
  `ui_manager.cpp`) to do that lock-taking itself, because the callback is
  answering a request the page itself made, not something `ui_manager` is
  routing between a service and a page. The callback also checks that
  `s_forecast[s_selected_day].iso_date` still matches the date it fetched
  for before applying the result — a stale reply for a day the user has
  since backed out of, switched away from, or that a periodic refresh
  already replaced is silently dropped rather than corrupting whichever
  day is now selected.
- **At most one weather HTTP/TLS request runs at a time, enforced inside
  `fetch_raw()` itself** (a `std::atomic<bool> s_fetch_in_flight` +
  RAII guard), shared by the periodic poll and on-demand hourly fetches.
  This exists because a real `ESP_ERR_NO_MEM` crash was observed
  (`phy_track_pll_init`'s `esp_timer_create` failing during Wi-Fi PHY
  re-enable, right after a weather TLS fetch completed) — two overlapping
  mbedTLS/HTTPS requests competing for scarce internal RAM is a plausible
  contributor, and dropping hourly data from the periodic poll (above)
  independently shrinks that request's own footprint too. **This has not
  been specifically re-tested against the exact conditions that produced
  the crash** — if it recurs, suspect internal-RAM pressure from
  WiFi+mbedTLS+LVGL's combined footprint more broadly, not just this one
  cause, and don't assume the guard alone is a complete fix.
- **A real bug, found and fixed: `xTaskCreate()`'s return value was never
  checked, anywhere in this file.** For `weather_service_fetch_hours()`
  specifically, that meant: if creating the one-shot 8KB
  `hourly_fetch_task` failed (plausible under internal-RAM pressure — the
  same scarce resource behind the `ESP_ERR_NO_MEM` crash above, and this
  task is spawned fresh per tap, potentially long after boot when RAM is
  more fragmented than at startup), `hourly_fetch_task` simply never ran,
  its `args` leaked, and — critically — `args->cb()` never fired. Since
  `page_weather.cpp` only ever clears `s_hours_loading` from inside that
  callback, the observed symptom was the Weather page stuck on "Loading
  hourly forecast..." forever with no error, no timeout, nothing in the
  log to explain why. Both `xTaskCreate()` calls in this file (the
  periodic task and the on-demand one) now check the return value; the
  on-demand one specifically still calls the callback with `success=false`
  on failure so the UI can show "Couldn't load..." instead of hanging. If
  you add another `xTaskCreate()` call anywhere a caller is synchronously
  waiting on a callback to unblock UI state, check its return value —
  this exact failure mode (silent leak, callback never fires, UI stuck)
  will recur otherwise. Extensive `ESP_LOGI` tracing was also added
  through the whole on-demand path (task start, Wi-Fi wait outcome, the
  request URL, `fetch_raw()`'s outcome, the full response body in
  400-byte chunks, JSON parse outcome, hours parsed, callback firing,
  lock acquisition) and on the `page_weather.cpp` side (tap received,
  cache hit/miss, callback received, stale-reply drop, lock failure) —
  keep this logging if you touch this path again; it's what would have
  made this bug obvious immediately instead of needing to be reasoned out
  from a stuck UI with no visible error.
- **A second, separate hang, found via that same logging: rendering the 24
  hour-cards themselves froze hard enough to repeatedly trip the ESP-IDF
  task watchdog** (`E (...) task_wdt: ... CPU 1: wx_hourly_task`, with the
  *identical* PC and stack pointer inside `lv_label_set_text` on 12+
  independent 5-second watchdog samples spanning over a minute — not
  "slow," genuinely stuck). The log confirmed the fetch/parse/callback
  chain above (the `xTaskCreate` bug) all worked correctly this time; the
  hang was purely in building the UI for the result. Prime suspect: each
  hour card used to call the full `weather_icon_create()`, which for a
  sun/clear icon alone builds **9 sub-objects** (a circle + 8 ray
  squares) — so 24 hour cards could synchronously create up to ~288 LVGL
  objects in one burst, all while holding the `esp_lvgl_port` lock from a
  background task (blocking the LVGL task's own render loop the entire
  time) — a very different order of magnitude from the 5-day grid's ≤35
  objects, which never showed this symptom. Fix: hour cards now use
  `weather_icon_dot_create()` (`weather_icons.{h,cpp}`) — one color-coded
  circle per hour instead of the full vector icon — cutting the burst to
  ~96 objects, plus a defensive `vTaskDelay(1)` every 6 cards in
  `page_weather.cpp`'s hour-card loop so other tasks (notably the idle
  tasks that feed the watchdog) get scheduled even if the burst is still
  nontrivial. **Confirmed fixed on hardware**: tapping a day card now
  finishes rendering the hour row within a couple seconds with no
  `task_wdt` errors. This was inferred from object-count math and this
  session's repeated theme of internal-RAM/CPU pressure causing failures,
  not from a debugger attached to the original hang — the reasoning turned
  out to be right, but if a *different* hang shows up later in this same
  area (e.g. after adding more per-card content), don't assume the same
  fix generalizes automatically; get a stack high-water-mark reading or
  re-derive from fresh log evidence rather than pattern-matching to this
  incident.
- `page_weather.cpp`'s `rebuild_forecast_view()` doesn't open a second page
  for the hourly view; it swaps the same fixed-size `s_forecast_row`
  between the day-card grid (`LV_FLEX_FLOW_ROW`, horizontal scroll) and a
  "< Back" row + hour-card row (`LV_FLEX_FLOW_COLUMN` outer, horizontal
  scroll only on the inner hour-card row, with loading/failed states while
  a fetch is in flight) — keep that container reused rather than creating
  a new screen/tab if you touch this.
- **Weather fetch happens off the LVGL thread.** `weather_service.cpp` runs
  its own FreeRTOS task and calls back into `ui_manager_update_weather`,
  which takes the `esp_lvgl_port` lock itself
  (`lvgl_port_lock`/`lvgl_port_unlock`). Any new code that touches LVGL
  objects from outside the LVGL task (i.e. not from an `lv_timer` callback)
  must take that same lock — this is the one concurrency rule in the
  codebase that will crash/corrupt the UI if skipped.
- **NTP, weather, and smart lights all wait for Wi-Fi themselves, on their
  own background tasks.** `time_manager_start()` spawns a task that polls
  `wifi_manager_is_connected()` before calling `esp_netif_sntp_init()`;
  `weather_service_start()`'s and `smart_lights_service_start()`'s tasks do
  the same before their first fetch/poll. This keeps `app_main()`
  non-blocking — `wifi_manager_start()`, `time_manager_start()`,
  `weather_service_start()`, and `smart_lights_service_start()` can all be
  called back-to-back in `main.cpp` regardless of how long the Wi-Fi
  handshake takes. If you add another network-dependent service, follow
  the same pattern rather than assuming Wi-Fi is already up by the time
  your code runs.
- **Smart lights are polled over plain UDP sockets (`lwip/sockets.h`), not
  `esp_http_client`.** WiZ's protocol is a small unauthenticated
  JSON-over-UDP exchange (`getPilot` request → `result` object in the
  reply) on port 38899 by default — no HTTP, no TLS, no discovery. Each
  poll cycle (`smart_lights_service.cpp`, every 5s) opens one UDP socket
  per configured light with a 1s `SO_RCVTIMEO`, sends, receives, and closes
  it; a timeout or malformed reply just leaves that light reported as
  `online = false` for that cycle rather than failing the whole poll. This
  has **not** been tested against real WiZ hardware (see `README.md`'s
  Known gaps) — the request/response shape follows WiZ's publicly
  documented protocol, not something confirmed on-device the way the rest
  of this project has been.
- **Smart light controls (`wiz_set_power`/`wiz_set_brightness`/
  `wiz_set_color`/`wiz_set_scene` in `smart_lights_service.cpp`) are
  fire-and-forget: a single non-blocking UDP send, no reply read.** This is
  why `page_smart_lights.cpp`'s power switch/slider/scene-dropdown/
  color-button event callbacks call them directly and synchronously —
  unlike the weather/
  status-poll callbacks into `ui_manager`, these do **not** need
  `lvgl_port_lock()` for the network call itself (they're already running
  on the LVGL task, inside an LVGL event callback, and the UDP send is a
  single quick syscall) — don't add locking there, and don't turn these
  into a background-task+callback round trip like the status poll; that
  would just add latency for no benefit since we never read a reply
  anyway. The UI doesn't optimistically show the new state after sending a
  command — it waits for the next 5s poll (`page_smart_lights_update`) to
  reflect whatever the light's state actually became. One accepted
  side-effect: `rebuild_control_panel()` runs on every poll refresh (to
  reflect externally-changed state), which will interrupt an in-progress
  brightness-slider drag if a poll lands at that exact moment.
- **The WiZ scene dropdown's option index is its scene ID, by
  construction.** `kSceneNames` in `page_smart_lights.cpp` is a single
  `\n`-joined string in scene-ID order (1-32, the mapping shared by WiZ's
  own app and third-party integrations like pywizlight) with a sentinel
  `"Color / Custom"` prepended at index 0 meaning "not running a scene" —
  so option index *is* the scene ID for every real entry, no separate
  lookup table. If you ever reorder or insert into that string, the
  dropdown-selection → `wiz_set_scene()` call breaks silently (wrong scene
  applied) rather than failing loudly — append-only, or update the ID
  math in `scene_dropdown_event_cb()`/`light.scene_id` handling together
  with the string. `LightStatus.scene_id` (parsed from `getPilot`'s
  `sceneId` in `poll_wiz()`) is 0 when the light is in plain color/CCT
  mode, matching the sentinel.
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

## Adding a second smart light brand

`SmartLightConfig.brand` and `LightStatus` are already brand-agnostic,
mirroring the weather provider pattern above. To add one (e.g. Philips
Hue, which — unlike WiZ — needs bridge discovery/pairing and an API token,
so expect a bigger config schema addition than WiZ needed):

1. Extend the `smart_lights[]` schema in `README.md` /
   `sdcard/config_crono.json` / `app_config.cpp` only if the new brand
   needs fields WiZ doesn't have (e.g. a bridge IP + auth token for Hue).
2. In `smart_lights_service.cpp`, branch on `light.brand` in
   `smart_lights_task()` to call a different poll function for that brand,
   producing a `LightStatus` the same way `poll_wiz()` does; add matching
   `set_power`/`set_brightness`/`set_color`/`set_scene`-style control
   functions for that brand (fire-and-forget send, same shape as
   `wiz_set_power()` et al. — or a different transport entirely if the
   brand needs one, e.g. Hue's controls go over HTTP to a bridge, not UDP
   to the bulb). Don't assume WiZ's 1-32 scene numbering carries over —
   another brand's scene list/IDs (if it has one at all) needs its own
   name/ID mapping, not a reuse of `kSceneNames`.
3. `main/ui/page_smart_lights.cpp`'s status cards (`page_smart_lights_update`)
   don't need to change — they only ever see the normalized `LightStatus`
   list. Only `rebuild_control_panel()` needs a new brand branch, alongside
   the existing `if (light.brand != "WiZ")` check, to build that brand's
   own control widgets instead of (or in addition to) the "no controls
   available" fallback message.

## Testing expectations

There is no unit test suite (this is a hardware-driven embedded app with no
host-side build target). "Testing" here means the physical bring-up
checklist in `Agent_FNK0115_new_ESP-IDF.md` §8 (panel colors, no tearing,
touch alignment) plus manually confirming: SD card mounts and
`config_crono.json` parses (check boot log), Wi-Fi connects, NTP syncs
(clock page status row goes green), and a weather fetch succeeds (weather
page populates, clock page's corner icon updates). All of the above has
been confirmed on one physical FNK0115L_4_3_IPS unit, including the
on-demand hourly forecast fetch and the periodic request's date-ranging
(`WeatherDay.hours`/`iso_date`, `weather_service_fetch_hours()`,
`build_periodic_url()`'s date-range logic and `weather_task()`'s
wait-for-`time_manager_is_synced()` gate in front of it, the
`s_fetch_in_flight` mutual-exclusion guard, `rebuild_forecast_view()`'s
day-card ↔ hour-card swap in `page_weather.cpp`): tapping a day card
fetches, parses, and renders that day's hours correctly, and — critically
— the watchdog hang described above (rendering 24 hour-cards with the
full vector icon) is **confirmed fixed** by `weather_icon_dot_create()`;
tapping a day card now finishes rendering within a couple seconds with no
`task_wdt` errors. Not specifically exercised on hardware yet: a fetch
attempted with Wi-Fi down (should report "Couldn't load hourly
forecast..." rather than hanging — the code path exists but hasn't been
triggered), and tapping a day right at the exact moment the periodic poll
fires (the `s_fetch_in_flight` guard should make one of the two simply
retry, logged as "Skipping fetch — another weather request is already in
flight" — this is also the untested half of the `ESP_ERR_NO_MEM`
mitigation described above). The Smart Lights feature
(`smart_lights_service.cpp`, `page_smart_lights.cpp`) is the other
exception: it builds clean but **has not been tested against a real WiZ
bulb** — confirm a configured light actually shows correct on/off/
brightness/color/scene state (and that an unreachable one correctly shows
"Offline" rather than hanging), and separately confirm the control panel
actually controls the bulb: tapping a card selects it and shows the panel,
the power switch/brightness slider/scene dropdown/color presets each
produce the expected change on the physical light, the scene dropdown's
initial selection matches whatever scene (or "Color / Custom") the light
was actually already running, and selecting a non-WiZ-brand entry shows
the "no controls available" message instead of a panel. If you change
anything in this list and can't re-run it against real hardware, say so
explicitly rather than claiming the feature still works.
