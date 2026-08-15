#include "WebHandlers.h"
#include "AccessControl.h"
#include "UnlockSchedule.h"
#include "HtmlPages.h"
#include <ArduinoJson.h>
#include <time.h>

void registerWebHandlers(WebServer& server) {
    server.on("/", [&server]() {
        server.send_P(200, "text/html", DASHBOARD_HTML);
    });

    server.on("/config", [&server]() {
        server.send_P(200, "text/html", CONFIG_HTML);
    });

    server.on("/api/taps", [&server]() {
        JsonDocument doc;
        LOCK();
        doc["count"]  = logCount;
        doc["uptime"] = millis();
        JsonArray arr = doc["taps"].to<JsonArray>();
        for (int i = 0; i < logCount; i++) {
            int idx = (logHead - 1 - i + LOG_SIZE) % LOG_SIZE;
            JsonObject o = arr.add<JsonObject>();
            o["uid"]     = tapLog[idx].uid;
            o["type"]    = tapLog[idx].type;
            o["granted"] = tapLog[idx].granted;
            o["ms"]      = tapLog[idx].ms;
            String nm;
            o["name"] = acNameFor(tapLog[idx].uid, nm) ? nm : String();
        }
        UNLOCK();
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/list", [&server]() {
        JsonDocument doc;
        // Roster guards itself; the lock here covers lastUnknownUid only.
        JsonArray arr = doc["entries"].to<JsonArray>();
        size_t n = acCount();
        for (size_t i = 0; i < n; i++) {
            String uid, name;
            if (!acEntryAt(i, uid, name)) break;
            JsonObject o = arr.add<JsonObject>();
            o["uid"]  = uid;
            o["name"] = name;
        }
        LOCK();
        if (lastUnknownUid.length()) doc["unknown"] = lastUnknownUid;
        else                         doc["unknown"] = nullptr;
        UNLOCK();
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/add", HTTP_POST, [&server]() {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "bad json");
            return;
        }
        acAddEntry(doc["uid"].as<String>(), doc["name"].as<String>());
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/schedule", HTTP_GET, [&server]() {
        JsonDocument doc;
        doc["enabled"]    = unlockSchedule.enabled();
        doc["start"]      = unlockSchedule.startMin();
        doc["end"]        = unlockSchedule.endMin();
        doc["days"]       = unlockSchedule.daysMask();
        doc["active"]     = unlockSchedule.isActiveNow();
        doc["timeSynced"] = UnlockSchedule::timeValid();
        if (UnlockSchedule::timeValid()) {
            time_t    now = time(nullptr);
            struct tm tm;
            localtime_r(&now, &tm);
            char buf[24];
            strftime(buf, sizeof(buf), "%a %I:%M %p", &tm);
            doc["now"] = buf;
        } else {
            doc["now"] = "not synced";
        }
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.on("/api/schedule", HTTP_POST, [&server]() {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "bad json");
            return;
        }
        unlockSchedule.set(doc["enabled"] | false,
                           doc["start"] | 0,
                           doc["end"] | 0,
                           doc["days"] | 0);
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/rename", HTTP_POST, [&server]() {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "bad json");
            return;
        }
        acRenameEntry(doc["uid"].as<String>(), doc["name"].as<String>());
        server.send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/remove", HTTP_POST, [&server]() {
        JsonDocument doc;
        if (deserializeJson(doc, server.arg("plain"))) {
            server.send(400, "text/plain", "bad json");
            return;
        }
        acRemoveEntry(doc["uid"].as<String>());
        server.send(200, "application/json", "{\"ok\":true}");
    });
}
