#include "UnlockSchedule.h"
#include <Preferences.h>
#include <time.h>

UnlockSchedule unlockSchedule;

static Preferences prefs;

void UnlockSchedule::begin() {
    prefs.begin("sched", false);
    _enabled  = prefs.getBool("en", false);
    _startMin = prefs.getUShort("start", 0);
    _endMin   = prefs.getUShort("end", 0);
    _daysMask = prefs.getUChar("days", 0);
}

void UnlockSchedule::set(bool enabled, uint16_t startMin, uint16_t endMin,
                         uint8_t daysMask) {
    _enabled  = enabled;
    _startMin = min<uint16_t>(startMin, 1439);
    _endMin   = min<uint16_t>(endMin, 1439);
    _daysMask = daysMask & 0x7F;
    save();
}

void UnlockSchedule::save() {
    prefs.putBool("en", _enabled);
    prefs.putUShort("start", _startMin);
    prefs.putUShort("end", _endMin);
    prefs.putUChar("days", _daysMask);
}

bool UnlockSchedule::timeValid() {
    // Anything earlier than ~Nov 2023 means the clock never synced (the ESP32
    // boots at epoch 0). No trusted time -> the schedule stays inactive.
    return time(nullptr) > 1700000000;
}

bool UnlockSchedule::isActiveNow() const {
    if (!_enabled || _daysMask == 0 || _startMin == _endMin) return false;
    if (!timeValid()) return false;   // fail secure: unknown time = locked

    time_t    now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    uint16_t cur       = tm.tm_hour * 60 + tm.tm_min;
    uint8_t  today     = tm.tm_wday;            // 0 = Sunday
    uint8_t  yesterday = (today + 6) % 7;

    if (_startMin < _endMin)
        return ((_daysMask >> today) & 1) && cur >= _startMin && cur < _endMin;

    // Overnight window (e.g. 22:00-06:00): the evening half runs on the
    // start day's bit, the morning half on the same bit via "yesterday".
    return (((_daysMask >> today) & 1) && cur >= _startMin) ||
           (((_daysMask >> yesterday) & 1) && cur < _endMin);
}
