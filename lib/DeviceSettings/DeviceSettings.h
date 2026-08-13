/**
 * @file    DeviceSettings.h
 * @brief   Reusable NVS-backed key/value settings store for ESP32 projects.
 *
 * A thin, self-contained wrapper over Arduino Preferences (NVS). It persists
 * the small set of device settings that survive reboots: the reusable ones that
 * every project wants (splash minimum hold, preferred board, mDNS hostname) and
 * any project-specific scalars a project chooses to store under the same
 * namespace.
 *
 * This module owns ONLY storage. The /setup web page that reads and writes
 * these values is owned by WebService, which composes a reusable settings block
 * with a project-injected block (mirroring how /status composes common fields
 * with a project status provider).
 *
 * All access goes through a single Preferences handle guarded by a mutex, so it
 * is safe to read at boot and write from a web-handler task concurrently.
 */

#pragma once
#include <Arduino.h>
#include <Preferences.h>

class DeviceSettings {
public:
    // Reusable setting keys (kept short; NVS keys are <= 15 chars).
    // Projects may add their own keys via the generic getters/setters below.
    static constexpr const char* KEY_SPLASH_HOLD = "splashHold";   // seconds
    static constexpr const char* KEY_DIAG_HOLD   = "diagHold";     // seconds
    static constexpr const char* KEY_BOARD       = "board";        // string id
    static constexpr const char* KEY_HOSTNAME    = "hostname";     // mDNS label

    // Open the NVS namespace. Call once early in setup(), before reads.
    void begin(const char* nvsNamespace = "device");

    // ---- Reusable settings (typed convenience accessors) ------------------
    // Splash minimum visible hold, in seconds (0 = animate only, no hold).
    uint16_t splashHoldSec(uint16_t def = 5);
    void     setSplashHoldSec(uint16_t sec);

    // Boot-diagnostics screen hold, in seconds (how long the per-subsystem
    // boot status stays up after the splash, before live content). 0 = skip.
    uint16_t diagHoldSec(uint16_t def = 3);
    void     setDiagHoldSec(uint16_t sec);

    // Preferred board identifier (free-form string the project interprets).
    String   board(const String& def = "");
    void     setBoard(const String& id);

    // mDNS hostname label (no ".local"). Empty = use the project default.
    String   hostname(const String& def = "");
    void     setHostname(const String& name);

    // ---- Generic accessors (for project-specific settings) ----------------
    // These let a project persist its own scalars under the same namespace
    // without adding a second store. Keys must be <= 15 characters.
    uint32_t getUInt (const char* key, uint32_t def = 0);
    void     setUInt (const char* key, uint32_t v);
    float    getFloat(const char* key, float def = 0.0f);
    void     setFloat(const char* key, float v);
    bool     getBool (const char* key, bool def = false);
    void     setBool (const char* key, bool v);
    String   getString(const char* key, const String& def = "");
    void     setString(const char* key, const String& v);

private:
    Preferences        _prefs;
    const char*        _ns      = "device";
    bool               _open    = false;
    SemaphoreHandle_t  _mutex   = nullptr;

    // Preferences is not re-entrant; each access opens/uses/closes under lock.
    void lock()   { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
    void unlock() { if (_mutex) xSemaphoreGive(_mutex); }
};
