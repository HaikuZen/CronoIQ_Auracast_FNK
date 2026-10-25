# Wraps Bosch's BSEC 3.x release (libalgobsec.a + headers + BME690 IAQ
# config) and the BSD-3 bme69x SensorAPI driver shipped inside it, WITHOUT
# copying any of it into this repo: libalgobsec.a and its headers are
# proprietary (Bosch's license forbids redistributing them), so they stay in
# the user's own unpacked BSEC download and are referenced from there.
#
# Where to find it, first match wins:
#   1. -DBSEC_DIR=/path/to/bsec_v3   (idf.py -DBSEC_DIR=... build)
#   2. $BSEC_DIR environment variable
#   3. ../bsec_v3 next to this project's parent directory (the layout this
#      project was developed with: ~/Scaricati/bsec_v3 alongside
#      ~/Scaricati/CYD/CronoIQ_Auracast_FNK)
#
# If none of those exist the component still registers (empty) and the app
# builds without BSEC — env_sensor_service.cpp then reports "library not
# available" on the Indoor page instead of failing the whole build.

if(NOT BSEC_DIR)
    if(DEFINED ENV{BSEC_DIR})
        set(BSEC_DIR "$ENV{BSEC_DIR}")
    else()
        get_filename_component(BSEC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../bsec_v3" ABSOLUTE)
    endif()
endif()

# BME690, 3.3V supply (the FNK0115's P5 connector), LP mode (3s sample
# interval), 4-day calibration window. Must match BSEC_SAMPLE_RATE_LP in
# env_sensor_service.cpp — a config/sample-rate mismatch makes
# bsec_update_subscription() fail.
set(BSEC_CONFIG_NAME "bme690_iaq_33v_3s_4d")

set(BSEC_LIB      "${BSEC_DIR}/release_bin/IAQ/bin/esp/esp32_s3/libalgobsec.a")
set(BSEC_INC      "${BSEC_DIR}/release_bin/IAQ/inc")
set(BSEC_CFG_DIR  "${BSEC_DIR}/release_bin/IAQ/config/bme690/${BSEC_CONFIG_NAME}")
set(BME69X_DIR    "${BSEC_DIR}/examples/BSEC_Integration_Examples/src/bme69x")

if(EXISTS "${BSEC_LIB}" AND EXISTS "${BSEC_INC}/bsec_interface.h"
        AND EXISTS "${BSEC_CFG_DIR}/bsec_iaq.c" AND EXISTS "${BME69X_DIR}/bme69x.c")
    message(STATUS "BSEC: using ${BSEC_DIR} (config ${BSEC_CONFIG_NAME})")
    idf_component_register(
        SRCS
            "${BME69X_DIR}/bme69x.c"
            "${BSEC_CFG_DIR}/bsec_iaq.c"
        INCLUDE_DIRS
            "${BSEC_INC}"
            "${BME69X_DIR}"
            "${BSEC_CFG_DIR}"
    )
    # Vendor sources — don't let their warnings fail this project's -Werror
    # build.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)
    add_prebuilt_library(algobsec "${BSEC_LIB}")
    target_link_libraries(${COMPONENT_LIB} PUBLIC algobsec)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CRONOIQ_HAVE_BSEC=1)
else()
    message(WARNING "BSEC: not found at ${BSEC_DIR} — building without the "
                    "BME690 air-quality sensor. Pass -DBSEC_DIR=/path/to/bsec_v3 "
                    "or set the BSEC_DIR environment variable to enable it.")
    idf_component_register()
endif()
