/**
 * @file    CloudSync.cpp
 * @brief   Implementation of the backend sync client.
 */

#include "CloudSync.h"
#include "RootCerts.h"
#include "AccessControl.h"
#include "EventLog.h"
// BOARD_NAME, keyed off the per-environment BOARD_* build flag. The backend
// needs it because firmware is per board type: an image built for one ESP32
// variant bricks another, so a rollout must never offer a mismatched build.
#include "BoardConfig.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

CloudSync cloudSync;

// A sync batch is bounded so one very stale door cannot try to POST thousands of
// events in a single request and run the ESP32 out of heap mid-serialise. The
// remainder simply goes in the next cycle.
static const size_t MAX_EVENTS_PER_SYNC = 40;

// TLS handshake wants ~40-50 KB of heap. Refuse to start one below this, rather
// than failing deep inside mbedTLS with an opaque error.
static const size_t MIN_FREE_HEAP = 60000;

void CloudSync::begin(DeviceSettings* settings, DeviceIdentity* identity,
                      const char* host) {
    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    _settings = settings;
    _identity = identity;
    _host     = host;

    lock();
    _deviceKey = _settings ? _settings->getString(KEY_DEVICE_KEY, "") : String();
    _status.paired = _deviceKey.length() > 0;
    unlock();

    if (_status.paired) startTask();
}

bool CloudSync::paired() const {
    lock();
    bool p = _deviceKey.length() > 0;
    unlock();
    return p;
}

CloudSync::Status CloudSync::status() const {
    lock();
    Status s = _status;
    unlock();
    s.paired = s.paired;
    if (s.lastSuccessMs != 0) {
        s.secsSinceSuccess = (millis() - s.lastSuccessMs) / 1000;
        s.stale = (millis() - s.lastSuccessMs) > STALE_AFTER_MS;
    } else {
        s.secsSinceSuccess = 0;
        // Never having synced is only "stale" once the device has been up long
        // enough that it should have managed one.
        s.stale = s.paired && millis() > STALE_AFTER_MS;
    }
    return s;
}

void CloudSync::requestSyncNow() { _syncNow = true; }

void CloudSync::unpair() {
    lock();
    _deviceKey = "";
    _status.paired = false;
    _status.lastError = "unpaired";
    unlock();
    if (_settings) _settings->setString(KEY_DEVICE_KEY, "");
    // The task is left running: it no-ops while unpaired and picks straight back
    // up if the device is paired again, which avoids task lifecycle churn.
}

uint32_t CloudSync::bootEpoch() const {
    time_t now = time(nullptr);
    if (now < 1700000000) return 0;          // clock not yet trustworthy
    return (uint32_t)(now - (millis() / 1000));
}

// -----------------------------------------------------------------------------
//  Pairing
// -----------------------------------------------------------------------------

bool CloudSync::pair(const String& code, String& err) {
    if (code.length() < 4) { err = "code too short"; return false; }
    if (WiFi.status() != WL_CONNECTED) { err = "not connected to WiFi"; return false; }
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
        err = "not enough free memory for TLS right now";
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(CERT_DIGICERT_GLOBAL_ROOT_G2);
    client.setHandshakeTimeout(20);

    HTTPClient http;
    String url = "https://" + _host + "/api/v1/enroll";
    if (!http.begin(client, url)) { err = "could not open connection"; return false; }
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(20000);

    JsonDocument req;
    req["deviceId"] = _identity->deviceId();
    req["board"]    = BOARD_NAME;
    req["firmware"] = FW_VERSION;
    req["code"]     = code;
    String body;
    serializeJson(req, body);

    int rc = http.POST(body);
    if (rc <= 0) {
        // Negative codes are client-side: DNS, TCP or TLS. The commonest cause
        // by far is the certificate, so say so rather than leaving a bare number.
        err = "connection failed (" + String(rc) + ") - check DNS/TLS";
        http.end();
        return false;
    }
    if (rc == 403) { err = "code rejected: invalid or expired"; http.end(); return false; }
    if (rc != 200) { err = "server returned HTTP " + String(rc); http.end(); return false; }

    // getString(), NOT getStream(). Azure Functions replies with
    // Transfer-Encoding: chunked and no Content-Length, and HTTPClient's
    // getStream() hands back the raw client with the chunk framing still in
    // place -- ArduinoJson then chokes on the leading hex length prefix. Only
    // getString() decodes chunking. The body here is a few hundred bytes.
    String payload = http.getString();
    http.end();

    JsonDocument resp;
    DeserializationError jerr = deserializeJson(resp, payload);
    if (jerr) {
        // Report shape, never content: this body contains the device key.
        err = String("bad response: ") + jerr.c_str() +
              " (" + payload.length() + " bytes, starts '" +
              (payload.length() ? String(payload[0]) : String("?")) + "')";
        return false;
    }

    String key = resp["deviceKey"].as<String>();
    if (key.length() < 16) { err = "server did not return a device key"; return false; }

    _settings->setString(KEY_DEVICE_KEY, key);

    // Adopt the operator-chosen names from the backend, so the door labels itself
    // consistently with what the admin UI already shows.
    String doorName = resp["doorName"].as<String>();
    String site     = resp["site"].as<String>();
    if (doorName.length()) _identity->setDoorName(doorName);
    if (site.length())     _identity->setSiteName(site);

    lock();
    _deviceKey        = key;
    _status.paired    = true;
    _status.failures  = 0;
    _status.lastError = "";
    unlock();

    startTask();
    requestSyncNow();     // pull the roster immediately rather than in 30 s
    return true;
}

