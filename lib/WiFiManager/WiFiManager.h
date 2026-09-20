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

    /// Seconds since the clock was last actually corrected by NTP; -1 if it
    /// never has been. "The time looks right" and "the time is being maintained"
    /// are different claims, and only the first one is visible without this.
    /// Static: the SNTP callback is a bare function pointer with nowhere to hang
    /// an instance, so the timestamp is file-scope in the .cpp.
    static int32_t secsSinceTimeSync();

    // Blocking: reads NVS credentials, tries STA, falls back to provisioning AP on failure.
    // After begin() returns, exactly one of isConnected() or isProvisioning() is true.
    void begin();

    // Non-blocking: call from loop(). In STA mode, supervises reconnects. In
    // portal mode, keeps retrying the saved network and reboots once it can be
    // joined -- see retrySavedNetwork(). The portal itself is served by an
    // internal task created in begin().
    void loop();

    bool isConnected()    const;
    bool isProvisioning() const { return _state == STATE_AP; }
    IPAddress localIP()   const;

    // Erase stored credentials and reboot into provisioning mode.
    //
    // DESTRUCTIVE AND UNRECOVERABLE FROM THE NETWORK: the device comes back in
    // the portal, reachable only by someone in radio range. Prefer
    // changeNetwork(), which reverts on failure, or openSetupAP(), which raises
    // the portal without giving up the working connection.
    void clearCredentials();

    // ---- Moving to another network ----------------------------------------

    /// What a requested network change is doing. Read for a status page.
    enum ChangeState : uint8_t {
        CHANGE_IDLE = 0,
        CHANGE_TRYING,     // attempting the new network
        CHANGE_REVERTING,  // it failed; going back to the one that worked
        CHANGE_DONE,       // joined the new network
        CHANGE_FAILED,     // could not join; the previous network was restored
    };

    /// Ask to move to another network, WITHOUT risking the door.
    ///
    /// Returns immediately; the work happens in loop(), so the caller (a web
    /// handler) can answer before the radio drops the connection the request
    /// arrived on. The new credentials are tried, kept only once the device has
    /// an address, and otherwise replaced by the ones that worked -- a typo
    /// costs a reconnect, not a visit. False if a change is already running.
    bool changeNetwork(const String& ssid, const String& pass);

    ChangeState changeState() const { return _change; }
    /// SSID of the change in flight, or of the last one attempted.
    String      changeSsid()  const { return _newSsid; }

    /// Raise the setup AP while STAYING on the current network.
    ///
    /// Unlike the boot-time portal this does not take the device off the air and
    /// does not start a web server: in WIFI_AP_STA one server on port 80 already
    /// answers on both interfaces, and the application owns that server. The
    /// caller is expected to serve its own Wi-Fi page (see changeNetwork).
    ///
    /// For the case the portal cannot reach: a door on a network that still
    /// works, which has to be moved to one it cannot see from there.
    void openSetupAP();
    void closeSetupAP();
    bool setupAPOpen() const { return _setupAP; }

    /// Skip the saved network at boot and go straight to the portal. Call before
    /// begin(). Used for a physical recovery path (holding the exit button at
    /// power-on), so a door on an unreachable network can be re-homed with no
    /// network and no USB. The saved credentials are NOT erased, and the
    /// background retry is suppressed so the portal does not reboot away
    /// underneath whoever is standing there using it.
    void forcePortal() { _forcePortal = true; }

private:
    enum State { STATE_STA, STATE_AP };

    const char*   _apSsid;
    const char*   _apPass;
    uint32_t      _connectTimeout;
    State         _state        = STATE_STA;
    bool          _forcePortal  = false;
    bool          _setupAP      = false;   // AP raised alongside a live STA link

    // Network change in flight. Driven from loop(); see changeNetwork().
    ChangeState _change        = CHANGE_IDLE;
    String      _newSsid, _newPass;        // the candidate
    String      _oldSsid, _oldPass;        // what to go back to
    uint32_t    _changeStartedMs = 0;
    bool        _changePending   = false;  // requested, not yet started
    bool          _wasConnected = false;
    unsigned long _lastCheck    = 0;

    // Portal-mode retry of the saved network (see retrySavedNetwork()).
    uint32_t _retryStartedMs = 0;           // 0 = no attempt in progress
    uint32_t _lastRetryEndMs = 0;
    // millis() of the last page a person loaded from the portal; 0 = never.
    // Written by the portal task, read by the main loop: a 32-bit aligned
    // load/store is atomic on ESP32.
    volatile uint32_t _lastPortalUseMs = 0;
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
    void   driveNetworkChange(uint32_t now);   // the change state machine
    void   stopAP();                // release portal server/DNS/task
    void   retrySavedNetwork();     // portal mode: find the way back on our own
    void   setupPortalRoutes();
    String buildNetworkList();
    String buildPortalPage(const String& networkList, const String& errorMsg = "");
    String buildSavedPage(const String& ssid);
};
