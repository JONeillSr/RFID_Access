/**
 * @file    Display.h
 * @brief   Reusable, application-agnostic SSD1306 OLED helper.
 *
 * Generic primitives for the common two-color 0.96" panels (yellow header band
 * on rows 0-15, blue body on rows 16-63):
 *   - begin()          : bring the panel up on an already-started I2C bus
 *   - showStatus()     : up to four plain lines (boot diagnostics, notices)
 *   - showStartup()    : simple titled splash
 *   - showMessage()    : titled status / error screen, one or two body lines
 *   - drawHeaderBand() : title in the yellow band with an underline
 *   - clear/display/raw: low-level access for custom screens
 *
 * Two design points worth keeping if this is ported:
 *
 *   1. The driver is held as a MEMBER, not a file-static pointer. That means no
 *      heap allocation, no one-instance-per-program limit, and raw() is always
 *      safe to call rather than dereferencing a pointer that may still be null.
 *
 *   2. The CALLER owns the I2C bus and must start it (Wire.begin(sda, scl))
 *      before calling begin(). The panel normally shares the bus with other
 *      devices, and the caller often needs board-specific setup — bus clock, a
 *      settle delay — that this library must not second-guess. See the
 *      periphBegin note in Display.cpp for the concrete ESP32-C6 failure this
 *      avoids.
 *
 * Project-specific screens (a filament record, a temperature readout) belong in
 * the application, drawn via raw() or these primitives, so this module stays
 * reusable across projects.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include <Adafruit_SSD1306.h>

class Display {
public:
    Display(uint8_t width, uint8_t height, int8_t resetPin);

    /**
     * Initialize the panel on an already-started I2C bus.
     * @param wire    Reference to the active TwoWire bus (caller called begin()).
     * @param i2cAddr 7-bit I2C address (commonly 0x3C).
     * @return true if the controller acknowledged.
     */
    bool begin(TwoWire &wire, uint8_t i2cAddr);

    /// Up to four plain lines from the top of the panel. Empty lines are
    /// skipped. The general-purpose screen: boot diagnostics, notices, any
    /// "several short lines of text" case.
    void showStatus(const String &l1, const String &l2 = "",
                    const String &l3 = "", const String &l4 = "");

    /// Show a brief startup splash with the project name.
    void showStartup(const String &title);

    /// Display a short status / error message in the body region.
    void showMessage(const String &title, const String &message);

    /// Display a titled screen with two body lines (e.g. hostname + IP).
    /// Either line may be empty to show just one.
    void showMessage2(const String &title, const String &line1,
                      const String &line2);

    /// Draw a title in the yellow band (rows 0-15) with an underline.
    /// Public so application screens can reuse the same header style.
    void drawHeaderBand(const String &title);

    /// Clear the back buffer (does not push to the panel; call display()).
    void clear();

    /// Push the back buffer to the panel.
    void display();

    /// True once begin() has succeeded.
    bool isReady() const { return _ready; }

    /// Access the underlying SSD1306 driver for custom drawing (boot logo,
    /// project-specific screens). Held as a member, so this reference is always
    /// valid; drawing through it before a successful begin() is simply a no-op
    /// on the panel rather than a crash.
    Adafruit_SSD1306 &raw() { return _oled; }

private:
    Adafruit_SSD1306 _oled;
    uint8_t          _width;
    uint8_t          _height;
    bool             _ready;
};

#endif  // DISPLAY_H
