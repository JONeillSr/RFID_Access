/**
 * @file    CloudSync.h
 * @brief   Talks to the Azure backend: events up, roster and config down.
 *
 * THE GOVERNING RULE: this is never in the access decision path.
 *
 * A door decides locally, against its cached Roster, with no reference to this
 * module. Everything here runs on its own FreeRTOS task. If the WAN is down, the
 * backend is deploying, a certificate has expired, or Azure is having a bad day,
 * the door keeps working exactly as it did a minute earlier. That is not a
 * fallback mode — it is the normal mode, and the network is the optional part.
 *
 * Consequences of that rule, all deliberate:
 *
 *   - A failed sync NEVER clears the roster. Never fail-open (admit everyone),
 *     never fail-closed-to-everyone (deny everyone). The last known-good roster
 *     stays authoritative indefinitely.
 *   - Roster replacement is atomic. Roster::replaceAll() builds the new list
 *     completely before swapping it in, so a tap arriving mid-sync sees either
 *     the old list or the new one — never a partial one.
 *   - Events are only discarded once the backend confirms them. A batch that is
 *     sent but not acknowledged is retried; the (deviceId, bootId, idx) key
 *     makes the retry converge rather than duplicate.
 *   - Staleness is surfaced loudly. A door that quietly stopped syncing looks
 *     identical to a healthy one from the outside, and a revoked fob that still
 *     works is the failure mode that actually matters. After
 *     STALE_AFTER_MS without a success, /status says so.
 *
 * PAIRING
 * Devices ship with no credentials. An operator generates a short-lived code in
 * the admin UI and types it into /setup; the device exchanges it once at
 * /api/v1/enroll for a long-lived key kept in NVS. No secret is baked into the
 * firmware image — which matters, because the image is published and identical
 * across every door.
 */

#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "DeviceSettings.h"
#include "DeviceIdentity.h"

class CloudSync {
public:
    /// How long without a successful sync before the device calls itself stale.
    static const uint32_t STALE_AFTER_MS = 6UL * 60UL * 60UL * 1000UL;   // 6 h

    /// Nominal poll interval. Jittered per device so a fleet coming back from a
    /// shared outage does not arrive in lockstep.
    static const uint32_t POLL_INTERVAL_MS = 30000;

    /// Backoff bounds after consecutive failures.
    static const uint32_t BACKOFF_MIN_MS = 30000;       // 30 s
    static const uint32_t BACKOFF_MAX_MS = 15UL * 60UL * 1000UL;   // 15 min

    struct Status {
        bool     paired          = false;
        bool     everSynced      = false;
        bool     stale           = false;
        uint32_t lastSuccessMs   = 0;    // millis() of last good sync, 0 = never
        uint32_t secsSinceSuccess = 0;
        uint32_t rosterRev       = 0;
        uint16_t failures        = 0;    // consecutive
        uint32_t eventsSent      = 0;    // this boot
        String   lastError;              // empty when healthy
    };

    /// Call once in setup(), AFTER WiFi, Roster and EventLog are up. Starts the
    /// sync task only if the device is already paired; an unpaired device sits
    /// idle until pair() succeeds, costing nothing.
    void begin(DeviceSettings* settings, DeviceIdentity* identity, const char* host);

    bool paired() const;

    /// Exchange an operator-supplied enrollment code for a device key.
    /// Blocking (it is called from a web handler) and slow — a TLS handshake
    /// plus a round trip. Returns false with a human-readable reason in `err`.
    bool pair(const String& code, String& err);

    /// Forget the device key. The door keeps its cached roster and keeps
    /// working; it simply stops talking to the backend.
    void unpair();

    Status status() const;

    /// Ask the task to sync now rather than waiting out the interval. Used
    /// after pairing so the first roster arrives immediately.
    void requestSyncNow();

private:
    DeviceSettings* _settings = nullptr;
    DeviceIdentity* _identity = nullptr;
    String          _host;
    String          _deviceKey;      // empty = unpaired

    // NVS keys. Preferences limits key names to 15 characters.
    static constexpr const char* KEY_DEVICE_KEY = "cloudKey";

    mutable SemaphoreHandle_t _mutex = nullptr;
    void lock()   const { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
    void unlock() const { if (_mutex) xSemaphoreGive(_mutex); }

    // Guarded by _mutex.
    Status   _status;
    volatile bool _syncNow = false;

    /// One full cycle: build the batch, POST, apply the response. Returns true
    /// on success. Never throws, never blocks the decision path.
    bool syncOnce(String& err);

    /// Epoch seconds at which this boot began, or 0 if the clock is not yet
    /// trusted. Lets the backend resolve events logged before NTP landed.
    uint32_t bootEpoch() const;

    static void task(void* pv);
    void        startTask();
    bool        _taskStarted = false;
};

extern CloudSync cloudSync;
