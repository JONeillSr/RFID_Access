/**
 * @file    WebService.cpp
 * @brief   Implementation of the reusable device web service.
 */

#include "WebService.h"
#include <ElegantOTA.h>
#include <time.h>   // log timestamps

WebService::WebService(uint16_t port) : _server(port) {
    _logMutex = xSemaphoreCreateMutex();
}

// ---- remote log -------------------------------------------------------------

// Prefix every log line with a time. Without one the ring buffer reads as a set
// of simultaneous assertions rather than a sequence -- "not paired" followed
// later by "paired successfully" looks like a contradiction instead of a
// history.
//
// Wall-clock once NTP has landed, uptime before that, and the two are visually
// distinct (`19:05:12` vs `+127s`) so it is obvious which you are looking at.
// That distinction is not cosmetic: everything logged during the first seconds
// of a boot happens before the clock is trustworthy, and silently printing
// 1970 timestamps would be worse than admitting the clock is not set yet.
static String logStamp() {
    time_t now = time(nullptr);
    char buf[16];
    if (now > 1700000000) {               // plausible wall clock -> NTP is in
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    } else {
        snprintf(buf, sizeof(buf), "+%lus", (unsigned long)(millis() / 1000));
    }
    return String(buf);
}

void WebService::log(const String& line) {
    String stamped = logStamp() + "  " + line;
    Serial.println(stamped);              // always echo to physical serial
    if (!_logMutex) return;
    xSemaphoreTake(_logMutex, portMAX_DELAY);
    _log[_logHead] = stamped;
    _logHead = (_logHead + 1) % LOG_LINES;
    if (_logCount < LOG_LINES) _logCount++;
    // Live-push to a connected telnet client (under the same lock so writes
    // from different tasks don't interleave).
    if (_telnetClient && _telnetClient.connected()) {
        _telnetClient.println(stamped);
    }
    xSemaphoreGive(_logMutex);
}

// ---- HTML escaping ----------------------------------------------------------

// For text between tags: the three characters that would otherwise be parsed
// as markup. Used by the status and log views.
String WebService::escapeText(const String& s) {
    String o;
    o.reserve(s.length() + 16);
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if      (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else               o += c;
    }
    return o;
}

// For values inside a quoted attribute. Escaping only &<> is not enough here:
// this module quotes attributes with single quotes, so an apostrophe in an
// operator-entered value ("John's Office") would close the attribute early and
// mangle every field after it. Both quote styles are escaped so the result is
// safe in either.
String WebService::escapeAttr(const String& s) {
    String o;
    o.reserve(s.length() + 16);
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s[i];
        if      (c == '&')  o += "&amp;";
        else if (c == '<')  o += "&lt;";
        else if (c == '>')  o += "&gt;";
        else if (c == '\'') o += "&#39;";
        else if (c == '"')  o += "&quot;";
        else                o += c;
    }
    return o;
}

String WebService::renderLog() {
    String out;
    if (!_logMutex) return out;
    xSemaphoreTake(_logMutex, portMAX_DELAY);
    // Walk oldest -> newest.
    int start = (_logCount < LOG_LINES) ? 0 : _logHead;
    for (int i = 0; i < _logCount; i++) {
        out += _log[(start + i) % LOG_LINES];
        out += "\n";
    }
    xSemaphoreGive(_logMutex);
    return out;
}

// ---- footer link registry ---------------------------------------------------

void WebService::addFooterLink(const String& label, const String& href,
                               bool external) {
    _footerLinks.push_back({label, href, external});
}

void WebService::setFooterColors(const String& link,
                                 const String& separator,
                                 const String& linkHover) {
    _footerLinkColor = link;
    _footerSepColor  = separator;
    // Empty hover -> reuse the link color (no visible hover color change).
    _footerHoverColor = linkHover.length() ? linkHover : link;
}

void WebService::seedFooterLinks() {
    if (_footerSeeded) return;
    _footerSeeded = true;
    // Built-in admin pages this module serves. They are inserted at the FRONT
    // so they always lead; any project links added via addFooterLink() before
    // begin() follow after them, preserving the order they were added.
    std::vector<FooterLink> builtins = {
        {"Device status",  "/status",    false},
        {"Device log",     "/webserial", false},
        {"Firmware update","/update",    false},
    };
    _footerLinks.insert(_footerLinks.begin(), builtins.begin(), builtins.end());
}