// -----------------------------------------------------------------------------
//  Sync
// -----------------------------------------------------------------------------

bool CloudSync::syncOnce(String& err) {
    if (WiFi.status() != WL_CONNECTED) { err = "wifi down"; return false; }
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) { err = "low heap, skipping"; return false; }

    lock();
    String key = _deviceKey;
    unlock();
    if (!key.length()) { err = "not paired"; return false; }

    // ---- build the batch ---------------------------------------------------
    static EventLog::Record batch[MAX_EVENTS_PER_SYNC];
    size_t n = eventLog.peek(batch, MAX_EVENTS_PER_SYNC);

    JsonDocument req;
    req["deviceId"]  = _identity->deviceId();
    req["board"]     = BOARD_NAME;
    req["firmware"]  = FW_VERSION;
    req["bootId"]    = eventLog.bootId();
    req["bootEpoch"] = bootEpoch();
    req["rosterRev"] = acRosterRev();

    JsonArray evs = req["events"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) {
        const EventLog::Record& r = batch[i];
        JsonObject o = evs.add<JsonObject>();
        o["bootId"]     = r.bootId;
        o["idx"]        = r.idx;
        o["uptimeMs"]   = r.uptimeMs;
        o["epoch"]      = r.epoch;
        o["type"]       = r.type;
        o["reason"]     = r.reason;
        o["granted"]    = (r.flags & EventLog::F_GRANTED) != 0;
        o["timeApprox"] = (r.flags & EventLog::F_TIME_APPROX) != 0;
        o["cred"]       = r.cred;
    }

    String body;
    serializeJson(req, body);

    // ---- send --------------------------------------------------------------
    WiFiClientSecure client;
    client.setCACert(CERT_DIGICERT_GLOBAL_ROOT_G2);
    client.setHandshakeTimeout(20);

    HTTPClient http;
    String url = "https://" + _host + "/api/v1/sync";
    if (!http.begin(client, url)) { err = "could not open connection"; return false; }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-device-key", key);
    http.setTimeout(20000);

    int rc = http.POST(body);
    if (rc <= 0) {
        err = "connection failed (" + String(rc) + ")";
        http.end();
        return false;
    }
    if (rc == 401) {
        // The backend no longer recognises this key. Do NOT clear the roster:
        // the door must keep working. Surface it and let an operator re-pair.
        err = "unauthorized - device key rejected, re-pair needed";
        http.end();
        return false;
    }
    if (rc != 200) { err = "HTTP " + String(rc); http.end(); return false; }

    // getString() rather than getStream(), for the same reason as in pair():
    // the backend replies chunked, and getStream() would leave the chunk
    // framing in the data.
    //
    // This does mean buffering the whole body. At 300 credentials the roster is
    // roughly 15 KB of JSON, plus the JsonDocument, on top of the ~45 KB the TLS
    // session already holds -- comfortable against the ~200 KB free here, and
    // MIN_FREE_HEAP is checked before the handshake even starts. If the fleet
    // ever outgrows that, the fix is server-side pagination of the roster, not
    // streaming: chunked framing rules streaming out.
    String payload = http.getString();
    http.end();

    JsonDocument resp;
    DeserializationError jerr = deserializeJson(resp, payload);
    if (jerr) {
        // Shape only, never content: the roster is a list of live credentials.
        err = String("bad response: ") + jerr.c_str() +
              " (" + payload.length() + " bytes)";
        return false;
    }

    // ---- apply -------------------------------------------------------------
    // Roster first. If it fails, the events are deliberately left un-acked so
    // the whole cycle is retried rather than half-applied.
    if (resp["roster"].is<JsonArray>()) {
        JsonArray arr = resp["roster"].as<JsonArray>();
        uint32_t newRev = resp["rosterRev"] | 0;

        size_t count = arr.size();
        RosterUpdate* updates = (RosterUpdate*)calloc(count ? count : 1, sizeof(RosterUpdate));
        String* creds = new String[count ? count : 1];
        String* names = new String[count ? count : 1];
        if (!updates) { err = "out of memory applying roster"; delete[] creds; delete[] names; return false; }

        size_t i = 0;
        for (JsonObject o : arr) {
            creds[i] = o["cred"].as<String>();
            names[i] = o["name"].as<String>();
            updates[i].cred = creds[i].c_str();
            updates[i].name = names[i].c_str();
            i++;
        }

        bool ok = acReplaceRoster(updates, i, newRev);
        free(updates);
        delete[] creds;
        delete[] names;

        if (!ok) { err = "roster write failed"; return false; }
    }

    // Only now discard events: the backend has them durably, and the roster
    // applied cleanly.
    uint32_t ackBoot = resp["ackBootId"] | 0;
    uint32_t ackIdx  = resp["ackIdx"] | 0;
    if (n > 0 && (ackBoot != 0 || ackIdx != 0)) {
        eventLog.ack(ackBoot, ackIdx);
    }

    lock();
    _status.eventsSent += (uint32_t)n;
    _status.rosterRev = resp["rosterRev"] | _status.rosterRev;
    unlock();

    return true;
}

