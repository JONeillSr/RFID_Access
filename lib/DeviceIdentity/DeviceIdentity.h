/**
 * @file    DeviceIdentity.h
 * @brief   Per-unit identity for fleet deployments: a stable hardware-derived
 *          device ID plus operator-assigned location labels.
 *
 * Solves the problem that one firmware image flashed to many units gives them
 * all the same name. Identity here comes from hardware and NVS, never from a
 * build flag, so the same binary can be flashed to every door:
 *
 *   deviceId()  immutable and unique, derived from the efuse MAC. Needs no
 *               provisioning and survives reflashing and an NVS erase, which
 *               makes it the right key for a backend to identify a door by.
 *   hostname()  the mDNS label: the operator-set name if one was saved, else
 *               deviceId(). Two units are therefore never identical out of the
 *               box, so a default-configuration name collision is impossible.
 *   doorName()  human labels for reports and UI. Free-form; empty until set.
 *   siteName()
 *
 * Labels persist through DeviceSettings (same NVS namespace — no second store).
 * This module owns only identity; the /setup page that edits these values is
 * composed by WebService, the same way it composes /status.
 */

#pragma once
#include <Arduino.h>
#include "DeviceSettings.h"

class DeviceIdentity {
public:
    // NVS keys. Must stay <= 15 characters (an NVS limit).
    static constexpr const char* KEY_DOOR_NAME = "doorName";
    static constexpr const char* KEY_SITE_NAME = "siteName";

    /**
     * Bind to a settings store and compute the device ID.
     *
     * @param settings  An already-begun() DeviceSettings instance. Not owned.
     * @param prefix    Short project tag leading the device ID, e.g. "rfid"
     *                  yields "rfid-a1b2c3". Sanitised like any other label.
     */
    void begin(DeviceSettings* settings, const char* prefix = "dev");

    /// Immutable hardware identity, e.g. "rfid-a1b2c3". Valid after begin().
    const String& deviceId() const { return _deviceId; }

    /// mDNS label (no ".local"): the saved hostname when set and usable,
    /// otherwise deviceId().
    String hostname() const;

    /// Operator-assigned labels. Empty when never set.
    String doorName() const;
    String siteName() const;
    void   setDoorName(const String& v);
    void   setSiteName(const String& v);

    /// Best available human label: doorName() if set, else hostname().
    String displayName() const;

    /**
     * Coerce arbitrary text into a valid DNS/mDNS label: lowercased, only
     * [a-z0-9-], runs of separators collapsed to a single '-', no leading or
     * trailing '-', clamped to 63 characters. Returns "" if nothing usable
     * remains.
     *
     * Worth applying to anything an operator types. A space or underscore in a
     * hostname breaks mDNS resolution silently — the device comes up fine and
     * simply never answers to its name, which is painful to diagnose from the
     * device side.
     */
    static String sanitizeLabel(const String& in);

private:
    DeviceSettings* _settings = nullptr;   // not owned
    String          _deviceId;
};
