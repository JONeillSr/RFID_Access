#include "WiFiManager.h"

static const uint8_t DNS_PORT = 53;

WiFiManager::WiFiManager(const char* apSsid, const char* apPass, uint32_t connectTimeoutMs)
    : _apSsid(apSsid), _apPass(apPass), _connectTimeout(connectTimeoutMs) {
    // Guards the shared Preferences object: the portal task writes credentials
    // while the main task may read them.
    _nvsMutex = xSemaphoreCreateMutex();
}

bool WiFiManager::isConnected() const {
    return _state == STATE_STA && WiFi.status() == WL_CONNECTED;
}

IPAddress WiFiManager::localIP() const {
    return (_state == STATE_STA) ? WiFi.localIP() : WiFi.softAPIP();
}

// ===== Credential storage (NVS namespace "wifimgr") =====

bool WiFiManager::loadCredentials(String& ssid, String& pass) {
    if (_nvsMutex) xSemaphoreTake(_nvsMutex, portMAX_DELAY);
    _prefs.begin("wifimgr", true);
    ssid = _prefs.getString("ssid", "");
    pass = _prefs.getString("pass", "");
    _prefs.end();
    if (_nvsMutex) xSemaphoreGive(_nvsMutex);
    return ssid.length() > 0;
}

void WiFiManager::saveCredentials(const String& ssid, const String& pass) {
    if (_nvsMutex) xSemaphoreTake(_nvsMutex, portMAX_DELAY);
    _prefs.begin("wifimgr", false);
    _prefs.putString("ssid", ssid);
    _prefs.putString("pass", pass);
    _prefs.end();
    if (_nvsMutex) xSemaphoreGive(_nvsMutex);
}

void WiFiManager::clearCredentials() {
    if (_nvsMutex) xSemaphoreTake(_nvsMutex, portMAX_DELAY);
    _prefs.begin("wifimgr", false);
    _prefs.clear();
    _prefs.end();
    if (_nvsMutex) xSemaphoreGive(_nvsMutex);
    ESP.restart();
}

// ===== STA mode =====

void WiFiManager::startSTA(const String& ssid, const String& pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
}

// Start (or restart) the mDNS responder so the device is reachable as
// http://<hostname>.local. Called on every transition into the connected
// state. No-op when no hostname has been set. MDNS.end() first makes this
// safe to call again on reconnect without leaving a stale responder.
void WiFiManager::startMDNS() {
    if (_hostname.length() == 0) return;
    MDNS.end();
    if (MDNS.begin(_hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.print("[WiFi] mDNS responder: http://");
        Serial.print(_hostname);
        Serial.println(".local");
    } else {
        Serial.println("[WiFi] mDNS start failed");
    }
}

// Start (or refresh) SNTP time sync. Called on every transition into the
// connected state, like startMDNS(). configTzTime is idempotent, so calling
// it again on reconnect just re-arms the sync; the system clock keeps
// ticking between syncs and across WiFi drops. No-op until setTimeSync()
// has provided a timezone.
void WiFiManager::startTimeSync() {
    if (_tzInfo.length() == 0) return;
    configTzTime(_tzInfo.c_str(), _ntp1.c_str(), _ntp2.c_str());
    Serial.println("[WiFi] NTP time sync started");
}

// ===== Captive-portal helpers =====

static String htmlEncode(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if      (c == '&')  out += "&amp;";
        else if (c == '<')  out += "&lt;";
        else if (c == '>')  out += "&gt;";
        else if (c == '"')  out += "&quot;";
        else if (c == '\'') out += "&#39;";
        else                out += c;
    }
    return out;
}

String WiFiManager::buildNetworkList() {
    // WIFI_AP_STA mode is set in startAP(), so scanning is possible while the AP is up.
    int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
    if (n <= 0) {
        return "<p style='color:#6b7d90;font-size:14px'>No networks found. "
               "<a href='/' style='color:#5fd3a0'>Tap to rescan</a></p>";
    }
    String html;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;   // skip hidden networks
        bool   secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        String enc     = htmlEncode(ssid);
        html += "<div class='net' data-ssid='" + enc + "'>";
        html += "<span class='ssid'>" + enc;
        if (secured) html += " &#x1F512;";
        html += "</span><span class='rssi'>" + String(WiFi.RSSI(i)) + " dBm</span></div>";
    }
    WiFi.scanDelete();
    return html;
}

