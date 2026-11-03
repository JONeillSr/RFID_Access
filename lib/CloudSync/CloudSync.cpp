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

// A fragmented heap: plenty free, but no single run large enough for a TLS
// session's buffers. Free heap cannot see it -- the 2.12.0 failure on Front
// Door was 118 KB free with a 35 KB largest block, and every sync failed while
// opening the connection. Below this block size a failing door is presumed to
// be failing for that reason (see the self-heal in the task loop). Syncs are
// still ATTEMPTED below it: the true threshold is not pinned down, and a door
// that can still connect must not be stopped by a guess.
static const size_t SYNC_MIN_BLOCK = 45000;

// Failing syncs with a fragmented heap for this long, and the door restarts
// itself -- only while the strike is not releasing. A restart is the one thing
// that reliably defragments, and a door that silently stops syncing is worse
// than one that is locked for a few seconds.
static const uint32_t HEAL_AFTER_MS = 15UL * 60 * 1000;

// The status text (~1.5 KB, built from many small strings) goes up at most this
// often, and only with a healthy largest block: building it every 30 s was the
// likeliest source of the fragmentation above.
static const uint32_t STATUS_EVERY_MS = 5UL * 60 * 1000;

// The release feed holds a TLS session open between syncs, so it asks for more
// room than a sync does: the session plus MIN_FREE_HEAP to spare, and a largest
// block a TLS record buffer fits in. Below this the feed simply does not run and
// releases arrive by sync -- slower, never lost. The last field failure was a
// fragmented heap during a sync's TLS session; a convenience must not cause the
// next one. The feed is closed before every sync, so it never competes with one.
static const size_t FEED_MIN_FREE  = 100000;
static const size_t FEED_MIN_BLOCK = 40000;

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
    _restartId = _settings ? _settings->getString(KEY_RESTART_ID, "") : String();
    _releaseId = _settings ? _settings->getString(KEY_RELEASE_ID, "") : String();
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

    // Pairing runs on the web task and needs a TLS session of its own; the
    // release feed may be holding one open on the sync task. Ask it to close and
    // wait briefly -- it checks every 250 ms between polls.
    struct FeedPause {
        CloudSync* s;
        explicit FeedPause(CloudSync* c) : s(c) {
            s->_feedPause = true;
            for (int i = 0; i < 50 && s->_feedClient; i++) delay(100);
        }
        ~FeedPause() { s->_feedPause = false; }
    } pause(this);

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

    // Set if the roster could not be applied. The sync still reports failure so
    // backoff and the failure counter behave, but the firmware offer below is
    // evaluated first -- see the comment at the roster write.
    bool rosterFailed = false;

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
    // What this door is actually running, so the admin app can tell a config it
    // has merely stored from one the door has applied.
    if (_readerMode.length()) req["readerMode"] = _readerMode;
    if (_hasContactKnown) req["hasDoorContact"] = _hasDoorContact;
    // Every sync, not just the first after a restart: if that one fails, the
    // backend would otherwise keep offering a restart that already happened.
    lock();
    if (_restartId.length()) req["restartId"] = _restartId;
    unlock();
    // Only with a handler: claiming the capability is what makes the admin app
    // offer the Release button for this door.
    if (_onRelease) req["remoteRelease"] = true;
    if (_releaseId.length()) req["releaseId"] = _releaseId;
    // Diagnostics for the admin app, at most every STATUS_EVERY_MS and only
    // while the heap has room to spare -- see STATUS_EVERY_MS.
    if (_statusSource && ESP.getMaxAllocHeap() >= SYNC_MIN_BLOCK &&
        (_lastStatusMs == 0 || millis() - _lastStatusMs >= STATUS_EVERY_MS)) {
        req["status"] = _statusSource();
        _lastStatusMs = millis();
    }
    req["ip"] = WiFi.localIP().toString();

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

    // Sized once, so the body is one allocation rather than a run of growing
    // ones -- each regrowth leaves a hole behind it.
    String body;
    body.reserve(measureJson(req) + 1);
    serializeJson(req, body);

    // Measured before the TLS session takes its share, so a failure below says
    // what the connection actually had to work with.
    size_t block = ESP.getMaxAllocHeap();

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
        err = "connection failed (" + String(rc) + "), largest block " +
              String(block / 1024) + " KB";
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

    // Server time, for judging a release's expiry. Taken here, before anything
    // can fail, because it is valid whatever else this response holds.
    {
        uint32_t se = resp["serverEpoch"] | 0;
        if (se) { _srvEpoch = se; _srvAtMs = millis(); }
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

        if (!ok) {
            // Carry the heap state. The bare message cost most of an
            // investigation once: it does not say whether the write failed for
            // memory or for filesystem reasons, and free heap alone looked
            // healthy while the largest contiguous block was the real limit.
            err = "roster write failed (" + String(ESP.getFreeHeap() / 1024) +
                  " KB free, largest block " + String(ESP.getMaxAllocHeap() / 1024) +
                  " KB, " + String(i) + " entries)";

            // DO NOT return here. Events stay unacked -- correct, nothing was
            // applied -- but the firmware offer below must still be considered.
            //
            // Returning early created a deadlock: a door that cannot apply a
            // roster never reaches the offer that would fix it, so it cannot
            // self-heal and needs a reboot or a cable. The door least able to
            // update is exactly the one that most needs the update, and that is
            // precisely the case the early return locked out.
            rosterFailed = true;
        }
    }

    // Only discard events once the roster applied cleanly. If it did not, the
    // backend still holds them and will be sent them again -- losing them here
    // to a failure the door already knows about would be the worst of both.
    if (!rosterFailed) {
        uint32_t ackBoot = resp["ackBootId"] | 0;
        uint32_t ackIdx  = resp["ackIdx"] | 0;
        if (n > 0 && (ackBoot != 0 || ackIdx != 0)) {
            eventLog.ack(ackBoot, ackIdx);
        }

        lock();
        _status.eventsSent += (uint32_t)n;
        _status.rosterRev = resp["rosterRev"] | _status.rosterRev;
        unlock();
    }

    // ---- door configuration ------------------------------------------------
    // Delivered on every sync, not only when it changes: a door that was offline
    // or failed to apply a setting gets another chance on the next cycle, and
    // the handler is the one place that knows what is already in effect.
    //
    // Deliberately after the ack above -- a config the door cannot use must not
    // cost it the event batch -- and before the firmware offer, which may reboot.
    if (_onConfig && resp["config"].is<JsonObject>()) {
        JsonObject c = resp["config"];
        DoorConfig cfg;
        if (c["relayHoldMs"].is<uint32_t>()) {
            cfg.hasRelayHoldMs = true;
            cfg.relayHoldMs    = c["relayHoldMs"].as<uint32_t>();
        }
        if (c["resultHoldMs"].is<uint32_t>()) {
            cfg.hasResultHoldMs = true;
            cfg.resultHoldMs    = c["resultHoldMs"].as<uint32_t>();
        }
        if (c["schedule"].is<JsonObject>()) {
            JsonObject sc      = c["schedule"];
            cfg.hasSchedule    = true;
            cfg.schedEnabled   = sc["enabled"]  | false;
            cfg.schedStartMin  = sc["startMin"] | 0;
            cfg.schedEndMin    = sc["endMin"]   | 0;
            cfg.schedDaysMask  = sc["daysMask"] | 0;
        }
        if (c["doorContact"].is<bool>()) {
            cfg.hasDoorContact = true;
            cfg.doorContact    = c["doorContact"].as<bool>();
        }
        if (c["doorHeldSec"].is<uint32_t>()) {
            cfg.hasDoorHeldSec = true;
            cfg.doorHeldSec    = c["doorHeldSec"].as<uint32_t>();
        }
        if (c["openSetupPortal"].is<bool>()) {
            cfg.hasSetupAP = true;
            cfg.setupAP    = c["openSetupPortal"].as<bool>();
        }
        if (c["readerMode"].is<const char*>()) {
            String m = c["readerMode"].as<String>();
            m.toLowerCase();
            // Only the two values this firmware understands. Anything else is a
            // newer backend talking about a reader this image cannot drive, and
            // guessing at it would stop every fob at the door.
            if (m == "wiegand" || m == "cnd") {
                cfg.hasReaderMode = true;
                cfg.readerWiegand = (m == "wiegand");
            } else if (m.length()) {
                say("[cloud] ignoring unknown reader format '" + m + "'");
            }
        }
        _onConfig(cfg);
    }

    // ---- remote restart ----------------------------------------------------
    // Only noted here; the task loop acts on it once this frame -- and its TLS
    // client -- is gone, and only when the app says the door is safe to restart.
    if (resp["restart"].is<JsonObject>()) {
        String id = resp["restart"]["id"] | "";
        lock();
        bool fresh = id.length() && id != _restartId;
        if (fresh && id != _pendingRestartId) {
            _pendingRestartId = id;
            unlock();
            say("[cloud] restart requested from the admin app (" + id + ")");
        } else {
            unlock();
        }
    }

    // ---- remote release ----------------------------------------------------
    // The slow path: carried here for a door that could not poll its feed. The
    // feed URL is the fast path, re-issued every sync so it never nears expiry.
    if (_onRelease) {
        if (resp["release"].is<JsonObject>()) {
            considerRelease(resp["release"]["id"] | "", resp["release"]["exp"] | 0, "sync");
        }
        String feed = resp["releaseFeed"]["url"] | "";
        if (feed.length() && feed != _feedUrl) {
            _feedUrl  = feed;
            _feedEtag = "";   // a new URL is a new conditional baseline
        }
        _feedFailures = 0;    // a good sync is a fresh start for the feed too
    }

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
            return !rosterFailed;
        }

        // GATE 2 - already running it. Guards against a version that was
        // published, applied, then re-offered from a stale record.
        if (fwVersion == FW_VERSION) {
            lock(); _status.fwNote = ""; unlock();
            return !rosterFailed;
        }

        // GATE 3 - is the door safe to restart? Never interrupt a release or a
        // scheduled-unlock window; just wait for the next cycle.
        if (!_safeToUpdate) {
            String m = "not applied - no safety check wired, OTA disabled";
            say("[ota] " + m);
            lock(); _status.fwNote = m; unlock();
            return !rosterFailed;
        }
        if (!_safeToUpdate()) {
            String m = "deferred " + fwVersion + " - door not idle";
            say("[ota] " + m);
            lock(); _status.fwNote = m; unlock();
            return !rosterFailed;
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

    return !rosterFailed;
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

String CloudSync::consumeRestart() {
    lock();
    String id = _pendingRestartId;
    _pendingRestartId = "";
    if (id.length()) _restartId = id;
    unlock();
    // NVS outside the lock: a flash write can stall long enough to hold up a
    // web request reading status().
    if (id.length() && _settings) _settings->setString(KEY_RESTART_ID, id);
    return id;
}

// -----------------------------------------------------------------------------
//  Remote release
// -----------------------------------------------------------------------------

void CloudSync::setReleaseNote(const String& n) {
    lock();
    bool changed = _status.releaseNote != n;
    _status.releaseNote = n;
    unlock();
    if (changed) say("[release] " + n);
}

void CloudSync::considerRelease(const String& id, uint32_t exp, const char* via) {
    // Once per id: both paths carry the same request, and the feed keeps serving
    // it after it has been acted on.
    if (!id.length() || id == _releaseId || id == _lapsedReleaseId) return;
    uint32_t now = serverNow();
    if (!now || now > exp) {
        // Late is not "better than never" here: the person it was for has gone.
        // Not reported back, so the admin app shows it as not confirmed.
        say("[release] " + id + " ignored - expired before it arrived (via " + via + ")");
        _lapsedReleaseId = id;
        return;
    }
    // Recorded before acting, in NVS, so a reboot straight after cannot repeat it.
    _releaseId = id;
    if (_settings) _settings->setString(KEY_RELEASE_ID, id);
    say("[release] " + id + " - releasing (via " + via + ")");
    _onRelease(id);
    // Confirm it to the backend in seconds rather than at the next interval, so
    // whoever pressed the button sees that it worked.
    requestSyncNow();
}

void CloudSync::closeFeed() {
    WiFiClientSecure* c = _feedClient;
    if (!c) return;
    delete _feedHttp;       // stop()s the connection
    _feedHttp = nullptr;
    c->stop();
    _feedClient = nullptr;
    delete c;
}

void CloudSync::pollReleaseFeed() {
    if (!_feedClient) {
        size_t freeHeap = ESP.getFreeHeap(), block = ESP.getMaxAllocHeap();
        if (freeHeap < FEED_MIN_FREE || block < FEED_MIN_BLOCK) {
            setReleaseNote("next sync only (low heap: " + String(freeHeap / 1024) +
                           " KB free, largest block " + String(block / 1024) + " KB)");
            return;
        }
        WiFiClientSecure* c = new WiFiClientSecure();
        c->setCACert(CERT_DIGICERT_GLOBAL_ROOT_G2);
        c->setHandshakeTimeout(10);
        _feedHttp = new HTTPClient();
        // Keeps the connection open after end(), so after the first poll each
        // one costs a request on an established session -- not a handshake,
        // which on this chip is the expensive part.
        _feedHttp->setReuse(true);
        _feedClient = c;
    }
    HTTPClient& http = *_feedHttp;
    if (!http.begin(*_feedClient, _feedUrl)) {
        closeFeed();
        _feedFailures++;
        return;
    }
    http.setTimeout(5000);
    if (_feedEtag.length()) http.addHeader("If-None-Match", _feedEtag);
    const char* want[] = { "ETag" };
    http.collectHeaders(want, 1);

    int rc = http.GET();
    if (rc == 304 || rc == 404) {
        // Unchanged, or this door has never been released: both are healthy.
        http.end();
    } else if (rc == 200) {
        String etag = http.header("ETag");
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
            considerRelease(doc["id"] | "", doc["exp"] | 0, "feed");
        }
        _feedEtag = etag;
    } else if (rc == 403) {
        // The URL expired or was revoked. The next sync issues a fresh one.
        http.end();
        closeFeed();
        _feedUrl = "";
        setReleaseNote("next sync only (feed URL refused; renewing at the next sync)");
        return;
    } else {
        http.end();
        closeFeed();       // reconnect from scratch next time
        _feedFailures++;
        setReleaseNote("feed error " + String(rc) + " (" + String(_feedFailures) + " in a row)");
        return;
    }
    _feedFailures = 0;
    setReleaseNote("polling the feed every " + String(RELEASE_POLL_MS / 1000) + " s");
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
            self->closeFeed();
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
        uint32_t lastOkMs = self->_status.lastSuccessMs;
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
        //
        // Deliberately NOT gated on `ok`. A queued version got there by passing
        // every gate inside syncOnce, so the offer is sound whatever else in that
        // sync went wrong. Requiring success completed a deadlock: a door failing
        // to apply rosters reported failure, the update that fixed it was
        // therefore never applied, and it could not recover without a reboot or a
        // cable. A door that cannot sync properly is the one that most needs the
        // new image, not the one to withhold it from.
        String restartOwed;
        if (fwVersion.length()) {
            // A successful update reboots, which is the restart someone asked
            // for. Mark it honoured first, or the door would come back up, see
            // the same request and restart a second time.
            restartOwed = self->consumeRestart();
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

        // A remote restart. Acted on only when the app says the door is safe;
        // otherwise it stays pending and the next sync, ~30 s on, tries again.
        // restartOwed is a request an update was going to satisfy, then did not:
        // it is already marked honoured, so it restarts now or never -- and an
        // update only starts when the door is idle, so now is safe.
        self->lock();
        bool pending = self->_pendingRestartId.length() > 0;
        self->unlock();
        if (pending && !self->_safeToRestart) {
            self->say("[cloud] restart ignored - no safety check wired");
            self->lock(); self->_pendingRestartId = ""; self->unlock();
        } else if (pending && !self->_safeToRestart()) {
            self->say("[cloud] restart deferred - door releasing");
        } else if (pending || restartOwed.length()) {
            String id = restartOwed.length() ? restartOwed : self->consumeRestart();
            // Logged before the restart: the spool is in flash, so the event
            // goes up on the first sync after the boot, and the reboot in the
            // door's history is attributable to a request.
            eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false, "restart=remote");
            self->say("[cloud] restarting now (" + id + ")");
            delay(250);
            ESP.restart();
        }

        // Self-heal from a fragmented heap. Only when syncs are failing AND the
        // largest block is too small for a TLS session, for HEAL_AFTER_MS --
        // counted from the last success, or from boot if there has been none,
        // so a door cannot restart on a loop shorter than that. Never while the
        // strike is releasing; the next failed sync tries again.
        if (!ok && ESP.getMaxAllocHeap() < SYNC_MIN_BLOCK &&
            millis() - lastOkMs >= HEAL_AFTER_MS &&
            self->_safeToRestart && self->_safeToRestart()) {
            String m = "restarting: no sync for " + String((millis() - lastOkMs) / 60000) +
                       " min, largest heap block " + String(ESP.getMaxAllocHeap() / 1024) + " KB";
            self->say("[cloud] " + m);
            eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false, "restart=heap");
            delay(250);
            ESP.restart();
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

        // Wait for the next sync, checking the release feed meanwhile. Wake early
        // if something asked for an immediate sync (just paired, or a release
        // that wants confirming). Only while healthy: a door that is backing off
        // is failing to reach the backend, and polling harder will not help.
        uint32_t start = millis(), lastPoll = 0;
        bool polled = false;
        while (millis() - start < wait) {
            if (self->_syncNow) { self->_syncNow = false; break; }
            if (self->_feedPause) {
                self->closeFeed();
            } else if (!backingOff && self->_onRelease && self->_feedUrl.length() &&
                       self->_feedFailures < 3 &&
                       (!polled || millis() - lastPoll >= RELEASE_POLL_MS)) {
                self->pollReleaseFeed();
                lastPoll = millis();
                polled = true;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (self->_onRelease && !self->_feedUrl.length()) {
            self->setReleaseNote("next sync only (no feed URL yet)");
        } else if (self->_feedFailures >= 3) {
            self->setReleaseNote("next sync only (feed failing; retrying after the sync)");
        }
        // Never into a sync with the feed's TLS session still open.
        self->closeFeed();
    }
}
