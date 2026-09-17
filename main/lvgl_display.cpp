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

    // Without driver_data, esp_lcd_touch_new_i2c_gt911() skips its I2C
    // address-selection reset sequence entirely (logs "I2C address
    // initialization procedure skipped - using default GT9xx setup") and
    // falls back to a generic reset that doesn't drive INT during reset —
    // leaving the address the chip actually latches at power-on
    // undetermined instead of deterministically selecting
    // ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS (0x5D, matching the panel IO
    // config above). Supplying it performs the proper datasheet reset
    // dance (RST low, INT driven to select the address, RST high) and was
    // seen fixing repeated `i2c transaction failed` / GT911 read errors at
    // boot.
    static esp_lcd_touch_io_gt911_config_t gt911_addr_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
    };

    // Verify mirror_x/mirror_y on real hardware before shipping — don't
    // assume these from a vendor Arduino library's own rotation convention.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = (gpio_num_t)TOUCH_RST,
        .int_gpio_num = (gpio_num_t)TOUCH_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data = &gt911_addr_cfg,
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