String WiFiManager::buildPortalPage(const String& networkList, const String& errorMsg) {
    String p;
    p.reserve(2500);
    p += "<!DOCTYPE html><html><head>"
         "<meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>WiFi Setup</title><style>"
         "body{font-family:system-ui,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}"
         "header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645}"
         "h1{margin:0;font-size:18px}"
         ".wrap{padding:20px;max-width:480px;margin:0 auto}"
         "h2{font-size:14px;color:#9fb3c8;margin:20px 0 8px}"
         ".net{background:#161e29;border:1px solid #2a3645;border-radius:6px;"
              "padding:10px 14px;margin:6px 0;cursor:pointer;"
              "display:flex;justify-content:space-between;align-items:center}"
         ".net:hover,.net:active{background:#1e2d3e;border-color:#3a5068}"
         ".ssid{font-weight:600}.rssi{font-size:12px;color:#6b7d90}"
         "label{display:block;margin:14px 0 4px;font-size:14px;color:#9fb3c8}"
         "input{width:100%;box-sizing:border-box;background:#0f1419;"
               "border:1px solid #2a3645;color:#e6e6e6;"
               "padding:9px 10px;border-radius:5px;font-size:15px}"
         "input:focus{outline:none;border-color:#2563eb}"
         ".row{display:flex;gap:10px;margin-top:20px}"
         "button{flex:1;padding:12px;border:0;border-radius:6px;"
                "font-size:15px;cursor:pointer;font-weight:600}"
         ".bs{background:#2563eb;color:#fff}"
         ".br{background:#161e29;color:#9fb3c8;border:1px solid #2a3645}"
         ".err{margin-top:12px;padding:10px 14px;border-radius:6px;"
              "background:#3d1420;border:1px solid #6b2030;color:#e0556b;font-size:14px}"
         "</style></head><body>"
         "<header><h1>WiFi Setup</h1></header>"
         "<div class='wrap'><h2>Available networks</h2>";
    p += networkList;
    if (errorMsg.length()) p += "<div class='err'>" + errorMsg + "</div>";
    p += "<h2>Credentials</h2>"
         "<form method='POST' action='/save'>"
         "<label>SSID</label>"
         "<input name='ssid' id='s' autocomplete='off' placeholder='Network name'>"
         "<label>Password</label>"
         "<input name='pass' type='password' autocomplete='new-password' "
                "placeholder='Leave blank for open networks'>"
         "<div class='row'>"
         "<button type='button' class='br' onclick='location.reload()'>&#x21BB; Rescan</button>"
         "<button type='submit' class='bs'>Save &amp; Connect</button>"
         "</div></form></div>"
         "<script>"
         "document.querySelectorAll('.net').forEach(e=>{"
         "e.onclick=()=>{"
         "document.getElementById('s').value=e.dataset.ssid;"
         "document.getElementById('s').focus();"
         "};"
         "});"
         "</script></body></html>";
    return p;
}

String WiFiManager::buildSavedPage(const String& ssid) {
    String p;
    p.reserve(700);
    p += "<!DOCTYPE html><html><head>"
         "<meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>WiFi Setup</title><style>"
         "body{font-family:system-ui,sans-serif;margin:0;background:#0f1419;color:#e6e6e6}"
         "header{background:#1b2430;padding:16px 20px;border-bottom:1px solid #2a3645}"
         "h1{margin:0;font-size:18px}"
         ".w{padding:40px 20px;max-width:480px;margin:0 auto;text-align:center}"
         ".ok{background:#143d2e;border:1px solid #1e6b45;border-radius:8px;padding:24px}"
         "h2{margin:0 0 8px;font-size:20px;color:#5fd3a0}"
         "p{margin:8px 0 0;color:#8aa0b4;font-size:14px}"
         "</style></head><body>"
         "<header><h1>WiFi Setup</h1></header>"
         "<div class='w'><div class='ok'>"
         "<h2>&#x2713; Saved!</h2><p>Connecting to <strong>";
    p += htmlEncode(ssid);
    p += "</strong>&hellip;</p>"
         "<p>Device is rebooting. Reconnect to your home network, "
         "then visit the device's IP address.</p>"
         "</div></div></body></html>";
    return p;
}

void WiFiManager::setupPortalRoutes() {
    _portalServer->on("/", [this]() {
        String nets = buildNetworkList();
        _portalServer->send(200, "text/html", buildPortalPage(nets));
    });

    _portalServer->on("/save", HTTP_POST, [this]() {
        String ssid = _portalServer->arg("ssid");
        String pass = _portalServer->arg("pass");
        if (ssid.length() == 0) {
            String nets = buildNetworkList();
            _portalServer->send(200, "text/html",
                                buildPortalPage(nets, "SSID cannot be empty."));
            return;
        }
        saveCredentials(ssid, pass);
        _portalServer->send(200, "text/html", buildSavedPage(ssid));
        // Give the browser time to receive the response before rebooting.
        // Reboot to apply: on restart, begin() reads the new credentials,
        // connects in STA mode, and the app server starts cleanly. Rebooting
        // is simpler and more robust than tearing down the AP and switching the
        // radio to STA live while a client is mid-request.
        // This runs inside the portal FreeRTOS task, so vTaskDelay is safe.
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP.restart();
    });

    // Redirect captive-portal detection requests (iOS, Android, Windows) to the setup page.
    _portalServer->onNotFound([this]() {
        _portalServer->sendHeader("Location", "http://192.168.4.1/");
        _portalServer->send(302, "text/plain", "");
    });
}

