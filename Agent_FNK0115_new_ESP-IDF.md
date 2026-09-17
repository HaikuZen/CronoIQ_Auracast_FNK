# Bootstrapping a new Freenove FNK0115 ESP-IDF project (LVGL v9 + GT911)

Instructions for an agent creating a **brand-new** ESP-IDF project targeting
the Freenove FNK0115 (RGB-panel + GT911-touch variants:
`FNK0115L_4_3_IPS` 800×480, `FNK0115Q_5_0_IPS` 800×480) from scratch — no
existing Arduino code to port. This produces a minimal but complete,
verified-pattern skeleton: display + touch + a trivial LVGL demo screen,
ready to build application logic on top of. (For porting an *existing*
Arduino FNK0115 sketch instead of starting fresh, see
`Agent_FNK0115_migration_to_ESP-IDF.md`.)

Everything below is a known-working pattern (proven in this repository's
`main/lvgl_display.cpp`/`main/board.h`), not a guess — but pin/timing values
are for the specific unit that pattern was verified against. **Confirm colors
and touch orientation on the real board (step 8) before considering this
"done."**

## 0. Prerequisites

- ESP-IDF v5.3+ (this was built and verified against v6.1) with the
  toolchain set up (`source $IDF_PATH/export.sh`).
- Target: `esp32s3`.
- Know which variant you're targeting — this guide's concrete pin/timing
  values are for the two IPS+GT911 variants. If it's a TN variant
  (`FNK0115B_4_3_TN` 480×272 / `FNK0115N_5_0_TN` 800×480), the RGB panel
  section still applies but touch is XPT2046-over-SPI, not GT911 — that's
  out of scope here.
- **Query the actual board** before trusting any flash-size/PSRAM assumption:
  ```sh
  esptool.py --port /dev/ttyUSBx flash_id
  ```
  Use what it reports for `sdkconfig.defaults` (step 3), not a copied value.

## 1. Project skeleton

```sh
idf.py create-project my_fnk0115_app
cd my_fnk0115_app
idf.py set-target esp32s3
```

This produces `CMakeLists.txt` (root), `main/CMakeLists.txt`, `main/main.c`.
Rename `main/main.c` → `main/main.cpp` (the reference implementation and this
guide use C++ for the app layer; LVGL/esp_lcd/NimBLE are all C-callable
either way) and update `main/CMakeLists.txt` accordingly (step 6).

Root `CMakeLists.txt` needs no changes from the generated default:

```cmake
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_fnk0115_app)
```

**Do not put the project under a path containing `[` or `]`.** CMake's
`file(GLOB_RECURSE)` — which LVGL's managed-component build script uses to
collect its sources — silently returns zero files under such a path, and the
resulting failure (LVGL registered as a sourceless component) looks
completely unrelated to the actual cause.

## 2. Board variant config — `main/TFT_Config.h`

```c
#ifndef _TFT_CONFIG_H_
#define _TFT_CONFIG_H_

//#define FNK0115B_4_3_TN
#define FNK0115L_4_3_IPS
//#define FNK0115N_5_0_TN
//#define FNK0115Q_5_0_IPS

#endif
```

Uncomment the one matching your board; the rest of the code branches on
these macros for resolution/pins/timings.

## 3. Dependencies — `main/idf_component.yml`

**Must live here** (per-component manifest) — a manifest placed at the
project root is silently never read by the build.

```yaml
dependencies:
  idf: ">=5.3"
  lvgl/lvgl: "^9.5.0"
  espressif/esp_lvgl_port: "^2.9.0"
  espressif/esp_lcd_touch_gt911: "^1.2.1"
```

Check `https://components.espressif.com` for current versions rather than
trusting these indefinitely.

## 4. `sdkconfig.defaults`

Fill in flash size / PSRAM mode from what `esptool flash_id` actually
reported in step 0 — the values below are what this pattern was verified
with, not universal:

```
CONFIG_IDF_TARGET="esp32s3"

# --- Flash — match the real chip ---
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="16MB"

# --- PSRAM — only set MODE_OCT if flash_id actually reported octal PSRAM ---
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y

# --- Partition table: single large app, no OTA ---
CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y

# --- LVGL 9 ---
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_COLOR_16_SWAP=y          # standard requirement for esp_lcd_rgb_panel 16bpp RGB565
CONFIG_LV_FONT_MONTSERRAT_14=y
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_24=y      # add whichever sizes the UI actually uses —
                                     # LVGL only compiles in fonts enabled here
```

Delete any generated `sdkconfig` after editing this file and let
`idf.py build`/`reconfigure` regenerate it — don't hand-edit the generated
file.

## 5. Display + touch driver

### `main/lvgl_display.h`

