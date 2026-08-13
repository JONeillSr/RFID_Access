/**
 * @file    Pins.h
 * @brief   Project peripheral pin map for the RFID Access Control firmware.
 *
 * This is the PROJECT-SPECIFIC half of the hardware configuration: which GPIO
 * each peripheral (Paxton reader, relay, buzzer, OLED, RGB status LED) lands
 * on, for each supported board. It includes BoardConfig.h for the board-identity facts
 * (Wire1 availability, default I2C pins, strapping-pin warnings) and builds the
 * peripheral assignments on top.
 *
 * main.cpp and the modules reference ONLY the names defined here — never a raw
 * GPIO number — so retargeting to a different board is just a platformio.ini
 * environment switch.
 *
 * ──────────────────────────────────────────────────────────────────────────
 *  VERIFICATION STATUS — READ BEFORE TRUSTING ON HARDWARE
 *  - XIAO ESP32-C6: FULLY BENCH-VERIFIED, including Paxton P50 reads
 *    end-to-end (relay fail-secure confirmed clean through reboot with a
 *    meter). NOTE: the data/clock 10K pull-ups must reference a stiff 3.3V
 *    rail (dedicated buck), NOT the ESP32 module's 3V3 pin — see README.
 *  - ESP32-C3 / ESP32-S3 / ESP32 DevKit V1: pins are PLAUSIBLE SAFE DEFAULTS
 *    chosen to avoid strapping/flash pins, but are NOT yet hardware-verified.
 *    Confirm the relay pin specifically shows no spurious pulse at boot before
 *    deploying to a real door. Adjust below as needed.
 * ──────────────────────────────────────────────────────────────────────────
 */

#ifndef PINS_H
#define PINS_H

#include "BoardConfig.h"

// ============================================================================
//  Seeed XIAO ESP32-C6   (BENCH-VERIFIED)
// ============================================================================
#if defined(BOARD_XIAO_ESP32C6)
    // Paxton P-series reader (Clock & Data / Wiegand). D-aliases are valid on
    // the XIAO variant. DATA/CLOCK are open-collector inputs — fit external
    // 10K pull-ups to 3V3 for long cable runs (internal pull-ups also enabled).
    #define PIN_PAXTON_DATA     D8      // reader Data/D0 line
    #define PIN_PAXTON_CLOCK    D9      // reader Clock/D1 line
    // Reader LED lines: active-low, driven open-drain (sink to light). Check
    // each line idles at or below 3.3V before wiring direct; else use an NPN.
    #define PIN_PAXTON_LED_R    D3      // reader Red LED
    #define PIN_PAXTON_LED_G    D10     // reader Green LED
    #define PIN_PAXTON_LED_A    D0      // reader Amber LED

    #define PIN_BUZZER      D2
    #define PIN_RELAY       1       // GPIO1 — verified clean at boot (fail-secure)

    #define PIN_OLED_SDA    BOARD_I2C_SDA_DEFAULT   // 22
    #define PIN_OLED_SCL    BOARD_I2C_SCL_DEFAULT   // 23

    // Exit button (request-to-exit): push-to-make between this pin and GND.
    // The C6 has no free pad left, so this repurposes D6 from the panel LED's
    // red channel — the reader's own red LED shows "denied" at the door, so
    // the panel red was redundant.
    #define PIN_EXIT_BTN    D6      // GPIO16

    // Panel status LED (common-cathode assumed; see RGB_ACTIVE_LOW below).
    #define PIN_LED_R       -1      // repurposed for the exit button
    #define PIN_LED_G       D7      // GPIO17 — lights while door unlocked
    #define PIN_LED_B       -1      // no free pin

// ============================================================================
//  ESP32-C3 generic   (UNVERIFIED defaults — avoid 2/8/9/12-19)
// ============================================================================
#elif defined(BOARD_ESP32C3)
    #define PIN_PAXTON_DATA     5
    #define PIN_PAXTON_CLOCK    4
    #define PIN_PAXTON_LED_R    7
    #define PIN_PAXTON_LED_G    6
    #define PIN_PAXTON_LED_A    10
    #define PIN_BUZZER      0
    #define PIN_RELAY       1       // not strapping; verify clean at boot
    #define PIN_OLED_SDA    BOARD_I2C_SDA_DEFAULT   // 8
    #define PIN_OLED_SCL    BOARD_I2C_SCL_DEFAULT   // 9
    #define PIN_LED_R       2
    #define PIN_LED_G       3
    #define PIN_LED_B       20
    #define PIN_EXIT_BTN    21      // shares UART0 TX — serial unusable when wired

// ============================================================================
//  ESP32-S3 generic   (UNVERIFIED defaults — avoid 0/3/45/46, 19/20, 26-37)
// ============================================================================
#elif defined(BOARD_ESP32S3)
    #define PIN_PAXTON_DATA     13
    #define PIN_PAXTON_CLOCK    12
    #define PIN_PAXTON_LED_R    10
    #define PIN_PAXTON_LED_G    11
    #define PIN_PAXTON_LED_A    9
    #define PIN_BUZZER      4
    #define PIN_RELAY       5       // not strapping; verify clean at boot
    #define PIN_OLED_SDA    BOARD_I2C_SDA_DEFAULT   // 8
    #define PIN_OLED_SCL    BOARD_I2C_SCL_DEFAULT   // 9
    #define PIN_LED_R       6
    #define PIN_LED_G       7
    #define PIN_LED_B       15
    #define PIN_EXIT_BTN    16

// ============================================================================
//  Classic ESP32 DevKit V1   (UNVERIFIED defaults — avoid 0/2/5/12/15, 6-11,
//  and input-only 34/35/36/39)
// ============================================================================
#elif defined(BOARD_ESP32DEV)
    #define PIN_PAXTON_DATA     19
    #define PIN_PAXTON_CLOCK    18
    #define PIN_PAXTON_LED_R    17
    #define PIN_PAXTON_LED_G    23
    #define PIN_PAXTON_LED_A    4
    #define PIN_BUZZER      13
    #define PIN_RELAY       14      // not strapping; verify clean at boot
    #define PIN_OLED_SDA    BOARD_I2C_SDA_DEFAULT   // 21
    #define PIN_OLED_SCL    BOARD_I2C_SCL_DEFAULT   // 22
    #define PIN_LED_R       25
    #define PIN_LED_G       26
    #define PIN_LED_B       27
    #define PIN_EXIT_BTN    32

#endif

// ────────────────────────────────────────────────────────────────────────────
//  Cross-board peripheral facts (not pins)
// ────────────────────────────────────────────────────────────────────────────

// OLED I2C address (same on every board for these panels).
#define OLED_ADDR   0x3C
#define OLED_W      128
#define OLED_H      64

// RGB status LED drive polarity. Common-cathode = active-HIGH (drive a channel
// HIGH to light it). Set to 1 if you wire a common-anode LED instead.
#ifndef RGB_ACTIVE_LOW
#define RGB_ACTIVE_LOW  0
#endif

// True only when all three RGB channels are assigned a real pin. Lets the
// firmware skip blue (or the whole LED) gracefully on a board where a channel
// couldn't be allocated (e.g. the C6 currently has PIN_LED_B == -1).
#define HAS_RGB_FULL   (PIN_LED_R >= 0 && PIN_LED_G >= 0 && PIN_LED_B >= 0)

#endif  // PINS_H
