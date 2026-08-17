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
#include <Update.h>              // OTA partition writer
#include <mbedtls/sha256.h>      // image verification
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
                      const char* defaultHost) {
    if (!_mutex) _mutex = xSemaphoreCreateMutex();
    _settings = settings;
    _identity = identity;

    lock();
    // A stored host wins over the compiled-in one. `defaultHost` is only a
    // starting value for a device that has never been told otherwise, which
    // keeps existing units working across this change with nothing to migrate.
    //
    // This is what lets ONE image serve every customer. Each deployment lives in
    // its own tenant with its own Function App hostname, so a compile-time host
    // would mean a firmware build per customer -- multiplying the per-board OTA
    // matrix by the customer count and making images non-interchangeable in a
    // way nothing detects: a spare flashed from the wrong folder boots fine and
    // quietly syncs to somebody else's backend.
    _host      = _settings ? _settings->getString(KEY_CLOUD_HOST, defaultHost) : String(defaultHost);
    _deviceKey = _settings ? _settings->getString(KEY_DEVICE_KEY, "") : String();
    _status.paired = _deviceKey.length() > 0;
    unlock();

    if (_status.paired) startTask();
}

String CloudSync::host() const {
    lock();
    String h = _host;
    unlock();
    return h;
}

bool CloudSync::setHost(const String& host) {
    String h = host;
    h.trim();
    if (h.length() == 0) return false;

    lock();
    bool changed = (h != _host);
    _host = h;
    if (_settings) _settings->setString(KEY_CLOUD_HOST, h);

    // A device key is issued BY a backend and means nothing to a different one.
    // Keeping it across a host change would leave the door looking paired while
    // every sync is rejected -- so pointing at a new backend forces re-pairing,
    // which is the honest state and the one /setup already knows how to resolve.
    if (changed && _deviceKey.length()) {
        _deviceKey = "";
        if (_settings) _settings->setString(KEY_DEVICE_KEY, "");
        _status.paired     = false;
        _status.everSynced = false;
        _status.lastError  = "backend changed - re-pair required";
    }
    unlock();
    return changed;
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
    uint32_t waitMs = _currentWaitMs;
    uint32_t dueAt  = _nextAttemptAtMs;
    unlock();

    s.backoffSecs = waitMs / 1000;
    // Unsigned subtraction, so compare before subtracting: once the deadline has
    // passed, an attempt is due now rather than in 49 days.
    uint32_t now = millis();
    s.nextAttemptSecs = (dueAt > now) ? (dueAt - now) / 1000 : 0;

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

    // ---- firmware offer ----------------------------------------------------
    // Everything above has already succeeded, so the sync counts as good even if
    // the update is declined or fails. An OTA problem must not look like a sync
    // problem, or a door that cannot update appears to have lost the backend.
    if (resp["firmware"].is<JsonObject>()) {
        JsonObject fw = resp["firmware"];
        String fwBoard   = fw["board"]   | "";
        String fwVersion = fw["version"] | "";
        String fwUrl     = fw["url"]     | "";
        String fwSha     = fw["sha256"]  | "";

        say("[ota] offered " + fwVersion + " for '" + fwBoard + "' (running " FW_VERSION
            " on '" BOARD_NAME "')");

        // GATE 1 - board must match exactly.
        //
        // The backend already filters by board, so a mismatch here means a
        // server-side bug or a tampered response. Check anyway: writing another
        // variant's image does not degrade the device, it stops it booting, and
        // some of these doors are above ceilings. A hard local check is the only
        // thing that cannot be undone by a mistake elsewhere.
        if (fwBoard != BOARD_NAME) {
            String m = "REFUSED - image is for '" + fwBoard + "', this is '" BOARD_NAME "'";
            say("[ota] " + m);
            eventLog.append(EventLog::EVT_FW_FAILED, EventLog::R_NONE, false,
                            fwVersion.c_str());
            lock(); _status.fwNote = m; unlock();
            return true;
        }

        // GATE 2 - already running it. Guards against a version that was
        // published, applied, then re-offered from a stale record.
        if (fwVersion == FW_VERSION) {
            lock(); _status.fwNote = ""; unlock();
            return true;
        }

        // GATE 3 - is the door safe to restart? Never interrupt a release or a
        // scheduled-unlock window; just wait for the next cycle.
        if (!_safeToUpdate) {
            String m = "not applied - no safety check wired, OTA disabled";
            say("[ota] " + m);
            lock(); _status.fwNote = m; unlock();
            return true;
        }
        if (!_safeToUpdate()) {
            String m = "deferred " + fwVersion + " - door not idle";
            say("[ota] " + m);
            lock(); _status.fwNote = m; unlock();
            return true;
        }

        // Do NOT download from here. This function still owns a live
        // WiFiClientSecure: HTTPClient::end() closes the socket but the mbedTLS
        // context (~45 KB) is not released until that object is destroyed, which
        // happens when this function returns. Starting the OTA's handshake now
        // means two TLS contexts at once, which does not fit -- the symptom is a
        // bare "HTTP -1" from a host the device was talking to a second earlier.
        //
        // Record the approved offer; the task loop applies it after this frame
        // is gone.
        lock();
        _pendingFwVersion = fwVersion;
        _pendingFwUrl     = fwUrl;
        _pendingFwSha     = fwSha;
        _status.fwNote    = "queued " + fwVersion;
        unlock();
    } else {
        lock(); _status.fwNote = ""; unlock();
    }

    return true;
}

