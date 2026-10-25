#include "env_sensor_service.h"

#include <cstring>
#include <utility>

#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef CRONOIQ_HAVE_BSEC
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

extern "C" {
#include "bme69x.h"
#include "bsec_iaq.h"
#include "bsec_interface.h"
}
#endif

static const char *TAG = "env_sensor";

static EnvSensorConfig s_cfg;
static EnvUpdateCb s_on_update;
static EnvReading s_reading;

static void report_state(EnvSensorState state) {
    s_reading.state = state;
    if (s_on_update) s_on_update(s_reading);
}

#ifdef CRONOIQ_HAVE_BSEC

// The ESP-IDF I2C master driver locks the bus per transaction, so these
// share the touch controller's bus safely from this task. 100kHz rather
// than the GT911's 400kHz: the bus only has the ESP32's weak internal
// pull-ups plus whatever the breakout adds, over flying leads from P4.
static constexpr uint32_t kI2cSpeedHz = 100000;
static constexpr int kI2cTimeoutMs = 100;
static constexpr uint32_t kReprobeIntervalMs = 10000;
// Consecutive bme69x read failures before assuming the sensor was
// unplugged/lost power and going back to probing.
static constexpr int kMaxConsecutiveErrors = 5;
// BSEC calibration state is saved to NVS this often (and restored at boot)
// so IAQ accuracy doesn't restart from 0 on every reboot. Hourly keeps NVS
// wear negligible (~200-byte blob) while losing at most an hour of
// calibration progress.
static constexpr int64_t kStateSaveIntervalUs = 60LL * 60 * 1000 * 1000;
// Total heater on-time budget per parallel-mode cycle, from Bosch's BSEC
// integration guide; the shared heater duration is what's left of it after
// the T/P/H measurement itself.
static constexpr uint16_t kParallelTotalHeatDurMs = 140;
// Readings are logged on the first BSEC output and then this often — every
// 3s would drown out the rest of the boot/serial log.
static constexpr int64_t kReadingLogIntervalUs = 60LL * 1000 * 1000;
static constexpr const char *kNvsNamespace = "bsec";
static constexpr const char *kNvsStateKey = "state";

static i2c_master_dev_handle_t s_i2c_dev = nullptr;
static struct bme69x_dev s_bme = {};
static struct bme69x_conf s_bme_conf = {};
static struct bme69x_heatr_conf s_heatr_conf = {};
static uint8_t s_op_mode = BME69X_SLEEP_MODE;
static void *s_bsec = nullptr;
// Scratch buffer BSEC needs for set_configuration/set_state/get_state —
// static rather than on the task stack (4KB).
static uint8_t s_work_buffer[BSEC_MAX_WORKBUFFER_SIZE];

// ---------------------------------------------------------------------------
// bme69x SensorAPI glue
// ---------------------------------------------------------------------------

static BME69X_INTF_RET_TYPE bme_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    auto dev = static_cast<i2c_master_dev_handle_t>(intf_ptr);
    esp_err_t err = i2c_master_transmit_receive(dev, &reg_addr, 1, reg_data, len, kI2cTimeoutMs);
    return err == ESP_OK ? 0 : -1;
}

static BME69X_INTF_RET_TYPE bme_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr) {
    auto dev = static_cast<i2c_master_dev_handle_t>(intf_ptr);
    // bme69x_set_regs() interleaves (reg, value) pairs into at most ~40
    // bytes per call; 64 leaves headroom.
    uint8_t buf[64];
    if (len + 1 > sizeof(buf)) return -1;
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);
    esp_err_t err = i2c_master_transmit(dev, buf, len + 1, kI2cTimeoutMs);
    return err == ESP_OK ? 0 : -1;
}

static void bme_delay_us(uint32_t period, void *intf_ptr) {
    (void)intf_ptr;
    // Yield for anything long enough to be worth a context switch (the
    // driver's 10ms new-data polls, soft reset) rather than busy-waiting.
    if (period >= 1000) {
        TickType_t ticks = pdMS_TO_TICKS((period + 999) / 1000);
        vTaskDelay(ticks > 0 ? ticks : 1);
    } else {
        esp_rom_delay_us(period);
    }
}

