/**
 * @file    Display.cpp
 * @brief   Implementation of the reusable, application-agnostic SSD1306 helper.
 */

#include "Display.h"

Display::Display(uint8_t width, uint8_t height, int8_t resetPin)
    : _oled(width, height, &Wire, resetPin),
      _width(width),
      _height(height),
      _ready(false) {}

bool Display::begin(TwoWire &wire, uint8_t i2cAddr) {
    // Re-bind the controller to the supplied bus, then attempt allocation.
    // Reset pin -1 means the panel shares the host reset (typical for these
    // 4-pin I2C OLED modules).
    //
    // The 4th begin() arg (periphBegin) is false: the caller is responsible for
    // having already called wire.begin(SDA, SCL). If left true, Adafruit's
    // begin() calls Wire.begin() with NO pin args, which on the ESP32-C6 resets
    // the bus to the default I2C pins and discards an explicit 22/23 binding --
    // the panel then ACKs against a stale state but never renders. Passing false
    // keeps the bus on the pins the caller configured.
    //
    // This is easy to get wrong because it only bites when the configured pins
    // differ from the core's variant defaults: a build that happens to use the
    // defaults works fine and hides the bug until someone moves the panel.
    _oled = Adafruit_SSD1306(_width, _height, &wire, -1);
    _ready = _oled.begin(SSD1306_SWITCHCAPVCC, i2cAddr, /*reset=*/true,
                         /*periphBegin=*/false);
    if (_ready) {
        _oled.clearDisplay();
        _oled.display();
    }
    return _ready;
}

void Display::drawHeaderBand(const String &title) {
    // Title sits in the top yellow band of the two-color panel.
    _oled.setTextColor(SSD1306_WHITE);
    _oled.setTextSize(1);
    _oled.setCursor(0, 4);
    _oled.print(title);
    // Underline separating header band from body.
    _oled.drawFastHLine(0, 15, _width, SSD1306_WHITE);
}

void Display::showStatus(const String &l1, const String &l2,
                         const String &l3, const String &l4) {
    if (!_ready) return;

    _oled.clearDisplay();
    _oled.setTextColor(SSD1306_WHITE);
    _oled.setTextSize(1);
    _oled.setCursor(0, 0);
    // Empty lines are skipped rather than printed blank, so a caller can pass
    // only the lines it has without leaving gaps.
    if (l1.length()) _oled.println(l1);
    if (l2.length()) _oled.println(l2);
    if (l3.length()) _oled.println(l3);
    if (l4.length()) _oled.println(l4);
    _oled.display();
}

void Display::showStartup(const String &title) {
    if (!_ready) return;

    _oled.clearDisplay();
    drawHeaderBand(title);

    _oled.setTextSize(1);
    _oled.setCursor(0, 28);
    _oled.print(F("Starting up..."));
    _oled.display();
}

void Display::showMessage(const String &title, const String &message) {
    if (!_ready) return;

    _oled.clearDisplay();
    drawHeaderBand(title);
    _oled.setTextSize(1);
    _oled.setCursor(0, 28);
    _oled.print(message);
    _oled.display();
}

void Display::showMessage2(const String &title, const String &line1,
                           const String &line2) {
    if (!_ready) return;

    _oled.clearDisplay();
    drawHeaderBand(title);
    _oled.setTextSize(1);
    // Two body lines in the blue region, comfortably spaced.
    _oled.setCursor(0, 26);
    _oled.print(line1);
    _oled.setCursor(0, 42);
    _oled.print(line2);
    _oled.display();
}

void Display::clear() {
    if (_ready) _oled.clearDisplay();
}

void Display::display() {
    if (_ready) _oled.display();
}