// ===== AP mode =====

void WiFiManager::startAP() {
    // Guard against double-start: if a portal is already up, tear it down first
    // so we never leak the server/DNS objects or spawn a duplicate task.
    if (_state == STATE_AP || _portalServer != nullptr) {
        stopAP();
    }

    _state = STATE_AP;

    // WIFI_AP_STA so we can still scan for nearby networks while the AP is up.
    WiFi.mode(WIFI_AP_STA);
    if (strlen(_apPass) >= 8) {
        WiFi.softAP(_apSsid, _apPass);
    } else {
        WiFi.softAP(_apSsid);
    }
    delay(100);  // let the AP interface settle before reading softAPIP

    IPAddress apIP = WiFi.softAPIP();
    Serial.print("[WiFi] Provisioning AP: "); Serial.println(_apSsid);
    Serial.print("[WiFi] Connect to AP and open: http://"); Serial.println(apIP);

    _dnsServer = new DNSServer();
    _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    _dnsServer->start(DNS_PORT, "*", apIP);   // redirect ALL DNS queries to the device

    _portalServer = new WebServer(80);
    setupPortalRoutes();
    _portalServer->begin();

    // Dedicated task drives the DNS server and portal web server. The handle is
    // stored so stopAP() can delete it cleanly.
    xTaskCreate([](void* pv) {
        WiFiManager* self = static_cast<WiFiManager*>(pv);
        for (;;) {
            self->_dnsServer->processNextRequest();
            self->_portalServer->handleClient();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }, "portal", 4096, this, 1, &_portalTask);

    if (_onProvisioningStarted) _onProvisioningStarted();
}

void WiFiManager::stopAP() {
    // Delete the portal task first so it can't touch the server/DNS while we
    // free them.
    if (_portalTask != nullptr) {
        vTaskDelete(_portalTask);
        _portalTask = nullptr;
    }
    if (_portalServer != nullptr) {
        _portalServer->stop();
        delete _portalServer;
        _portalServer = nullptr;
    }
    if (_dnsServer != nullptr) {
        _dnsServer->stop();
        delete _dnsServer;
        _dnsServer = nullptr;
    }
    WiFi.softAPdisconnect(true);  // bring down the soft-AP interface
}

// ===== Public interface =====

void WiFiManager::begin() {
    String ssid, pass;
    if (loadCredentials(ssid, pass)) {
        Serial.print("[WiFi] Connecting to "); Serial.println(ssid);
        startSTA(ssid, pass);
        // One-time boot-up connect attempt. This yields to the scheduler via
        // vTaskDelay (not delay()), so other tasks already created keep running
        // while we wait. A brief wait here is acceptable: it only happens once
        // at startup, and the caller wants to know the outcome before deciding
        // whether to start app services or fall back to provisioning.
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < _connectTimeout) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (WiFi.status() == WL_CONNECTED) {
            _state = STATE_STA;
            _wasConnected = true;
            startMDNS();
            startTimeSync();
            if (_onConnected) _onConnected();
            return;
        }
        Serial.println("[WiFi] Connection timed out - starting provisioning AP.");
    } else {
        Serial.println("[WiFi] No credentials stored - starting provisioning AP.");
    }
    startAP();
}

void WiFiManager::loop() {
    if (_state != STATE_STA) return;   // AP mode is self-driven by the portal task

    unsigned long now = millis();
    if (now - _lastCheck < 5000) return;
    _lastCheck = now;

    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected && !_wasConnected) {
        _wasConnected = true;
        startMDNS();                  // re-establish responder after reconnect
        startTimeSync();              // re-arm SNTP after reconnect
        if (_onConnected) _onConnected();
    } else if (!connected && _wasConnected) {
        _wasConnected = false;
        if (_onDisconnected) _onDisconnected();
        String ssid, pass;
        if (loadCredentials(ssid, pass)) WiFi.begin(ssid.c_str(), pass.c_str());
    } else if (!connected) {
        WiFi.reconnect();
    }
}
