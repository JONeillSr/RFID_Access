/**
 * @file    BoardConfig.h
 * @brief   Reusable, application-agnostic board identity + capability flags.
 *
 * This header answers "what chip am I, and what are its fixed facts?" — the
 * things that are true for a given ESP32 variant regardless of which project
 * is using it: whether Wire1 is usable, whether it has native USB, the default
 * I2C pins, the onboard LED, and which GPIOs are unsafe to use as outputs
 * because they are strapping or flash pins.
 *
 * It deliberately holds NO project peripheral assignments (relay, RC522, etc.).
 * Those are per-project and live in the project's own pin map (e.g. src/Pins.h),
 * which includes this file for the board facts it needs.
 *
 * Board selection is by the -D BOARD_* flag set per environment in
 * platformio.ini. Exactly one must be defined.
 *
 * Capability flags exposed (per board):
 *   BOARD_NAME              human-readable string for logs / status pages
 *   BOARD_HAS_WIRE1         1 if the second I2C peripheral is usable
 *   BOARD_HAS_NATIVE_USB    1 if USB CDC is the native serial (drops on reboot)
 *   BOARD_I2C_SDA_DEFAULT   sensible default I2C SDA GPIO for this board
 *   BOARD_I2C_SCL_DEFAULT   sensible default I2C SCL GPIO for this board
 *   BOARD_LED_PIN           onboard LED GPIO (-1 if none / unknown)
 *   BOARD_LED_ACTIVE_LOW    1 if the onboard LED lights when driven LOW
 *
 * The STRAPPING / UNSAFE notes in each block are documentation, not enforced
 * in code — they are here so that whoever assigns peripheral pins in the
 * project header knows which GPIOs to avoid for boot-critical outputs (like a
 * fail-secure relay).
 */

#ifndef BOARDCONFIG_H
#define BOARDCONFIG_H

#include <Arduino.h>

// ============================================================================
//  Seeed XIAO ESP32-C6
// ============================================================================
#if defined(BOARD_XIAO_ESP32C6)
    #define BOARD_NAME              "XIAO ESP32-C6"
    // Wire1 is BROKEN on the C6 (espressif/arduino-esp32 #10685): initialising
    // the second I2C peripheral can wedge the bus. Keep everything on Wire.
    #define BOARD_HAS_WIRE1         0
    // Native USB CDC: serial drops on reboot and re-enumerates too slowly to
    // catch boot messages. Diagnostics must go to OLED / web, not Serial.
    #define BOARD_HAS_NATIVE_USB    1
    #define BOARD_I2C_SDA_DEFAULT   22
    #define BOARD_I2C_SCL_DEFAULT   23
    #define BOARD_LED_PIN           15      // onboard user LED
    #define BOARD_LED_ACTIVE_LOW    1
    // STRAPPING / boot-sensitive: GPIO4, GPIO5, GPIO8, GPIO9, GPIO15. GPIO1 has
    // been bench-verified clean as a fail-secure relay output on this board.
    // Flash: GPIO24-30. USB: GPIO12/13.

// ============================================================================
//  ESP32-C3 (generic devkit)
// ============================================================================
#elif defined(BOARD_ESP32C3)
    #define BOARD_NAME              "ESP32-C3"
    // C3 has a single usable I2C controller exposed in Arduino; treat as no
    // Wire1 for portability (the core maps Wire only by default here).
    #define BOARD_HAS_WIRE1         0
    #define BOARD_HAS_NATIVE_USB    1       // USB Serial/JTAG, drops on reboot
    #define BOARD_I2C_SDA_DEFAULT   8
    #define BOARD_I2C_SCL_DEFAULT   9
    #define BOARD_LED_PIN           8       // many C3 boards: GPIO8 (active-low)
    #define BOARD_LED_ACTIVE_LOW    1
    // STRAPPING / boot-sensitive: GPIO2, GPIO8, GPIO9. NOTE: GPIO8/9 are BOTH
    // strapping AND a common LED pin — avoid for boot-critical outputs.
    // Flash: GPIO12-17 (do not use). USB-JTAG: GPIO18, GPIO19.

// ============================================================================
//  ESP32-S3 (generic devkitc-1)
// ============================================================================
#elif defined(BOARD_ESP32S3)
    #define BOARD_NAME              "ESP32-S3"
    #define BOARD_HAS_WIRE1         1       // second I2C peripheral works
    #define BOARD_HAS_NATIVE_USB    1       // native USB (CDC on boot optional)
    #define BOARD_I2C_SDA_DEFAULT   8
    #define BOARD_I2C_SCL_DEFAULT   9
    #define BOARD_LED_PIN           48      // common WS2812 / LED pin on devkitc
    #define BOARD_LED_ACTIVE_LOW    0
    // STRAPPING / boot-sensitive: GPIO0, GPIO3, GPIO45, GPIO46.
    // Flash/PSRAM: GPIO26-32 (and 33-37 on octal-PSRAM parts) — do not use.
    // USB: GPIO19, GPIO20. Input-only: none (all S3 GPIO are bidirectional).

// ============================================================================
//  Classic ESP32 (DOIT DevKit V1 / WROOM-32)
// ============================================================================
#elif defined(BOARD_ESP32DEV)
    #define BOARD_NAME              "ESP32 DevKit V1"
    #define BOARD_HAS_WIRE1         1       // second I2C peripheral works
    #define BOARD_HAS_NATIVE_USB    0       // external USB-UART (CP2102/CH340):
                                            // serial survives reboot, watchable
    #define BOARD_I2C_SDA_DEFAULT   21
    #define BOARD_I2C_SCL_DEFAULT   22
    #define BOARD_LED_PIN           2       // onboard blue LED (also strapping!)
    #define BOARD_LED_ACTIVE_LOW    0
    // STRAPPING / boot-sensitive: GPIO0, GPIO2, GPIO5, GPIO12, GPIO15.
    //   - GPIO12 (MTDI): must be LOW at boot or the chip mis-sets flash voltage.
    //   - GPIO2 doubles as the onboard LED; fine to blink, avoid for relay.
    // Flash: GPIO6-11 (do not use). Input-only (no output, no pullup):
    //   GPIO34, 35, 36, 39 — never use for a relay or LED.

#else
    #error "BoardConfig.h: no known BOARD_* defined. Set one in platformio.ini build_flags (e.g. -D BOARD_XIAO_ESP32C6)."
#endif

#endif  // BOARDCONFIG_H