void WebService::handleFooterJson() {
    // Hand-built JSON (no ArduinoJson dependency in this module). Escapes the
    // characters that would break a JSON string or allow HTML injection.
    auto esc = [](const String& s) {
        String o;
        for (unsigned i = 0; i < s.length(); i++) {
            char c = s[i];
            if      (c == '"')  o += "\\\"";
            else if (c == '\\') o += "\\\\";
            else if (c == '<')  o += "\\u003c";
            else if (c == '>')  o += "\\u003e";
            else                o += c;
        }
        return o;
    };
    String out = "{\"links\":[";
    for (size_t i = 0; i < _footerLinks.size(); i++) {
        const FooterLink& l = _footerLinks[i];
        if (i) out += ",";
        out += "{\"label\":\"" + esc(l.label) +
               "\",\"href\":\""  + esc(l.href)  +
               "\",\"ext\":"     + (l.external ? "true" : "false") + "}";
    }
    out += "],\"colors\":{";
    out += "\"link\":\""  + esc(_footerLinkColor)  + "\",";
    out += "\"hover\":\"" + esc(_footerHoverColor) + "\",";
    out += "\"sep\":\""   + esc(_footerSepColor)   + "\"}}";
    _server.send(200, "application/json", out);
}

void WebService::handleFooterScript() {
    // Client-side injector: fetches /footer.json and appends a styled <footer>
    // to whatever page included it via <script src="/footer.js"></script>. The
    // styling is self-contained and theme-matched to the dashboards (dark, with
    // the green accent used elsewhere). Served as a static JS string from flash.
    static const char JS[] PROGMEM = R"JS(
(function(){
  function build(data){
    var links=data.links||[];
    var c=data.colors||{link:'#5fd3a0',hover:'#5fd3a0',sep:'#2a3645'};
    var f=document.createElement('footer');
    f.style.cssText='margin:24px auto 32px;text-align:center;font:14px system-ui,Arial,sans-serif;color:#6b7d90';
    var made=[];
    links.forEach(function(l){
      var a=document.createElement('a');
      a.href=l.href;
      a.textContent=l.label+(l.ext?' \u2192':'');
      a.style.cssText='color:'+c.link+';text-decoration:none;margin:0 4px';
      a.onmouseover=function(){a.style.color=c.hover;a.style.textDecoration='underline';};
      a.onmouseout=function(){a.style.color=c.link;a.style.textDecoration='none';};
      made.push(a);
    });
    made.forEach(function(a,i){
      if(i){var s=document.createElement('span');s.textContent=' | ';s.style.color=c.sep;f.appendChild(s);}
      f.appendChild(a);
    });
    document.body.appendChild(f);
  }
  fetch('/footer.json').then(function(r){return r.json();})
    .then(build)
    .catch(function(){/* footer is non-critical; ignore fetch errors */});
})();
)JS";
    _server.send_P(200, "application/javascript", JS);
}

// ---- routes -----------------------------------------------------------------

// Build just the status text body (no HTML shell). Used by both the page and
// the /status.txt content endpoint that the page polls.
String WebService::statusBody() {
    String body;
    body += "WiFi:     " + WiFi.localIP().toString() + "\n";
    if (_hostname.length())
        body += "mDNS:     " + _hostname + ".local\n";
    body += "Uptime:   " + String(millis() / 1000) + "s\n";
    if (_statusProvider) _statusProvider(body);
    return body;
}

void WebService::handleStatusText() {
    // Content-only endpoint the status page polls; HTML-escaped text.
    _server.send(200, "text/html", WebService::escapeText(statusBody()));
}

void WebService::handleStatus() {
    // Themed page shell (same look as /setup) with a Dashboard button and the
    // shared footer. The status text is polled into the <pre> via fetch() so the
    // header and footer load ONCE and never flicker -- a full-page meta refresh
    // would reload the footer (a separate fetch) and make it disappear briefly.
    String p;
    p.reserve(2000);
    p += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Device Status</title><style>"
         "body{font-family:system-ui,Arial,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}"
         "header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645;"
           "display:flex;justify-content:space-between;align-items:center}"
         "h1{margin:0;font-size:18px}"
         "a.btn{color:#5fd3a0;text-decoration:none;border:1px solid #2a3645;padding:8px 14px;"
           "border-radius:6px;font-size:14px}"
         ".wrap{padding:20px;max-width:560px;margin:0 auto}"
         ".card{background:#161e29;border:1px solid #2a3645;border-radius:10px;padding:18px;margin-top:14px}"
         "pre{margin:0;font-family:ui-monospace,Consolas,monospace;font-size:14px;line-height:1.6;"
           "color:#e6e6e6;white-space:pre-wrap;word-break:break-word}"
         "</style></head><body>"
         "<header><h1>Device Status</h1><a class='btn' href='/'>&larr; Dashboard</a></header>"
         "<div class='wrap'><div class='card'><pre id='body'>";
    p += WebService::escapeText(statusBody());     // initial content (also works without JS)
    p += "</pre></div></div>"
         "<script src='/footer.js'></script>"
         "<script>"
         "function upd(){fetch('/status.txt').then(function(r){return r.text();})"
         ".then(function(t){document.getElementById('body').innerHTML=t;}).catch(function(){});}"
         "setInterval(upd,5000);"
         "</script>"
         "</body></html>";
    _server.send(200, "text/html", p);
}

