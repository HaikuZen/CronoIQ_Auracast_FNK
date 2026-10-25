#pragma once

#include <cstddef>

#include "env_sensor_service.h"

// In-RAM (PSRAM) history of BME690 readings for the Indoor page's graphs.
// Two resolutions, each a ring of averaged buckets:
//   Hours3  — 1-minute averages, 180 points
//   Hours24 — 5-minute averages, 288 points
// Deliberately not persisted: history starts empty after every reboot, and
// avoiding periodic flash writes avoids the RGB-panel DMA underruns they
// cause (see CLAUDE.md). Time is uptime-based, so it works before NTP sync.

enum class EnvMetric { Temperature, Humidity, Pressure, Iaq, Co2, Count };
enum class EnvRange { Hours3, Hours24 };

// Allocates the rings. Call once at startup, before env_history_add().
void env_history_init(void);

// Folds one reading into the current buckets. Only Running readings count;
// IAQ and CO2-eq are recorded only once BSEC's IAQ accuracy is >= 1 (before
// that BSEC reports placeholder values). Thread-safe; called from the
// sensor task.
void env_history_add(const EnvReading &reading);

// Number of points a range always returns.
size_t env_history_points(EnvRange range);

// Fills `out` (env_history_points(range) entries, oldest first, the last
// one being the current partial bucket, i.e. "now") with the metric's
// averages; NAN marks buckets with no data. Temperature is in °C, pressure
// in hPa. Thread-safe; called from the LVGL task.
void env_history_get(EnvMetric metric, EnvRange range, float *out);
