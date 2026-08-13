/**
 * @file    DeviceSettings.cpp
 * @brief   Implementation of the NVS-backed settings store.
 */

#include "DeviceSettings.h"

void DeviceSettings::begin(const char* nvsNamespace) {
    _ns = nvsNamespace;
    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    _open = true;
}

// ---- reusable typed accessors ----------------------------------------------

uint16_t DeviceSettings::splashHoldSec(uint16_t def) {
    return (uint16_t)getUInt(KEY_SPLASH_HOLD, def);
}
void DeviceSettings::setSplashHoldSec(uint16_t sec) {
    setUInt(KEY_SPLASH_HOLD, sec);
}

uint16_t DeviceSettings::diagHoldSec(uint16_t def) {
    return (uint16_t)getUInt(KEY_DIAG_HOLD, def);
}
void DeviceSettings::setDiagHoldSec(uint16_t sec) {
    setUInt(KEY_DIAG_HOLD, sec);
}

String DeviceSettings::board(const String& def)       { return getString(KEY_BOARD, def); }
void   DeviceSettings::setBoard(const String& id)     { setString(KEY_BOARD, id); }

String DeviceSettings::hostname(const String& def)    { return getString(KEY_HOSTNAME, def); }
void   DeviceSettings::setHostname(const String& name){ setString(KEY_HOSTNAME, name); }

// ---- generic accessors ------------------------------------------------------
// Each opens the namespace, performs one operation, and closes it, all under
// the mutex. Opening per-call (rather than holding the handle open) keeps NVS
// writes flushed and avoids cross-task handle sharing.

uint32_t DeviceSettings::getUInt(const char* key, uint32_t def) {
    if (!_open) return def;
    lock();
    _prefs.begin(_ns, /*readOnly=*/true);
    uint32_t v = _prefs.getUInt(key, def);
    _prefs.end();
    unlock();
    return v;
}
void DeviceSettings::setUInt(const char* key, uint32_t v) {
    if (!_open) return;
    lock();
    _prefs.begin(_ns, false);
    _prefs.putUInt(key, v);
    _prefs.end();
    unlock();
}

float DeviceSettings::getFloat(const char* key, float def) {
    if (!_open) return def;
    lock();
    _prefs.begin(_ns, true);
    float v = _prefs.getFloat(key, def);
    _prefs.end();
    unlock();
    return v;
}
void DeviceSettings::setFloat(const char* key, float v) {
    if (!_open) return;
    lock();
    _prefs.begin(_ns, false);
    _prefs.putFloat(key, v);
    _prefs.end();
    unlock();
}

bool DeviceSettings::getBool(const char* key, bool def) {
    if (!_open) return def;
    lock();
    _prefs.begin(_ns, true);
    bool v = _prefs.getBool(key, def);
    _prefs.end();
    unlock();
    return v;
}
void DeviceSettings::setBool(const char* key, bool v) {
    if (!_open) return;
    lock();
    _prefs.begin(_ns, false);
    _prefs.putBool(key, v);
    _prefs.end();
    unlock();
}

String DeviceSettings::getString(const char* key, const String& def) {
    if (!_open) return def;
    lock();
    _prefs.begin(_ns, true);
    String v = _prefs.getString(key, def);
    _prefs.end();
    unlock();
    return v;
}
void DeviceSettings::setString(const char* key, const String& v) {
    if (!_open) return;
    lock();
    _prefs.begin(_ns, false);
    _prefs.putString(key, v);
    _prefs.end();
    unlock();
}
