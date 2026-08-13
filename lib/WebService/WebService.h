/**
 * @file    WebService.h
 * @brief   Reusable device web service: owns a long-lived WebServer and provides
 *          the standard admin surface every project wants — /status, /update
 *          (OTA), and /webserial — plus a hook for project-specific routes.
 *
 * Design:
 *   - The module CREATES and OWNS the WebServer (port configurable, default 80)
 *     and drives it from its own FreeRTOS task. Projects never touch the server
 *     lifecycle.
 *   - /status is owned here for the common fields (WiFi IP, mDNS name, uptime).
 *     The project injects its own app-specific lines via setStatusProvider().
 *   - /update is ElegantOTA, registered here so OTA has a proper home.
 *   - /webserial is a minimal log viewer served from this same (sync) server,
 *     with no async dependency — it reads a ring buffer fed by log().
 *   - Projects add their own routes via routes() -> the underlying WebServer,
 *     before begin().
 *
 * Connectivity (WiFi/provisioning/mDNS) is intentionally NOT here — that is
 * WiFiManager's responsibility. This module assumes the network is already up
 * and only starts when begin() is called (the caller gates that on STA mode).
 */

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <functional>
#include <vector>
#include "DeviceSettings.h"

class WebService {
public:
    // Appends app-specific lines to the /status body. The project implements
    // this; the module supplies the common fields around it.
    using StatusProvider = std::function<void(String& body)>;

    explicit WebService(uint16_t port = 80);

    // mDNS/host label shown on /status (purely cosmetic here; the actual mDNS
    // responder is started by WiFiManager). Optional.
    void setHostname(const String& host) { _hostname = host; }

    // Register a provider that appends project-specific status lines.
    void setStatusProvider(StatusProvider p) { _statusProvider = p; }

    // ---- Reusable page footer ---------------------------------------------
    // The module serves a small client-side script at /footer.js that injects a
    // consistent footer (a row of links to the device's other pages) into ANY
    // page that includes it. A project's main page opts in with a single line:
    //     <script src="/footer.js"></script>
    // placed just before </body>. The footer's links come from a registry that
    // is pre-seeded in begin() with this module's own built-in pages
    // (/status, /webserial, /update) and can be extended per project.
    //
    // Call addFooterLink() BEFORE begin() to add a project-specific entry (e.g.
    // a "Setup" page the project itself serves). `external` adds a trailing
    // arrow, a cosmetic hint that the link leaves the dashboard.
    void addFooterLink(const String& label, const String& href,
                       bool external = false);

    // Replace the entire footer link list (advanced; clears the built-ins too).
    // Most projects should use addFooterLink() instead.
    void clearFooterLinks() { _footerLinks.clear(); }

    // Customize the footer colors to match a project theme. All are CSS color
    // strings (e.g. "#5fd3a0", "rgb(...)", "white"). Defaults match the dark
    // dashboard theme used across these projects. Call before begin() (or any
    // time; the values are read when /footer.json is requested).
    //   link      = link text color
    //   linkHover = link color on hover (defaults to link if left empty)
    //   separator = the " | " divider color
    void setFooterColors(const String& link,
                         const String& separator,
                         const String& linkHover = "");

    // Access the underlying server to register extra project routes BEFORE begin().
    WebServer& routes() { return _server; }

    // ---- Reusable /setup page ---------------------------------------------
    // WebService can serve a composed /setup page: a reusable settings block it
    // owns (splash minimum hold, preferred board, mDNS hostname) PLUS a
    // project-specific block the project injects. This mirrors how /status
    // composes common fields with a project status provider.
    //
    // To enable /setup, give WebService the settings store and (optionally) a
    // project field provider + save handler, BEFORE begin():
    //
    //   web.enableSetup(&settings);
    //   web.setSetupFieldsProvider([](String& html){ /* append <label><input> */ });
    //   web.setSetupSaveHandler([](WebServer& s){ /* read s.arg(...) + persist */ });
    //
    // The reusable fields are rendered, saved to the store, and (for splash hold
    // and hostname) applied automatically. The project provider appends its own
    // form fields; the project save handler reads them from the POST and saves.
    using SetupFieldsProvider = std::function<void(String& html)>;
    using SetupSaveHandler    = std::function<void(WebServer& server)>;