void WebService::handleLogText() {
    // Content-only endpoint the log page polls; HTML-escaped.
    _server.send(200, "text/html", WebService::escapeText(renderLog()));
}

void WebService::handleWebSerial() {
    // Themed shell matching /status. The log text is polled into the <pre> via
    // fetch() (every 2s) so the header and footer load ONCE and don't flicker --
    // a full-page meta refresh would reload the footer and make it disappear.
    String page =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Device Log</title><style>"
        "body{font-family:system-ui,Arial,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}"
        "header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645;"
          "display:flex;justify-content:space-between;align-items:center}"
        "h1{margin:0;font-size:18px}"
        "a.btn{color:#5fd3a0;text-decoration:none;border:1px solid #2a3645;padding:8px 14px;"
          "border-radius:6px;font-size:14px}"
        ".wrap{padding:20px;max-width:760px;margin:0 auto}"
        ".card{background:#161e29;border:1px solid #2a3645;border-radius:10px;padding:16px}"
        "pre{white-space:pre-wrap;word-break:break-word;margin:0;"
          "font:13px/1.5 ui-monospace,Consolas,monospace;color:#cfe}"
        "</style></head><body>"
        "<header><h1>Device Log</h1><a class='btn' href='/'>&larr; Dashboard</a></header>"
        "<div class='wrap'><div class='card'><pre id='body'>";
    page += WebService::escapeText(renderLog());   // initial content (also works without JS)
    page += "</pre></div></div>"
            "<script src='/footer.js'></script>"
            "<script>"
            "function upd(){fetch('/webserial.txt').then(function(r){return r.text();})"
            ".then(function(t){document.getElementById('body').innerHTML=t;}).catch(function(){});}"
            "setInterval(upd,2000);"
            "</script>"
            "</body></html>";
    _server.send(200, "text/html", page);
}

// ---- telnet -----------------------------------------------------------------

void WebService::serviceTelnet() {
    // Accept a new client. Single-client model: a new connection replaces the
    // old one (keeps the implementation simple for a diagnostic console).
    // The client swap is guarded by _logMutex because log() (called from other
    // tasks) writes to _telnetClient under that same lock.
    if (_telnet.hasClient()) {
        WiFiClient incoming = _telnet.accept();
        xSemaphoreTake(_logMutex, portMAX_DELAY);
        if (_telnetClient && _telnetClient.connected()) {
            _telnetClient.stop();       // drop the previous client
        }
        _telnetClient = incoming;
        _telnetGreeted = false;
        xSemaphoreGive(_logMutex);
    }

    if (_telnetClient && _telnetClient.connected()) {
        if (!_telnetGreeted) {
            // Send the existing backlog once, so a freshly-connected client
            // sees recent history (including the boot-time RC522 line).
            xSemaphoreTake(_logMutex, portMAX_DELAY);
            _telnetClient.println("=== device log (live) ===");
            // inline backlog render while holding the lock (renderLog() would
            // re-take the same non-recursive mutex and deadlock).
            int start = (_logCount < LOG_LINES) ? 0 : _logHead;
            for (int i = 0; i < _logCount; i++) {
                _telnetClient.println(_log[(start + i) % LOG_LINES]);
            }
            _telnetGreeted = true;
            xSemaphoreGive(_logMutex);
        }
        // Drain and discard any input (we don't accept commands).
        while (_telnetClient.available()) _telnetClient.read();
    }
}

// ---- lifecycle --------------------------------------------------------------