// Returns the first address (0x76, then 0x77, or just the configured one)
// that ACKs, or 0.
static uint8_t probe_address(i2c_master_bus_handle_t bus) {
    const uint8_t candidates[] = {BME69X_I2C_ADDR_LOW, BME69X_I2C_ADDR_HIGH};
    for (uint8_t addr : candidates) {
        if (s_cfg.i2c_address != 0 && s_cfg.i2c_address != addr) continue;
        if (i2c_master_probe(bus, addr, kI2cTimeoutMs) == ESP_OK) return addr;
    }
    return 0;
}

static void release_device() {
    if (s_i2c_dev) {
        i2c_master_bus_rm_device(s_i2c_dev);
        s_i2c_dev = nullptr;
    }
}

static bool init_sensor(i2c_master_bus_handle_t bus, uint8_t addr) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = kI2cSpeedHz;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02x) failed: %s", addr, esp_err_to_name(err));
        s_i2c_dev = nullptr;
        return false;
    }

    s_bme = {};
    s_bme.intf = BME69X_I2C_INTF;
    s_bme.intf_ptr = s_i2c_dev;
    s_bme.read = bme_i2c_read;
    s_bme.write = bme_i2c_write;
    s_bme.delay_us = bme_delay_us;
    // Used by the driver to compute heater resistance for a target
    // temperature; BSEC's own compensation handles the rest.
    s_bme.amb_temp = 25;

    int8_t rslt = bme69x_init(&s_bme);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_init failed at 0x%02x: %d (device answered but isn't a "
                      "BME69x, or the bus is marginal — check pull-ups/lead length)",
                 addr, rslt);
        return false;
    }
    s_op_mode = BME69X_SLEEP_MODE;
    ESP_LOGI(TAG, "BME69x found at 0x%02x (chip id 0x%02x, variant 0x%lx)", addr, s_bme.chip_id,
             (unsigned long)s_bme.variant_id);
    return true;
}

// ---------------------------------------------------------------------------
// BSEC state persistence
// ---------------------------------------------------------------------------

static void load_bsec_state() {
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "No saved BSEC state — IAQ calibration starts from scratch");
        return;
    }
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    size_t len = sizeof(state);
    esp_err_t err = nvs_get_blob(nvs, kNvsStateKey, state, &len);
    nvs_close(nvs);
    if (err != ESP_OK || len == 0) {
        ESP_LOGI(TAG, "No saved BSEC state — IAQ calibration starts from scratch");
        return;
    }
    bsec_library_return_t st = bsec_set_state(s_bsec, state, len, s_work_buffer, sizeof(s_work_buffer));
    if (st != BSEC_OK) {
        // E.g. after a BSEC library/config upgrade the saved blob no longer
        // matches — just recalibrate rather than fail.
        ESP_LOGW(TAG, "Saved BSEC state rejected (%d) — recalibrating from scratch", st);
    } else {
        ESP_LOGI(TAG, "Restored BSEC calibration state (%u bytes)", (unsigned)len);
    }
}

static void save_bsec_state() {
    uint8_t state[BSEC_MAX_STATE_BLOB_SIZE];
    uint32_t len = 0;
    bsec_library_return_t st =
        bsec_get_state(s_bsec, 0, state, sizeof(state), s_work_buffer, sizeof(s_work_buffer), &len);
    if (st != BSEC_OK || len == 0) {
        ESP_LOGW(TAG, "bsec_get_state failed (%d) — not saving", st);
        return;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, kNvsStateKey, state, len);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving BSEC state to NVS failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved BSEC calibration state (%u bytes, IAQ accuracy %u)", (unsigned)len,
                 s_reading.iaq_accuracy);
    }
}

// ---------------------------------------------------------------------------
// BSEC
// ---------------------------------------------------------------------------

