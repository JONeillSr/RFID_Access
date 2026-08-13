#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>

/**
 * @file    SplashScreen.h
 * @brief   Generic animated boot-splash for SSD1306 displays.
 *
 * Slides a monochrome bitmap up from below the screen with an ease-out rise and
 * two spring-damped bounces, then (optionally) holds before returning so the
 * caller can show normal content. The animation is logo-agnostic: pass any
 * 1-bit bitmap (Adafruit_GFX drawBitmap format) and its dimensions.
 *
 * The bitmap is horizontally centered and comes to rest vertically centered by
 * default; an optional restY lets the caller place it elsewhere.
 *
 * Usage:
 *   SplashScreen splash(display, myLogo, LOGO_W, LOGO_H);
 *   splash.play(10000);     // animate, then hold 10 s
 *   splash.playAnimation(); // animate only, no hold (non-blocking convention)
 */
class SplashScreen {
public:
    /**
     * @param display  Already-initialized SSD1306 instance.
     * @param bitmap    1-bit bitmap in Adafruit_GFX drawBitmap format (PROGMEM).
     * @param width     Bitmap width in pixels.
     * @param height    Bitmap height in pixels.
     * @param restY     Final top-Y of the bitmap. Negative = auto (centered).
     */
    SplashScreen(Adafruit_SSD1306 &display, const uint8_t *bitmap,
                 uint16_t width, uint16_t height, int16_t restY = -1);

    /// Animate, then block for holdMs (default 10 s). Retained for projects
    /// that want the splash to stay up for a fixed time.
    void play(uint32_t holdMs = 10000);

    /// Animate only, no hold. Use at boot when the caller will replace the
    /// splash with normal content as soon as it's ready (non-blocking style).
    void playAnimation();

private:
    Adafruit_SSD1306 &_disp;
    const uint8_t    *_bitmap;
    uint16_t          _w;
    uint16_t          _h;
    int16_t           _x;       // centered x
    int16_t           _restY;   // resolved rest y

    void drawAt(int y);
    void runAnimation();
};