// -----------------------------------------------------------------------------
//  OTA
// -----------------------------------------------------------------------------

bool CloudSync::applyFirmware(const String& url, const String& version,
                              const String& sha256Hex, String& err) {
    lock();
    String key = _deviceKey;
    unlock();

    // An OTA needs its own TLS session on top of whatever else is live. Check
    // explicitly and say so, rather than letting mbedTLS fail the handshake and
    // surface as an uninformative "HTTP -1".
    size_t freeHeap = ESP.getFreeHeap();
    say("[ota] free heap before download: " + String(freeHeap) + " B");
    if (freeHeap < MIN_FREE_HEAP) {
        err = "only " + String(freeHeap) + " B heap free, need " + String(MIN_FREE_HEAP);
        return false;
    }

    WiFiClientSecure client;
    client.setCACert(CERT_DIGICERT_GLOBAL_ROOT_G2);
    client.setHandshakeTimeout(20);

    HTTPClient http;
    if (!http.begin(client, url)) { err = "could not open connection"; return false; }
    http.addHeader("x-device-key", key);
    http.addHeader("x-device-id", _identity->deviceId());
    http.setTimeout(30000);
    // Collect the headers the server echoes so the image can be cross-checked
    // against what was offered, rather than trusting the URL alone.
    const char* want[] = { "x-fw-board", "x-fw-version", "x-fw-sha256", "Content-Length" };
    http.collectHeaders(want, 4);

    int rc = http.GET();
    if (rc != 200) {
        // Negative codes are client-side (TLS/TCP), not the server. Report the
        // heap alongside, because exhaustion is the usual cause and a bare code
        // sends you looking at the network instead.
        err = "HTTP " + String(rc);
        if (rc < 0) err += " (heap " + String(ESP.getFreeHeap()) + " B)";
        http.end();
        return false;
    }

    // GATE 4 — the served image must agree with the offer. A mismatch here means
    // the offer and the blob have diverged; refuse rather than flash something
    // that was never approved for this board.
    String servedBoard = http.header("x-fw-board");
    if (servedBoard.length() && servedBoard != BOARD_NAME) {
        err = "served image is for '" + servedBoard + "'";
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len <= 0) { err = "server did not report a size"; http.end(); return false; }

    if (!Update.begin((size_t)len)) {
        err = "no room in the OTA slot (" + String(len) + " B)";
        http.end();
        return false;
    }

    // Hash while writing. The ESP32 Update library can verify an MD5 natively but
    // not a SHA-256, so the digest is computed here in parallel with the flash
    // write and checked BEFORE Update.end() commits anything.
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int remaining = len;
    uint32_t lastYield = millis();

    while (remaining > 0 && http.connected()) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - lastYield > 15000) { err = "download stalled"; break; }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int n = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
        if (n <= 0) continue;
        if (Update.write(buf, n) != (size_t)n) { err = "flash write failed"; break; }
        mbedtls_sha256_update(&sha, buf, n);
        remaining -= n;
        lastYield = millis();
    }
    http.end();

    if (remaining > 0) {
        if (!err.length()) err = "download incomplete (" + String(remaining) + " B short)";
        Update.abort();
        mbedtls_sha256_free(&sha);
        return false;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[64] = '\0';

    // GATE 5 — content check, and the last chance to refuse. Nothing has been
    // committed yet; Update.abort() leaves the running image untouched.
    if (sha256Hex.length() && !sha256Hex.equalsIgnoreCase(hex)) {
        err = "sha256 mismatch - refusing to commit";
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        err = "commit failed (" + String(Update.getError()) + ")";
        return false;
    }

    // Record the update BEFORE rebooting: the spool is durable, so this survives
    // the restart and reaches the backend on the next sync. Otherwise the one
    // event you most want in the audit trail is the one that never gets sent.
    //
    // The version change goes in the detail field as "<old>><new>", so the
    // backend can answer "which doors took 2.5.0, and when" without inferring it
    // from a gap in the boot events. FW_VERSION is still the OLD version here --
    // this runs before the reboot.
    {
        String change = String(FW_VERSION) + ">" + version;
        eventLog.append(EventLog::EVT_FW_UPDATED, EventLog::R_NONE, true, change.c_str());
    }
    say("[ota] committed " + version + ", rebooting");
    delay(250);
    ESP.restart();
    return true;    // not reached
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
            // fwNote is deliberately NOT cleared here. A refused or deferred
            // update does not make the sync a failure, and wiping it on success
            // would erase the only explanation of why a door is not updating --
            // which is exactly the bug this comment exists to prevent recurring.
        } else {
            if (self->_status.failures < 0xFFFF) self->_status.failures++;
            self->_status.lastError = err;
        }
        uint16_t failures = self->_status.failures;
        // Take the queued offer, if any. Copied and cleared under the lock so a
        // second sync cannot start the same download twice.
        String fwVersion = self->_pendingFwVersion;
        String fwUrl     = self->_pendingFwUrl;
        String fwSha     = self->_pendingFwSha;
        self->_pendingFwVersion = "";
        self->_pendingFwUrl     = "";
        self->_pendingFwSha     = "";
        self->unlock();

        // Applied HERE, not inside syncOnce(): by this point syncOnce has
        // returned and its WiFiClientSecure has been destroyed, so the OTA gets
        // the heap to itself.
        if (ok && fwVersion.length()) {
            self->say("[ota] downloading " + fwVersion + " from " + fwUrl);
            self->lock(); self->_status.fwNote = "downloading " + fwVersion; self->unlock();

            String ferr;
            if (!self->applyFirmware(fwUrl, fwVersion, fwSha, ferr)) {
                // The running image is untouched and the door still works.
                // Record it so a door that cannot take updates stays visible
                // rather than silently stuck on an old build.
                String m = "FAILED " + fwVersion + ": " + ferr;
                self->say("[ota] " + m);
                eventLog.append(EventLog::EVT_FW_FAILED, EventLog::R_NONE, false,
                                fwVersion.c_str());
                self->lock(); self->_status.fwNote = m; self->unlock();
            }
            // On success applyFirmware() never returns -- it reboots.
        }

        // Exponential backoff, capped. A door that has been offline for hours
        // should not hammer the backend the moment it returns, but it must also
        // not wait so long that recovery looks like a hang.
        uint32_t wait = POLL_INTERVAL_MS + jitter;
        bool backingOff = false;
        if (failures > 0) {
            uint32_t backoff = BACKOFF_MIN_MS << (failures > 5 ? 5 : failures - 1);
            if (backoff > BACKOFF_MAX_MS) backoff = BACKOFF_MAX_MS;
            wait = backoff + jitter;
            backingOff = true;
        }

        // Publish the schedule so /status can answer "when will this door try
        // again?" without anyone having to infer it from the failure count.
        self->lock();
        self->_currentWaitMs   = backingOff ? wait : 0;
        self->_nextAttemptAtMs = millis() + wait;
        self->unlock();

        // Wake early if something asked for an immediate sync (e.g. just paired).
        uint32_t waited = 0;
        while (waited < wait) {
            if (self->_syncNow) { self->_syncNow = false; break; }
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
    }
}