```c
#ifndef _TFT_DISPLAY_H_
#define _TFT_DISPLAY_H_

#include "lvgl.h"
#include "TFT_Config.h"

#define TFT_BL 2

#if defined(FNK0115L_4_3_IPS) || defined(FNK0115N_5_0_TN) || defined(FNK0115Q_5_0_IPS)
  #define LCD_WIDTH  800
  #define LCD_HEIGHT 480
#elif defined(FNK0115B_4_3_TN)
  #define LCD_WIDTH  480
  #define LCD_HEIGHT 272
#endif

#define PIN_DE    40
#define PIN_VSYNC 41
#define PIN_HSYNC 39
#define PIN_PCLK  42
#define PIN_R0 45
#define PIN_R1 48
#define PIN_R2 47
#define PIN_R3 21
#define PIN_R4 14
#define PIN_G0 5
#define PIN_G1 6
#define PIN_G2 7
#define PIN_G3 15
#define PIN_G4 16
#define PIN_G5 4
#define PIN_B0 8
#define PIN_B1 3
#define PIN_B2 46
#define PIN_B3 9
#define PIN_B4 1

#if defined(FNK0115L_4_3_IPS) || defined(FNK0115Q_5_0_IPS)
  #define TOUCH_I2C_SDA 19
  #define TOUCH_I2C_SCL 20
  #define TOUCH_INT     18
  #define TOUCH_RST     38
#endif

// Initialises the RGB panel, backlight, GT911 touch, and the LVGL port
// (display + input device, when touch is available). Safe to call once
// before building any LVGL UI. Touch failure is non-fatal — see
// lvgl_display.cpp — the display still comes up without an input device.
void board_init(void);

#endif
```

### `main/lvgl_display.cpp`

```c
#include "lvgl_display.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

// This project's build uses -Wall -Werror without
// -Wno-error=missing-field-initializers; vendor SDK config macros/structs
// are meant to be partially designated-initialized. Silence just that for
// this file rather than changing the project-wide warning flags.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static const char *TAG = "board";

static esp_lcd_panel_handle_t s_lcd_panel = nullptr;
static esp_lcd_touch_handle_t s_touch_handle = nullptr;

static void lcd_panel_init(void) {
    ESP_LOGI(TAG, "Initialising RGB panel (%dx%d)", LCD_WIDTH, LCD_HEIGHT);

    const esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 13 * 1000 * 1000,
            .h_res = LCD_WIDTH,
            .v_res = LCD_HEIGHT,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 4,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 4,
            .flags = {
                .pclk_active_neg = 1,
            },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,   // IDF >= 6.0 field name;
                                                     // older IDF: .bits_per_pixel = 16
        .num_fbs = 2,
        .dma_burst_size = 64,
        .hsync_gpio_num = (gpio_num_t)PIN_HSYNC,
        .vsync_gpio_num = (gpio_num_t)PIN_VSYNC,
        .de_gpio_num = (gpio_num_t)PIN_DE,
        .pclk_gpio_num = (gpio_num_t)PIN_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,
        // Bit0 (LSB) .. Bit15 (MSB) of the 16-bit RGB565 bus.
        .data_gpio_nums = {
            (gpio_num_t)PIN_B0, (gpio_num_t)PIN_B1, (gpio_num_t)PIN_B2, (gpio_num_t)PIN_B3, (gpio_num_t)PIN_B4,
            (gpio_num_t)PIN_G0, (gpio_num_t)PIN_G1, (gpio_num_t)PIN_G2, (gpio_num_t)PIN_G3, (gpio_num_t)PIN_G4, (gpio_num_t)PIN_G5,
            (gpio_num_t)PIN_R0, (gpio_num_t)PIN_R1, (gpio_num_t)PIN_R2, (gpio_num_t)PIN_R3, (gpio_num_t)PIN_R4,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd_panel));
}

static void backlight_init(void) {
    const gpio_config_t bl_gpio_config = {
        .pin_bit_mask = 1ULL << TFT_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_gpio_config));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)TFT_BL, 1));
}

// Returns true if the GT911 was successfully brought up.
static bool touch_init(void) {
    ESP_LOGI(TAG, "Initialising GT911 touch (SDA=%d SCL=%d INT=%d RST=%d)",
             TOUCH_I2C_SDA, TOUCH_I2C_SCL, TOUCH_INT, TOUCH_RST);

    i2c_master_bus_handle_t i2c_bus = nullptr;
    const i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)TOUCH_I2C_SDA,
        .scl_io_num = (gpio_num_t)TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io_handle = nullptr;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));

    // Verify mirror_x/mirror_y on real hardware (step 8) — don't assume
    // these from a vendor Arduino library's own rotation convention.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = (gpio_num_t)TOUCH_RST,
        .int_gpio_num = (gpio_num_t)TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };

    // The GT911's I2C config read can intermittently fail at boot on some
    // units/connections (not always a fixed settling-time issue — can point
    // to a marginal physical connection). Retry, but don't hard-abort the
    // whole app over it — a display with no touch still beats no boot.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 10; attempt++) {
        err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_touch_handle);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "GT911 init attempt %d failed (%s), retrying...",
                 attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 not responding after retries (%s) — "
                      "continuing without touch input", esp_err_to_name(err));
        s_touch_handle = nullptr;
        return false;
    }
    return true;
}

static void lvgl_port_setup(void) {
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_lcd_panel,
        .buffer_size = (uint32_t)LCD_WIDTH * LCD_HEIGHT,
        .double_buffer = true,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_spiram = true,
            // REQUIRED alongside rgb_cfg.flags.avoid_tearing + num_fbs=2 —
            // without it the two physical frame buffers drift out of sync
            // on partial redraws (visible as the screen flipping between
            // stale/fresh "pages" on every touch-driven redraw).
            .direct_mode = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = { .avoid_tearing = true },
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed");
        abort();
    }

    if (s_touch_handle) {
        const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = s_touch_handle };
        if (!lvgl_port_add_touch(&touch_cfg)) {
            ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        }
    }
}

void board_init(void) {
    lcd_panel_init();
    backlight_init();
    touch_init();  // failure is non-fatal — see touch_init()
    lvgl_port_setup();
    ESP_LOGI(TAG, "Board init done");
}
```

