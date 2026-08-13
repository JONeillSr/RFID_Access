#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <functional>

class WiFiManager {
public:
    using StatusCallback = std::function<void()>;

    // apSsid:           SSID of the captive-portal AP created when no credentials are stored
    // apPass:           AP password; empty or fewer than 8 chars = open network
    // connectTimeoutMs: how long to attempt STA connection before falling back to AP mode
    WiFiManager(const char* apSsid,
                const char* apPass           = "",
                uint32_t    connectTimeoutMs = 10000);

    void onConnected(StatusCallback cb)          { _onConnected = cb; }
    void onDisconnected(StatusCallback cb)        { _onDisconnected = cb; }
    void onProvisioningStarted(StatusCallback cb) { _onProvisioningStarted = cb; }

    // mDNS hostname: the device becomes reachable as http://<hostname>.local
    // once connected in STA mode. Call before begin(). If never set, mDNS is
    // not started. Per-deployment units should each get a unique name.
    void   setHostname(const char* name) { _hostname = name; }
    String getHostname() const           { return _hostname; }

    // NTP time sync: tz is a POSIX TZ string (e.g. "EST5EDT,M3.2.0,M11.1.0");
    // sync starts on every transition into the connected state, alongside
    // mDNS. Call before begin(). If never set, time sync is not started.
    void setTimeSync(const char* tz,
                     const char* ntp1 = "pool.ntp.org",
                     const char* ntp2 = "time.nist.gov") {
        _tzInfo = tz; _ntp1 = ntp1; _ntp2 = ntp2;
    }

    // Blocking: reads NVS credentials, tries STA, falls back to provisioning AP on failure.
    // After begin() returns, exactly one of isConnected() or isProvisioning() is true.
    void begin();

    // Non-blocking: call from loop(). Handles STA reconnect only.
    // AP/portal mode is driven by an internal task created in begin().
    void loop();

    bool isConnected()    const;
    bool isProvisioning() const { return _state == STATE_AP; }
    IPAddress localIP()   const;

    // Erase stored credentials and reboot into provisioning mode.
    void clearCredentials();

private:
    enum State { STATE_STA, STATE_AP };

    const char*   _apSsid;
    const char*   _apPass;
    uint32_t      _connectTimeout;
    State         _state        = STATE_STA;
    bool          _wasConnected = false;
    unsigned long _lastCheck    = 0;
    String        _hostname     = "";       // empty = mDNS disabled
    String        _tzInfo       = "";       // empty = NTP time sync disabled
    String        _ntp1, _ntp2;             // NTP servers (set with _tzInfo)

    WebServer*   _portalServer = nullptr;
    DNSServer*   _dnsServer    = nullptr;
    TaskHandle_t _portalTask   = nullptr;   // so the portal can be torn down
    Preferences  _prefs;
    SemaphoreHandle_t _nvsMutex = nullptr;  // guards _prefs across tasks

    StatusCallback _onConnected           = nullptr;
    StatusCallback _onDisconnected        = nullptr;
    StatusCallback _onProvisioningStarted = nullptr;

    bool   loadCredentials(String& ssid, String& pass);
    void   saveCredentials(const String& ssid, const String& pass);
    void   startSTA(const String& ssid, const String& pass);
    void   startMDNS();             // (re)start mDNS responder if a hostname is set
    void   startTimeSync();         // (re)start SNTP if a timezone is set
    void   startAP();
    void   stopAP();                // release portal server/DNS/task
    void   setupPortalRoutes();
    String buildNetworkList();
    String buildPortalPage(const String& networkList, const String& errorMsg = "");
    String buildSavedPage(const String& ssid);
};