    // Enable /setup, backed by the given settings store. Call before begin().
    // Enabling /setup also registers POST /reboot and puts a "Reboot device"
    // button on the page, so a unit can be restarted from a browser instead of
    // in person — which matters once there are more devices than you want to
    // walk to. The route is POST-only so a stray GET, a link prefetch, or a
    // crawler can never restart a device.
    void enableSetup(DeviceSettings* settingsStore);
    void setSetupFieldsProvider(SetupFieldsProvider p) { _setupFields = p; }
    void setSetupSaveHandler(SetupSaveHandler h)       { _setupSave   = h; }

    // ---- HTML escaping (public: a project rendering injected /setup fields
    // needs the same escaping this module applies to its own) ---------------
    //
    // escapeText() is for text placed between tags. escapeAttr() is for values
    // placed inside a quoted attribute and additionally escapes both quote
    // characters — necessary because operator-entered names routinely contain
    // an apostrophe ("John's Office"), which would otherwise close a
    // single-quoted attribute early and corrupt the rest of the form.
    static String escapeText(const String& s);
    static String escapeAttr(const String& s);


    // Append a line to the remote log (viewable at /webserial AND over telnet).
    // Also echoes to Serial. Safe to call from any task.
    void log(const String& line);

    // Register built-in routes (/status, /update, /webserial), start the web
    // server and the telnet server (port 23), and spawn the driver task. Call
    // once, in STA mode only.
    void begin();

private:
    WebServer      _server;
    String         _hostname;
    StatusProvider _statusProvider = nullptr;

    // --- footer link registry (seeded in begin(), extended per project) ---
    struct FooterLink {
        String label;
        String href;
        bool   external;
    };
    std::vector<FooterLink> _footerLinks;
    bool _footerSeeded = false;     // guards one-time seeding of built-ins

    // Footer theme colors (defaults match the dark dashboards' green accent).
    String _footerLinkColor = "#5fd3a0";
    String _footerHoverColor = "#5fd3a0";
    String _footerSepColor   = "#2a3645";

    // --- reusable /setup page ---
    DeviceSettings*     _settings     = nullptr;   // not owned
    SetupFieldsProvider _setupFields  = nullptr;
    SetupSaveHandler    _setupSave    = nullptr;
    bool                _setupEnabled = false;
    void handleSetupGet();    // GET  /setup -> composed form
    void handleSetupPost();   // POST /setup -> save reusable + project fields
    void handleRebootPost();  // POST /reboot -> schedule a restart

    // Deferred reboot. handleRebootPost() only sets a deadline; driverTask
    // performs the restart once it passes. Restarting inside the handler would
    // cut the connection before the confirmation page reached the browser,
    // leaving the operator unsure whether the request landed.
    volatile unsigned long _rebootAt = 0;   // 0 = no reboot pending

    void   seedFooterLinks();       // add built-in pages once
    void   handleFooterJson();      // GET /footer.json  -> link list
    void   handleFooterScript();    // GET /footer.js    -> injector script

    // --- remote log ring buffer (shared by /webserial and telnet) ---
    static const int  LOG_LINES = 40;
    String            _log[LOG_LINES];
    int               _logHead  = 0;     // guarded by _logMutex
    int               _logCount = 0;     // guarded by _logMutex
    SemaphoreHandle_t _logMutex = nullptr;

    // --- telnet (port 23): one client at a time, fed from the same buffer ---
    WiFiServer  _telnet{23};
    WiFiClient  _telnetClient;
    bool        _telnetGreeted = false;

    void   handleStatus();
    void   handleStatusText();   // /status.txt -- content only, for polling
    String statusBody();         // builds the status text (no HTML shell)
    void   handleWebSerial();
    void   handleLogText();      // /webserial.txt -- content only, for polling
    String renderLog();
    void   serviceTelnet();         // accept client, push new log lines

    static void driverTask(void* pv);
};