void WebService::begin() {
    _server.on("/status",    [this]() { handleStatus();    });
    _server.on("/status.txt",    [this]() { handleStatusText(); });
    _server.on("/webserial", [this]() { handleWebSerial(); });
    _server.on("/webserial.txt", [this]() { handleLogText();    });

    // Reusable footer: seed the built-in page links (project links added via
    // addFooterLink() before begin() are already in the registry) and serve the
    // injector script + link list. Any page that includes
    // <script src="/footer.js"></script> gets a consistent footer for free.
    // If /setup is enabled, register its routes and add it to the footer.
    if (_setupEnabled) {
        _server.on("/setup", HTTP_GET,  [this]() { handleSetupGet();  });
        _server.on("/setup", HTTP_POST, [this]() { handleSetupPost(); });
        // POST-only by design: a GET route here would let a link prefetch, a
        // crawler, or a mistyped URL restart the device.
        _server.on("/reboot", HTTP_POST, [this]() { handleRebootPost(); });
        addFooterLink("Setup", "/setup");
    }
    seedFooterLinks();
    _server.on("/footer.js",   [this]() { handleFooterScript(); });
    _server.on("/footer.json", [this]() { handleFooterJson();   });

    ElegantOTA.begin(&_server);   // /update
    _server.begin();

    _telnet.begin();              // telnet log console on port 23
    _telnet.setNoDelay(true);

    xTaskCreate(driverTask, "web", 8192, this, 1, NULL);

    log("[web] service up: /status /update /webserial /footer.js + telnet:23");
}

