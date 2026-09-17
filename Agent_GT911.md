# Fixing GT911 touch init failures (`esp_lcd_touch_gt911`, ESP-IDF)

Instructions for an agent debugging a GT911 capacitive-touch controller that
fails to initialize (or initializes unreliably) on ESP-IDF, using
Espressif's `esp_lcd_touch_gt911` managed component
(`espressif/esp_lcd_touch_gt911` on the component registry, built on top of
`espressif/esp_lcd_touch`). This is a **portable** guide — it applies to any
board using this driver, not a specific one. It was written after
diagnosing and fixing exactly this failure on a Freenove FNK0115
(ESP32-S3 + GT911 over I2C); the root cause and fix below were confirmed
working on that physical hardware. The rest of the checklist (later
section) is standard troubleshooting knowledge for this chip/driver
combination, included for when the primary fix doesn't apply or doesn't
fully resolve things — treat that part as leads to check, not as
independently hardware-verified the way the primary fix is.

## 1. Symptom — does this guide apply?

Boot log shows the touch controller failing to initialize, typically
repeatedly if the calling code retries. Look for this exact combination:

```
I (...) GT911: I2C address initialization procedure skipped - using default GT9xx setup
E (...) lcd_panel.io.i2c: panel_io_i2c_rx_buffer(...): i2c transaction failed
E (...) GT911: touch_gt911_read_cfg(...): GT911 read error!
E (...) GT911: esp_lcd_touch_new_i2c_gt911(...): GT911 init failed
E (...) GT911: Error (0x108)! Touch controller GT911 initialization failed!
```

`0x108` is `ESP_ERR_INVALID_RESPONSE`. The load-bearing line is the first
one — **"I2C address initialization procedure skipped"** — that's the
driver telling you it didn't perform its address-selection reset sequence,
and the fix in §2 addresses exactly that. If your log doesn't show that
line at all (i.e. address init isn't being skipped) and you're still
failing, skip to §4.

## 2. Root cause and fix

In `esp_lcd_touch_new_i2c_gt911()`, the driver only performs GT911's
documented power-on I2C address-selection sequence (see §3) when **all**
of these are true:

- `config->driver_data` is non-NULL and points to a valid
  `esp_lcd_touch_io_gt911_config_t`
- `config->rst_gpio_num != GPIO_NUM_NC`
- `config->int_gpio_num != GPIO_NUM_NC`

If you build your `esp_lcd_touch_config_t` the "obvious" way — filling in
`x_max`/`y_max`/`rst_gpio_num`/`int_gpio_num`/`levels`/`flags` and leaving
`driver_data` untouched (so it's `NULL`, e.g. from a designated
initializer or `= {}`) — the condition above is false, the driver logs
"I2C address initialization procedure skipped", and falls back to a
generic reset that does **not** drive `INT` during reset. That leaves the
I2C address the chip actually latches at power-on undetermined instead of
deterministically forcing it to match what your panel IO config expects —
which is a very plausible cause of `i2c transaction failed` / GT911 read
errors that happen on *every* boot (not just an occasional flaky one).

**Fix**: populate `driver_data` with a `dev_addr` matching your panel IO
config's own address.

```c
#include "esp_lcd_touch_gt911.h"

// Must outlive the esp_lcd_touch_new_i2c_gt911() call — a function-local
// `static` (as here) or anything with equal-or-longer lifetime works; it
// does not need to survive after that call returns.
static esp_lcd_touch_io_gt911_config_t gt911_addr_cfg = {
    .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,  // 0x5D — match this to
                                                       // whatever address
                                                       // your esp_lcd_panel_io_i2c_config_t
                                                       // (built from
                                                       // ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG())
                                                       // is using.
};

const esp_lcd_touch_config_t tp_cfg = {
    .x_max = LCD_WIDTH,
    .y_max = LCD_HEIGHT,
    .rst_gpio_num = TOUCH_RST,   // must be a real GPIO, not GPIO_NUM_NC
    .int_gpio_num = TOUCH_INT,   // must be a real GPIO, not GPIO_NUM_NC
    .levels = { .reset = 0, .interrupt = 0 },
    .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    .driver_data = &gt911_addr_cfg,   // <-- the fix
};

esp_err_t err = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &touch_handle);
```

With this in place, the log line changes from "I2C address initialization
procedure skipped" to the driver actually running its reset dance, and on
the hardware this was diagnosed against, GT911 went from failing all 10
retry attempts on *every* boot to succeeding on the *first* attempt, every
time — logging something like:

```
I (...) board: Initialising GT911 touch (SDA=... SCL=... INT=... RST=...)
W (...) gpio: conflict found for GPIO[N]        <- benign, see below
I (...) GT911: TouchPad_ID:0x39,0x31,0x31
I (...) GT911: TouchPad_Config_Version:70
```

**Expect a benign `W (...) gpio: conflict found for GPIO[N]` warning**
right after, where `N` is your `int_gpio_num`. The address-selection
sequence legitimately reconfigures that pin twice — briefly as an output
to drive the address-select line during reset, then as an input for the
touch interrupt — and ESP-IDF's GPIO reservation tracking logs that reuse
as a "conflict" even though it isn't one. Don't chase that warning; it's
expected and harmless.

If `rst_gpio_num` or `int_gpio_num` is `GPIO_NUM_NC` on your board (RST/INT
hardwired rather than MCU-controlled, or genuinely not connected), this fix
cannot apply — the driver's condition for running the address-selection
sequence requires both pins. In that case you cannot force the address in
software; see §4 for what to check instead.

## 3. Why this works (GT911 address latch, for context)

Per the GT911 datasheet, the chip latches one of two I2C addresses at the
moment `RST` is released, based on the level of `INT` at that instant:

- `INT` low during reset → address `0x5D` (`ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS`)
- `INT` high during reset → address `0x14` (`ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP`)

The driver's address-selection sequence (only run when `driver_data` +
both GPIOs are present) explicitly drives `INT` to the level matching
`dev_addr` before releasing `RST`, so the chip deterministically comes up
at the address you asked for. Skip that sequence and `INT`'s level at
reset is whatever it happens to float to (external pull-up/pull-down,
previous GPIO state, etc.) — which may or may not match the address your
I2C transactions are actually targeting.

