/**
 * @file    DeviceIdentity.cpp
 * @brief   Implementation of the per-unit identity helper.
 */

#include "DeviceIdentity.h"

void DeviceIdentity::begin(DeviceSettings* settings, const char* prefix) {
    _settings = settings;

    // ESP.getEfuseMac() writes the six MAC bytes into the low six bytes of a
    // little-endian uint64, so MAC byte i sits at bit 8*i. Only the last three
    // bytes are per-unit — the first three are the OUI and are identical on
    // every ESP32 — so those are what make a six-hex-digit suffix unique.
    const uint64_t mac = ESP.getEfuseMac();
    char buf[40];
    snprintf(buf, sizeof(buf), "%s-%02x%02x%02x",
             prefix ? prefix : "dev",
             (unsigned)((mac >> 24) & 0xFF),
             (unsigned)((mac >> 32) & 0xFF),
             (unsigned)((mac >> 40) & 0xFF));

    // Sanitised so a careless prefix can't produce an invalid mDNS label.
    _deviceId = sanitizeLabel(String(buf));
}

String DeviceIdentity::hostname() const {
    if (_settings) {
        // Sanitise on read as well as on write: a value could predate this
        // module, or have been written by an older firmware.
        String saved = sanitizeLabel(_settings->hostname(""));
        if (saved.length()) return saved;
    }
    return _deviceId;
}

String DeviceIdentity::doorName() const {
    return _settings ? _settings->getString(KEY_DOOR_NAME, "") : String("");
}

String DeviceIdentity::siteName() const {
    return _settings ? _settings->getString(KEY_SITE_NAME, "") : String("");
}

void DeviceIdentity::setDoorName(const String& v) {
    if (_settings) _settings->setString(KEY_DOOR_NAME, v);
}

void DeviceIdentity::setSiteName(const String& v) {
    if (_settings) _settings->setString(KEY_SITE_NAME, v);
}

String DeviceIdentity::displayName() const {
    String d = doorName();
    return d.length() ? d : hostname();
}

String DeviceIdentity::sanitizeLabel(const String& in) {
    String out;
    out.reserve(in.length());

    // A separator is recorded as "pending" and only emitted when a valid
    // character follows it. That collapses runs and drops leading and trailing
    // separators in one pass, with no post-trim needed.
    bool pendingSep = false;

    for (unsigned int i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

        const bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (valid) {
            if (pendingSep && out.length()) out += '-';
            pendingSep = false;
            out += c;
            if (out.length() >= 63) break;   // DNS label limit
        } else {
            // Spaces, underscores, dots and existing dashes all fold into one
            // separator.
            pendingSep = true;
        }
    }
    return out;
}