void WebService::driverTask(void* pv) {
    WebService* self = static_cast<WebService*>(pv);
    for (;;) {
        self->_server.handleClient();
        ElegantOTA.loop();
        self->serviceTelnet();

        // Deferred reboot requested by POST /reboot. The deadline gives the
        // confirmation response time to flush to the browser before the device
        // goes down. Rollover-safe comparison, as used elsewhere in these
        // projects for millis() deadlines.
        if (self->_rebootAt != 0 &&
            (long)(millis() - self->_rebootAt) >= 0) {
            self->_rebootAt = 0;
            self->log("[web] rebooting now");
            vTaskDelay(pdMS_TO_TICKS(50));   // let the log line reach telnet/serial
            ESP.restart();
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// ============================================================================
//  Reusable /setup page (composed: reusable settings + project fields)
// ============================================================================

void WebService::enableSetup(DeviceSettings* settingsStore) {
    _settings     = settingsStore;
    _setupEnabled = (settingsStore != nullptr);
}

void WebService::handleSetupGet() {
    if (!_settings) { _server.send(503, "text/plain", "settings unavailable"); return; }

    // Read current reusable values.
    uint16_t hold = _settings->splashHoldSec();
    uint16_t diag = _settings->diagHoldSec();
    String   board = _settings->board();
    String   host  = _settings->hostname(_hostname);   // fall back to runtime label

    // Page shell + reusable settings block. The styling matches the dark
    // dashboard theme used elsewhere.
    String p;
    p.reserve(3000);
    p += "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Device Setup</title><style>"
         "body{font-family:system-ui,Arial,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}"
         "header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645;"
           "display:flex;justify-content:space-between;align-items:center}"
         "h1{margin:0;font-size:18px}h2{font-size:14px;color:#9fb3c8;margin:22px 0 8px}"
         "a.btn{color:#5fd3a0;text-decoration:none;border:1px solid #2a3645;padding:8px 14px;"
           "border-radius:6px;font-size:14px}"
         ".wrap{padding:20px;max-width:560px;margin:0 auto}"
         ".card{background:#161e29;border:1px solid #2a3645;border-radius:10px;padding:18px;margin-top:14px}"
         "label{display:block;margin:12px 0 4px;font-size:13px;color:#9fb3c8}"
         "input,select{width:100%;box-sizing:border-box;background:#0f1419;border:1px solid #2a3645;"
           "color:#e6e6e6;padding:9px 10px;border-radius:5px;font-size:15px}"
         "input:focus,select:focus{outline:none;border-color:#2563eb}"
         ".hint{color:#6b7d90;font-size:12px;margin-top:4px}"
         "button{margin-top:18px;background:#2563eb;color:#fff;border:0;padding:11px 18px;"
           "border-radius:6px;font-size:15px;font-weight:600;cursor:pointer}"
         "button.danger{background:#7f1d1d}"
         "button.danger:hover{background:#991b1b}"
         "</style></head><body>"
         "<header><h1>Device Setup</h1><a class='btn' href='/'>&larr; Dashboard</a></header>"
         "<div class='wrap'><form method='POST' action='/setup'>";

    // ---- Reusable settings block (owned by WebService) ----
    p += "<h2>Device settings</h2><div class='card'>";
    p += "<label>Splash minimum hold (seconds)</label>";
    p += "<input type='number' name='splashHold' min='0' max='60' value='" + String(hold) + "'>";
    p += "<div class='hint'>How long the boot logo stays up before diagnostics. 0 = no hold.</div>";

    p += "<label>Boot diagnostics hold (seconds)</label>";
    p += "<input type='number' name='diagHold' min='0' max='60' value='" + String(diag) + "'>";
    p += "<div class='hint'>How long the per-subsystem boot status stays up before live data. 0 = skip.</div>";

    p += "<label>Preferred board</label>";
    p += "<input type='text' name='board' value='" + escapeAttr(board) + "' placeholder='e.g. esp32dev'>";
    p += "<div class='hint'>Free-form board identifier this project understands.</div>";

    p += "<label>mDNS hostname</label>";
    p += "<input type='text' name='hostname' value='" + escapeAttr(host) + "' placeholder='e.g. filament'>";
    p += "<div class='hint'>Device is reachable at &lt;hostname&gt;.local on the network.</div>";
    p += "</div>";

    // ---- Project-specific block (injected) ----
    if (_setupFields) {
        String projHtml;
        _setupFields(projHtml);
        if (projHtml.length()) {
            p += "<h2>Project settings</h2><div class='card'>";
            p += projHtml;
            p += "</div>";
        }
    }

    p += "<button type='submit'>Save</button></form>";

    // ---- Maintenance block ----
    // Its own form, deliberately OUTSIDE the settings form above: nested forms
    // are invalid HTML and browsers silently drop the inner one, which would
    // leave the reboot button submitting the settings form instead.
    p += "<h2>Maintenance</h2><div class='card'>";
    p += "<form method='POST' action='/reboot' "
         "onsubmit=\"return confirm('Reboot this device now?');\">";
    p += "<button class='danger' type='submit'>Reboot device</button>";
    p += "</form>";
    p += "<div class='hint'>Restarts the device; it drops off the network for a few "
         "seconds. Save any changes above first &mdash; rebooting does not save them. "
         "A changed mDNS hostname only takes effect after a reboot.</div>";
    p += "</div>";

    p += "</div>";   // .wrap
    p += "<script src='/footer.js'></script></body></html>";
    _server.send(200, "text/html", p);
}

void WebService::handleRebootPost() {
    log("[web] reboot requested from " + _server.client().remoteIP().toString());

    // Schedule rather than restart here: send() only queues the response, so
    // restarting inside the handler would drop the connection before the
    // confirmation page reached the browser, leaving the operator unsure
    // whether the request landed at all. driverTask performs the restart.
    _rebootAt = millis() + 1200;

    String p =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='12;url=/setup'>"
        "<title>Rebooting</title><style>"
        "body{font-family:system-ui,sans-serif;background:#0f1419;color:#5fd3a0;"
          "text-align:center;padding:60px 20px}"
        "p{color:#8aa0b4;font-size:14px}</style></head><body>"
        "<h2>Rebooting&hellip;</h2>"
        "<p>This page reloads automatically once the device is back.</p>"
        "<p>If you just changed the mDNS hostname, the device now answers to its "
        "new name &mdash; this page's address is stale and won't resolve.</p>"
        "</body></html>";
    _server.send(200, "text/html", p);
}

void WebService::handleSetupPost() {
    if (!_settings) { _server.send(503, "text/plain", "settings unavailable"); return; }

    // ---- Save reusable settings ----
    if (_server.hasArg("splashHold")) {
        long v = _server.arg("splashHold").toInt();
        if (v < 0)  v = 0;
        if (v > 60) v = 60;
        _settings->setSplashHoldSec((uint16_t)v);
    }
    if (_server.hasArg("diagHold")) {
        long v = _server.arg("diagHold").toInt();
        if (v < 0)  v = 0;
        if (v > 60) v = 60;
        _settings->setDiagHoldSec((uint16_t)v);
    }
    if (_server.hasArg("board")) {
        _settings->setBoard(_server.arg("board"));
    }
    if (_server.hasArg("hostname")) {
        String h = _server.arg("hostname");
        _settings->setHostname(h);
        if (h.length()) _hostname = h;   // update the runtime label too
    }

    // ---- Let the project save its own fields ----
    if (_setupSave) _setupSave(_server);

    // Confirmation page with a short auto-redirect back to setup.
    String p =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='1;url=/setup'>"
        "<title>Saved</title><style>"
        "body{font-family:system-ui,sans-serif;background:#0f1419;color:#5fd3a0;"
          "text-align:center;padding:60px 20px}</style></head><body>"
        "<h2>&#x2713; Saved</h2><p style='color:#8aa0b4'>Returning to setup&hellip;</p>"
        "</body></html>";
    _server.send(200, "text/html", p);
}
