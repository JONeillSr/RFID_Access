/**
 * @file    UnlockSchedule.h
 * @brief   Timed unlock window ("hold the door open 08:00-17:00 weekdays").
 *
 * The schedule is a start/end time (minutes of local day) plus a days-of-week
 * mask, persisted in NVS. isActiveNow() answers "should the door be held
 * unlocked right now?" and FAILS SECURE: it returns false unless the system
 * clock has been NTP-synced, so a reboot with no network keeps the door
 * locked rather than guessing at the time.
 *
 * Overnight windows (start > end, e.g. 22:00-06:00) are supported; the
 * window belongs to the day it starts on.
 *
 * The single instance `unlockSchedule` is shared by main (relay control) and
 * WebHandlers (config API). Fields are small scalars, so cross-task access
 * is benign.
 */

#pragma once
#include <Arduino.h>

class UnlockSchedule {
public:
    void begin();   // load persisted schedule from NVS; call once in setup()

    /// Update and persist. Minutes are clamped to 0-1439; daysMask bit 0 =
    /// Sunday ... bit 6 = Saturday.
    void set(bool enabled, uint16_t startMin, uint16_t endMin, uint8_t daysMask);

    bool     enabled()  const { return _enabled; }
    uint16_t startMin() const { return _startMin; }
    uint16_t endMin()   const { return _endMin; }
    uint8_t  daysMask() const { return _daysMask; }

    /// True once the system clock holds a plausible NTP-synced time.
    static bool timeValid();

    /// True when the door should currently be held unlocked.
    bool isActiveNow() const;

private:
    void save();

    bool     _enabled  = false;
    uint16_t _startMin = 0;
    uint16_t _endMin   = 0;
    uint8_t  _daysMask = 0;
};

extern UnlockSchedule unlockSchedule;