static bool init_bsec() {
    if (!s_bsec) {
        s_bsec = heap_caps_calloc(1, bsec_get_instance_size(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_bsec) {
            ESP_LOGE(TAG, "Out of memory for BSEC instance (%u bytes)", (unsigned)bsec_get_instance_size());
            return false;
        }
    }

    bsec_library_return_t st = bsec_init(s_bsec);
    if (st != BSEC_OK) {
        ESP_LOGE(TAG, "bsec_init failed: %d", st);
        return false;
    }

    bsec_version_t ver = {};
    bsec_get_version(s_bsec, &ver);
    ESP_LOGI(TAG, "BSEC %d.%d.%d.%d", ver.major, ver.minor, ver.major_bugfix, ver.minor_bugfix);

    st = bsec_set_configuration(s_bsec, bsec_config_iaq, sizeof(bsec_config_iaq), s_work_buffer,
                                sizeof(s_work_buffer));
    if (st != BSEC_OK) {
        ESP_LOGE(TAG, "bsec_set_configuration failed: %d", st);
        return false;
    }

    load_bsec_state();

    // LP (3s) must match the bme690_iaq_33v_3s_4d config selected in
    // components/bsec/CMakeLists.txt.
    const float rate = BSEC_SAMPLE_RATE_LP;
    const uint8_t outputs[] = {
        BSEC_OUTPUT_IAQ,
        BSEC_OUTPUT_STATIC_IAQ,
        BSEC_OUTPUT_CO2_EQUIVALENT,
        BSEC_OUTPUT_TVOC_EQUIVALENT,
        BSEC_OUTPUT_RAW_PRESSURE,
        BSEC_OUTPUT_RAW_GAS,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
        BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
        BSEC_OUTPUT_GAS_PERCENTAGE,
        BSEC_OUTPUT_STABILIZATION_STATUS,
        BSEC_OUTPUT_RUN_IN_STATUS,
    };
    bsec_sensor_configuration_t requested[sizeof(outputs)];
    for (size_t i = 0; i < sizeof(outputs); i++) {
        requested[i] = {};
        requested[i].sensor_id = outputs[i];
        requested[i].sample_rate = rate;
    }
    bsec_sensor_configuration_t required[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n_required = BSEC_MAX_PHYSICAL_SENSOR;
    st = bsec_update_subscription(s_bsec, requested, sizeof(outputs), required, &n_required);
    if (st < BSEC_OK) {
        ESP_LOGE(TAG, "bsec_update_subscription failed: %d", st);
        return false;
    }
    if (st > BSEC_OK) {
        ESP_LOGW(TAG, "bsec_update_subscription warning: %d", st);
    }
    return true;
}

static void apply_forced_mode(const bsec_bme_settings_t &settings) {
    bme69x_set_op_mode(BME69X_SLEEP_MODE, &s_bme);
    if (bme69x_get_conf(&s_bme_conf, &s_bme) != BME69X_OK) return;
    s_bme_conf.os_hum = settings.humidity_oversampling;
    s_bme_conf.os_temp = settings.temperature_oversampling;
    s_bme_conf.os_pres = settings.pressure_oversampling;
    if (bme69x_set_conf(&s_bme_conf, &s_bme) != BME69X_OK) return;

    s_heatr_conf = {};
    s_heatr_conf.enable = BME69X_ENABLE;
    s_heatr_conf.heatr_temp = settings.heater_temperature;
    s_heatr_conf.heatr_dur = settings.heater_duration;
    if (bme69x_set_heatr_conf(BME69X_FORCED_MODE, &s_heatr_conf, &s_bme) != BME69X_OK) return;

    if (bme69x_set_op_mode(BME69X_FORCED_MODE, &s_bme) == BME69X_OK) {
        s_op_mode = BME69X_FORCED_MODE;
    }
}

static void apply_parallel_mode(const bsec_bme_settings_t &settings) {
    if (bme69x_get_conf(&s_bme_conf, &s_bme) != BME69X_OK) return;
    s_bme_conf.os_hum = settings.humidity_oversampling;
    s_bme_conf.os_temp = settings.temperature_oversampling;
    s_bme_conf.os_pres = settings.pressure_oversampling;
    if (bme69x_set_conf(&s_bme_conf, &s_bme) != BME69X_OK) return;

    uint32_t meas_ms = bme69x_get_meas_dur(BME69X_PARALLEL_MODE, &s_bme_conf, &s_bme) / 1000;
    s_heatr_conf = {};
    s_heatr_conf.enable = BME69X_ENABLE;
    // The driver only reads these profile arrays (writing them to the
    // sensor's registers inside bme69x_set_heatr_conf()); its struct just
    // isn't const-correct.
    s_heatr_conf.heatr_temp_prof = const_cast<uint16_t *>(settings.heater_temperature_profile);
    s_heatr_conf.heatr_dur_prof = const_cast<uint16_t *>(settings.heater_duration_profile);
    s_heatr_conf.profile_len = settings.heater_profile_len;
    s_heatr_conf.shared_heatr_dur = (uint16_t)(kParallelTotalHeatDurMs - meas_ms);
    if (bme69x_set_heatr_conf(BME69X_PARALLEL_MODE, &s_heatr_conf, &s_bme) != BME69X_OK) return;

    if (bme69x_set_op_mode(BME69X_PARALLEL_MODE, &s_bme) == BME69X_OK) {
        s_op_mode = BME69X_PARALLEL_MODE;
    }
}

// Feeds one bme69x sample to BSEC and folds whatever outputs come back into
// s_reading. Returns true if BSEC produced any output.
static bool process_sample(int64_t ts_ns, const struct bme69x_data &data, uint32_t process_data) {
    bsec_input_t inputs[BSEC_MAX_PHYSICAL_SENSOR];
    uint8_t n = 0;
    auto add = [&](uint8_t id, float signal) {
        inputs[n] = {};
        inputs[n].sensor_id = id;
        inputs[n].signal = signal;
        inputs[n].signal_dimensions = 1;
        inputs[n].time_stamp = ts_ns;
        n++;
    };
    auto wants = [&](uint8_t id) { return (process_data & (1u << (id - 1))) != 0; };

    if (wants(BSEC_INPUT_HEATSOURCE)) add(BSEC_INPUT_HEATSOURCE, s_cfg.temp_offset_c);
    if (wants(BSEC_INPUT_TEMPERATURE)) add(BSEC_INPUT_TEMPERATURE, data.temperature);
    if (wants(BSEC_INPUT_HUMIDITY)) add(BSEC_INPUT_HUMIDITY, data.humidity);
    if (wants(BSEC_INPUT_PRESSURE)) add(BSEC_INPUT_PRESSURE, data.pressure);
    if (wants(BSEC_INPUT_GASRESISTOR)) add(BSEC_INPUT_GASRESISTOR, data.gas_resistance);
    if (wants(BSEC_INPUT_PROFILE_PART)) {
        add(BSEC_INPUT_PROFILE_PART, s_op_mode == BME69X_FORCED_MODE ? 0.0f : (float)data.gas_index);
    }
    // LP mode: keep BSEC's baseline tracker enabled (0 = don't disable).
    add(BSEC_INPUT_DISABLE_BASELINE_TRACKER, 0.0f);

    bsec_output_t outputs[BSEC_NUMBER_OUTPUTS];
    uint8_t n_outputs = BSEC_NUMBER_OUTPUTS;
    bsec_library_return_t st = bsec_do_steps(s_bsec, inputs, n, outputs, &n_outputs);
    if (st != BSEC_OK) {
        ESP_LOGW(TAG, "bsec_do_steps returned %d", st);
        if (st < BSEC_OK) return false;
    }
    if (n_outputs == 0) return false;

    for (uint8_t i = 0; i < n_outputs; i++) {
        const bsec_output_t &o = outputs[i];
        switch (o.sensor_id) {
            case BSEC_OUTPUT_IAQ:
                s_reading.iaq = o.signal;
                s_reading.iaq_accuracy = o.accuracy;
                break;
            case BSEC_OUTPUT_STATIC_IAQ: s_reading.static_iaq = o.signal; break;
            case BSEC_OUTPUT_CO2_EQUIVALENT: s_reading.co2_ppm = o.signal; break;
            case BSEC_OUTPUT_TVOC_EQUIVALENT: s_reading.tvoc_ppb = o.signal; break;
            case BSEC_OUTPUT_RAW_PRESSURE: s_reading.pressure_hpa = o.signal / 100.0f; break;
            case BSEC_OUTPUT_RAW_GAS: s_reading.gas_resistance_ohm = o.signal; break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE: s_reading.temperature_c = o.signal; break;
            case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY: s_reading.humidity_pct = o.signal; break;
            case BSEC_OUTPUT_GAS_PERCENTAGE: s_reading.gas_percentage = o.signal; break;
            case BSEC_OUTPUT_STABILIZATION_STATUS: s_reading.stabilized = o.signal >= 1.0f; break;
            case BSEC_OUTPUT_RUN_IN_STATUS: s_reading.run_in_done = o.signal >= 1.0f; break;
            default: break;
        }
    }
    return true;
}

// Runs BSEC's sensor-control loop until the sensor stops answering. Returns
// so the caller can go back to probing (e.g. it was unplugged).
static void run_bsec_loop() {
    bsec_bme_settings_t settings = {};
    int consecutive_errors = 0;
    int64_t last_save_us = esp_timer_get_time();
    int64_t last_log_us = 0;

    report_state(EnvSensorState::WarmingUp);

    while (consecutive_errors < kMaxConsecutiveErrors) {
        const int64_t now_ns = esp_timer_get_time() * 1000;
        if (now_ns >= settings.next_call) {
            bsec_library_return_t st = bsec_sensor_control(s_bsec, now_ns, &settings);
            if (st != BSEC_OK) {
                // Typically BSEC_W_SC_CALL_TIMING_VIOLATION if this task was
                // starved for a while — harmless, just noted.
                ESP_LOGW(TAG, "bsec_sensor_control returned %d", st);
            }

            switch (settings.op_mode) {
                case BME69X_FORCED_MODE:
                    apply_forced_mode(settings);
                    break;
                case BME69X_PARALLEL_MODE:
                    if (s_op_mode != BME69X_PARALLEL_MODE) apply_parallel_mode(settings);
                    break;
                case BME69X_SLEEP_MODE:
                    if (s_op_mode != BME69X_SLEEP_MODE &&
                        bme69x_set_op_mode(BME69X_SLEEP_MODE, &s_bme) == BME69X_OK) {
                        s_op_mode = BME69X_SLEEP_MODE;
                    }
                    break;
                default:
                    break;
            }

            if (settings.trigger_measurement && settings.op_mode != BME69X_SLEEP_MODE) {
                if (s_op_mode == BME69X_FORCED_MODE) {
                    // A forced measurement takes T/P/H conversion time plus
                    // the heater duration (often 100ms+) — longer than the
                    // driver's own 5x10ms new-data poll in bme69x_get_data()
                    // covers — so wait it out before reading.
                    uint32_t wait_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &s_bme_conf, &s_bme) +
                                       (uint32_t)settings.heater_duration * 1000;
                    bme_delay_us(wait_us, nullptr);
                }

                struct bme69x_data data[3] = {};
                uint8_t n_fields = 0;
                int8_t rslt = bme69x_get_data(s_op_mode, data, &n_fields, &s_bme);
                if (rslt < BME69X_OK) {
                    consecutive_errors++;
                    ESP_LOGW(TAG, "bme69x_get_data failed: %d (%d/%d)", rslt, consecutive_errors,
                             kMaxConsecutiveErrors);
                } else {
                    consecutive_errors = 0;
                    bool got_output = false;
                    for (uint8_t i = 0; i < n_fields; i++) {
                        if (data[i].status & BME69X_GASM_VALID_MSK) {
                            got_output |= process_sample(now_ns, data[i], settings.process_data);
                        }
                    }
                    if (got_output) {
                        s_reading.state = EnvSensorState::Running;
                        if (last_log_us == 0 || esp_timer_get_time() - last_log_us >= kReadingLogIntervalUs) {
                            ESP_LOGI(TAG, "%.1fC %.0f%%rH %.1fhPa gas=%.0fohm IAQ=%.0f (acc %u) CO2eq=%.0fppm "
                                          "TVOCeq=%.0fppb stab=%d run_in=%d",
                                     s_reading.temperature_c, s_reading.humidity_pct, s_reading.pressure_hpa,
                                     s_reading.gas_resistance_ohm, s_reading.iaq, s_reading.iaq_accuracy,
                                     s_reading.co2_ppm, s_reading.tvoc_ppb, s_reading.stabilized,
                                     s_reading.run_in_done);
                            last_log_us = esp_timer_get_time();
                        }
                        if (s_on_update) s_on_update(s_reading);
                    }
                }
            }

            if (esp_timer_get_time() - last_save_us >= kStateSaveIntervalUs) {
                save_bsec_state();
                last_save_us = esp_timer_get_time();
            }
        }

        int64_t wait_ms = (settings.next_call - esp_timer_get_time() * 1000) / 1000000;
        TickType_t ticks = wait_ms > 0 ? pdMS_TO_TICKS(wait_ms) : 0;
        vTaskDelay(ticks > 0 ? ticks : 1);
    }

    ESP_LOGW(TAG, "BME69x stopped responding — saving state and re-probing");
    save_bsec_state();
}

static void env_sensor_task(void *arg) {
    (void)arg;
    i2c_master_bus_handle_t bus = board_get_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "No I2C bus (board_init() not called?) — sensor disabled");
        report_state(EnvSensorState::Error);
        vTaskDelete(nullptr);
        return;
    }

    bool logged_not_found = false;
    while (true) {
        uint8_t addr = probe_address(bus);
        if (addr == 0) {
            if (!logged_not_found) {
                ESP_LOGW(TAG, "No BME690 at %s — check wiring (P4 IO19=SDA, IO20=SCL; "
                              "P5 3.3V/GND). Re-probing every %lus.",
                         s_cfg.i2c_address ? (s_cfg.i2c_address == 0x76 ? "0x76" : "0x77") : "0x76/0x77",
                         (unsigned long)(kReprobeIntervalMs / 1000));
                logged_not_found = true;
            }
            s_reading.i2c_address = 0;
            report_state(EnvSensorState::NotFound);
            vTaskDelay(pdMS_TO_TICKS(kReprobeIntervalMs));
            continue;
        }
        logged_not_found = false;
        s_reading = {};
        s_reading.i2c_address = addr;

        if (!init_sensor(bus, addr) || !init_bsec()) {
            report_state(EnvSensorState::Error);
            release_device();
            vTaskDelay(pdMS_TO_TICKS(kReprobeIntervalMs));
            continue;
        }

        run_bsec_loop();
        release_device();
    }
}

