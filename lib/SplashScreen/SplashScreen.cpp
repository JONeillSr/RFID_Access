/**
 * @file    SplashScreen.cpp
 * @brief   Implementation of the generic animated boot-splash.
 */

#include "SplashScreen.h"

SplashScreen::SplashScreen(Adafruit_SSD1306 &display, const uint8_t *bitmap,
                           uint16_t width, uint16_t height, int16_t restY)
    : _disp(display), _bitmap(bitmap), _w(width), _h(height) {
    // Center horizontally within the panel.
    int16_t pw = _disp.width();
    int16_t ph = _disp.height();
    _x = (pw > _w) ? (int16_t)((pw - _w) / 2) : 0;
    // Resolve rest position: caller value, or vertically centered if negative.
    _restY = (restY >= 0) ? restY
                          : (int16_t)((ph > _h) ? (ph - _h) / 2 : 0);
}

void SplashScreen::drawAt(int y) {
    _disp.clearDisplay();
    _disp.drawBitmap(_x, y, _bitmap, _w, _h, SSD1306_WHITE);
    _disp.display();
}

// Shared slide-up + two spring-damped bounces, ending at the rest position.
// The short per-frame delays are intrinsic to the animation timing; this runs
// once at boot before the application tasks start.
void SplashScreen::runAnimation() {
    const int startY = _disp.height();   // start just below the visible area
    const int restY  = _restY;

    // Ease-out rise from below the screen.
    int y = startY;
    while (y > restY) {
        int step = max(1, (y - restY) / 3);
        y -= step;
        drawAt(y);
        delay(16);
    }

    // First overshoot — spring effect.
    const int OV1 = 5;
    for (int i = 1; i <= OV1; i++) { drawAt(restY - i); delay(15); }
    for (int i = OV1; i >= 0;  i--) { drawAt(restY - i); delay(20); }

    // Second overshoot — damped.
    const int OV2 = 2;
    for (int i = 1; i <= OV2; i++) { drawAt(restY - i); delay(22); }
    for (int i = OV2; i >= 0;  i--) { drawAt(restY - i); delay(28); }

    // Settle at rest.
    drawAt(restY);
}

void SplashScreen::playAnimation() {
    runAnimation();
}

void SplashScreen::play(uint32_t holdMs) {
    runAnimation();

    // Hold at rest — short chunks so the task watchdog doesn't fire during a
    // long hold. (Blocking; for callers that want a fixed-time splash.)
    if (holdMs > 0) {
        uint32_t start = millis();
        while (millis() - start < holdMs)
            delay(100);
    }
}
