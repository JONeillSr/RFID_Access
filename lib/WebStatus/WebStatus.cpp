/**
 * @file    WebStatus.cpp
 * @brief   Implementation of the status + scan-results web page.
 */

#include "WebStatus.h"
#include "ScanState.h"
#include <WiFi.h>

// Append one bus's results to an HTML string. Caller holds the scan lock.
static void appendBusHtml(String& h, const char* label, const char* pins,
                          const BusResult& bus, bool active) {
    h += "<h2>";
    h += label;
    if (pins[0]) { h += " <span class='pins'>("; h += pins; h += ")</span>"; }
    h += "</h2>";

    if (!active) {
        h += "<p class='off'>Disabled on this board (ESP32-C6 Wire1).</p>";
        return;
    }
    if (bus.count == 0) {
        h += "<p class='empty'>No devices found.</p>";
        return;
    }
    h += "<table><tr><th>Address</th><th>Chip</th></tr>";
    for (int i = 0; i < bus.count; i++) {
        char addr[8];
        snprintf(addr, sizeof(addr), "0x%02X", bus.found[i].addr);
        h += "<tr><td class='addr'>";
        h += addr;
        h += "</td><td>";
        h += bus.found[i].chip[0] ? bus.found[i].chip : "&mdash;";
        h += "</td></tr>";
    }
    h += "</table>";
}

static String buildStatusPage() {
    String h;
    h.reserve(2200);
    h += "<!DOCTYPE html><html><head>"
         "<meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>I2C Scanner</title><style>"
         "body{font-family:system-ui,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}"
         "header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645;"
              "display:flex;justify-content:space-between;align-items:center}"
         "h1{margin:0;font-size:18px}h2{font-size:14px;color:#9fb3c8;margin:20px 0 8px}"
         ".pins{color:#6b7d90;font-weight:400}"
         ".wrap{padding:20px;max-width:560px;margin:0 auto}"
         "a.btn{color:#5fd3a0;text-decoration:none;border:1px solid #2a3645;"
              "padding:8px 14px;border-radius:6px;font-size:14px}"
         "table{width:100%;border-collapse:collapse;background:#161e29;"
              "border-radius:8px;overflow:hidden;margin-top:4px}"
         "th,td{text-align:left;padding:9px 14px;font-size:14px}"
         "th{background:#202c3a;color:#9fb3c8}tr:nth-child(even) td{background:#19222e}"
         "td.addr{font-family:ui-monospace,monospace;color:#7fb8ff;font-weight:600}"
         ".empty,.off{color:#6b7d90;font-size:14px;padding:4px 0}"
         ".meta{color:#6b7d90;font-size:13px;margin-top:16px}"
         "</style></head><body>"
         "<header><h1>I2C Scanner</h1>"
         "<div><a class='btn' href='/rescan'>Rescan</a> "
         "<a class='btn' href='/update'>OTA</a></div></header>"
         "<div class='wrap'>";

    SCAN_LOCK();
    appendBusHtml(h, "Bus 0", "SDA 22 / SCL 23", g_bus0, true);
#ifdef BOARD_XIAO_ESP32C6
    appendBusHtml(h, "Bus 1", "", g_bus1, false);
#else
    appendBusHtml(h, "Bus 1", "SDA 20 / SCL 19", g_bus1, g_bus1Active);
#endif
    unsigned long ageMs = millis() - g_lastScanMs;
    SCAN_UNLOCK();

    h += "<p class='meta'>Last scan ";
    h += String(ageMs / 1000);
    h += "s ago &middot; IP ";
    h += WiFi.localIP().toString();
    h += " &middot; uptime ";
    h += String(millis() / 1000);
    h += "s</p></div></body></html>";
    return h;
}

static String buildScanJson() {
    String j = "{";
    SCAN_LOCK();
    j += "\"lastScanMs\":" + String(g_lastScanMs);
    j += ",\"uptimeMs\":" + String(millis());

    auto busJson = [](const BusResult& bus) {
        String s = "[";
        for (int i = 0; i < bus.count; i++) {
            if (i) s += ",";
            char addr[8];
            snprintf(addr, sizeof(addr), "0x%02X", bus.found[i].addr);
            s += "{\"addr\":\"";
            s += addr;
            s += "\",\"chip\":\"";
            s += bus.found[i].chip;
            s += "\"}";
        }
        s += "]";
        return s;
    };

    j += ",\"bus0\":" + busJson(g_bus0);
    j += ",\"bus1Active\":";
    j += g_bus1Active ? "true" : "false";
    j += ",\"bus1\":" + busJson(g_bus1);
    SCAN_UNLOCK();
    j += "}";
    return j;
}

void registerStatusHandlers(WebServer& server) {
    server.on("/", [&server]() {
        server.send(200, "text/html", buildStatusPage());
    });

    server.on("/api/scan", [&server]() {
        server.send(200, "application/json", buildScanJson());
    });

    server.on("/rescan", [&server]() {
        runScan();   // acquires the lock internally
        server.sendHeader("Location", "/");
        server.send(303, "text/plain", "rescanning");
    });
}