static void ensure_nvs() {
    // wifi_manager_start() normally already did this, but it skips it when
    // no SSID is configured — BSEC state persistence shouldn't depend on
    // Wi-Fi being set up.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init failed (%s) — BSEC calibration won't persist", esp_err_to_name(err));
    }
}

void env_sensor_service_start(const EnvSensorConfig &cfg, EnvUpdateCb on_update) {
    s_cfg = cfg;
    s_on_update = std::move(on_update);

    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "env_sensor disabled in config");
        report_state(EnvSensorState::Disabled);
        return;
    }

    ensure_nvs();

    // 6KB: bsec_do_steps() plus the bme69x driver's locals and a few
    // stack-allocated BSEC input/output/state arrays; the 4KB work buffer is
    // static, not on this stack.
    //
    // Priority above weather_task (+3) and the LVGL task (4): BSEC wants
    // bsec_sensor_control() called on time (±a few %), and at +2 a weather
    // TLS handshake starved it into BSEC_W_SC_CALL_TIMING_VIOLATION (100)
    // on real hardware. The task sleeps almost all the time and each wake
    // is a few ms of I2C + bsec_do_steps(), so it costs the others nothing.
    if (xTaskCreate(env_sensor_task, "env_sensor_task", 6144, nullptr, tskIDLE_PRIORITY + 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed for env_sensor_task (out of memory?)");
        report_state(EnvSensorState::Error);
    }
}

#else  // !CRONOIQ_HAVE_BSEC

void env_sensor_service_start(const EnvSensorConfig &cfg, EnvUpdateCb on_update) {
    s_cfg = cfg;
    s_on_update = std::move(on_update);
    if (!s_cfg.enabled) {
        report_state(EnvSensorState::Disabled);
        return;
    }
    ESP_LOGW(TAG, "Built without Bosch BSEC (see components/bsec/CMakeLists.txt) — "
                  "BME690 support disabled");
    report_state(EnvSensorState::NoLibrary);
}

#endif