## 4. If the fix in §2 doesn't apply or doesn't fully resolve it

Standard checklist for this chip/driver combination — not independently
hardware-verified the way §2 is, but the common next places to look:

1. **Address mismatch, not address instability.** Some GT911 modules ship
   hard-strapped to `0x14` instead of the library's default `0x5D`. Try
   building your panel IO config (`esp_lcd_panel_io_i2c_config_t`) with
   `.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP` and matching
   `gt911_addr_cfg.dev_addr` to the same value.
2. **Pin numbers / active levels don't match the actual schematic.** Don't
   trust a vendor Arduino library's pin numbering or reset polarity
   verbatim — GT911's reset is active-low (`levels.reset = 0`); confirm SDA/
   SCL/RST/INT against your board's actual schematic, not an assumption
   carried over from different example code.
3. **I2C bus timing/pull-ups.** `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()`
   defaults to `.scl_speed_hz = 100000`. If wiring is marginal (long
   traces, weak pull-ups), that's usually fine since it's already
   conservative — but confirm the I2C bus config
   (`i2c_master_bus_config_t`) has `.flags.enable_internal_pullup = 1`, or
   add external ~4.7kΩ pull-ups on SDA/SCL if the internal ones aren't
   enough for your bus capacitance.
4. **Power/reset settling time.** Some modules need the supply to settle
   and a real low pulse on `RST` (tens of ms) before the chip responds on
   I2C at all. If failures happen specifically right after cold power-on
   but a warm reset/re-flash works, suspect this before suspecting the
   software.
5. **Retry loop as a safety net, not a substitute for §2.** Even correctly
   configured hardware can occasionally miss an I2C transaction at boot.
   Wrapping the init call in a bounded retry loop (a handful of attempts,
   short delay between) is reasonable defensive coding on top of the fix
   in §2 — but if every attempt fails identically, that's a systemic cause
   (§2, or one of the items above), not something a retry loop will paper
   over.
6. **GPIO contention from other peripherals.** If SDA/SCL/RST/INT are
   shared with, or physically adjacent to, pins driven by other
   initialization code that runs before touch init (a display panel, an
   SD card bus, a backlight PWM), confirm nothing else claims or toggles
   those exact GPIOs first.
7. **Bus/handle ordering.** Confirm `i2c_new_master_bus()` and
   `esp_lcd_new_panel_io_i2c()` both return `ESP_OK` before
   `esp_lcd_touch_new_i2c_gt911()` is called, and that no earlier code
   already created a conflicting I2C master bus on the same port.