### `main/board.h`

```c
#pragma once
#include "lvgl_display.h"
```

## 6. Minimal demo app — `main/main.cpp`

A small self-contained screen that proves the panel renders and (if present)
touch tracks correctly — a title label plus a dot that follows your finger
and a label showing live coordinates. Build the real UI on top of this once
it's confirmed working.

```c
#include "esp_log.h"
#include "board.h"
#include "esp_lvgl_port.h"

static const char *TAG = "app";
static lv_obj_t *s_coord_label;
static lv_obj_t *s_touch_dot;

static void touch_event_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    ESP_LOGI(TAG, "Touch x=%d y=%d", (int)p.x, (int)p.y);
    lv_label_set_text_fmt(s_coord_label, "Touch: %d, %d", (int)p.x, (int)p.y);
    lv_obj_set_pos(s_touch_dot, p.x - 10, p.y - 10);
    lv_obj_clear_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
}

static void build_demo_ui(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "FNK0115 - esp_lcd RGB + LVGL 9 + GT911");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00C8FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    s_coord_label = lv_label_create(scr);
    lv_label_set_text(s_coord_label, "Touch: --, --");
    lv_obj_set_style_text_color(s_coord_label, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(s_coord_label, &lv_font_montserrat_24, 0);
    lv_obj_align(s_coord_label, LV_ALIGN_CENTER, 0, 20);

    s_touch_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_touch_dot);
    lv_obj_set_size(s_touch_dot, 20, 20);
    lv_obj_set_style_radius(s_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_touch_dot, lv_color_hex(0xFF6B35), 0);
    lv_obj_set_style_bg_opa(s_touch_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_touch_dot, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_add_event_cb(scr, touch_event_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(scr, touch_event_cb, LV_EVENT_PRESSING, nullptr);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "FNK0115 template — display/touch demo");
    board_init();
    if (lvgl_port_lock(0)) {
        build_demo_ui();
        lvgl_port_unlock();
    }
    // No lv_timer_handler()/polling loop needed — esp_lvgl_port runs its own task.
}
```

## 7. `main/CMakeLists.txt`

```cmake
idf_component_register(SRCS "main.cpp" "lvgl_display.cpp"
                    INCLUDE_DIRS ".")
```

## 8. Build, flash, verify

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSBx flash monitor
```

Checklist:

1. Clean build (fix any exact struct/field-name mismatches against the
   installed IDF/LVGL/esp_lvgl_port versions — field names do shift between
   versions; the compiler errors point at the exact struct member).
2. Boot log: RGB panel init OK, GT911 probed (or gracefully skipped),
   `esp_lvgl_port`/LVGL task started, no panic/reset loop.
3. **Look at the physical screen**: colors correct (not red/blue swapped —
   if they are, check the `data_gpio_nums` bit order or try toggling
   `CONFIG_LV_COLOR_16_SWAP`), image stable (no flicker/page-flipping — if it
   is, `direct_mode` is almost certainly not set).
4. **Touch all four corners and the center**: the dot should land exactly
   where you touch. If it's mirrored on one or both axes, flip
   `mirror_x`/`mirror_y` (or `swap_xy`) in `lvgl_display.cpp` and reflash —
   don't assume a vendor library's convention carries over.

Once all four pass, this is a solid base to build real application logic
(additional screens, sensors, BLE, etc.) on top of.