// -----------------------------------------------------------------------------
//  Task
// -----------------------------------------------------------------------------

void CloudSync::startTask() {
    if (_taskStarted) return;
    _taskStarted = true;
    // 8 KB: TLS wants a deep stack, and this task also parses JSON.
    xTaskCreate(task, "cloudsync", 8192, this, 1, nullptr);
}

void CloudSync::task(void* pv) {
    CloudSync* self = static_cast<CloudSync*>(pv);

    // Per-device jitter, seeded from the MAC-derived device id, so a fleet
    // returning from a shared outage does not stampede the backend in lockstep.
    uint32_t jitter = 0;
    for (char c : self->_identity->deviceId()) jitter = jitter * 31 + (uint8_t)c;
    jitter %= 7000;

    // Let WiFi, NTP and the web server settle before the first attempt.
    vTaskDelay(pdMS_TO_TICKS(10000 + jitter));

    for (;;) {
        if (!self->paired()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        String err;
        bool ok = self->syncOnce(err);

        self->lock();
        if (ok) {
            self->_status.lastSuccessMs = millis();
            self->_status.everSynced    = true;
            self->_status.failures      = 0;
            self->_status.lastError     = "";
        } else {
            if (self->_status.failures < 0xFFFF) self->_status.failures++;
            self->_status.lastError = err;
        }
        uint16_t failures = self->_status.failures;
        self->unlock();

        // Exponential backoff, capped. A door that has been offline for hours
        // should not hammer the backend the moment it returns, but it must also
        // not wait so long that recovery looks like a hang.
        uint32_t wait = POLL_INTERVAL_MS + jitter;
        if (failures > 0) {
            uint32_t backoff = BACKOFF_MIN_MS << (failures > 5 ? 5 : failures - 1);
            if (backoff > BACKOFF_MAX_MS) backoff = BACKOFF_MAX_MS;
            wait = backoff + jitter;
        }

        // Wake early if something asked for an immediate sync (e.g. just paired).
        uint32_t waited = 0;
        while (waited < wait) {
            if (self->_syncNow) { self->_syncNow = false; break; }
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
    }
}
