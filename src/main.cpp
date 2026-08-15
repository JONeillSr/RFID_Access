/**
 * @file    main.cpp
 * @brief   RFID Access Control — Seeed Studio XIAO ESP32-C6
 *
 * Architecture: FreeRTOS tasks + an event queue.
 *   readerTask  polls the Paxton reader driver and posts EVT_CARD_TAP events
 *               to appEventQueue.
 *   accessTask  consumes events, applies allow-list logic, drives buzzer/relay,
 *               reader LEDs, and the OLED result screen.
 *   webTask     drives the management WebServer + ElegantOTA (STA mode only).
 *   loop()      drives WiFiManager reconnect logic.
 *
 * Display: built on the reusable Display + SplashScreen libraries. A single
 * shared Wire bus (GPIO 22/23) carries the OLED. On the ESP32-C6 the second
 * hardware I2C peripheral (Wire1) is buggy (espressif/arduino-esp32 #10685) and
 * even probing the bus aggressively can wedge it, so this code brings the bus up
 * once, cleanly, and never scans — that scan-on-boot was what previously left
 * the panel dark.
 *
 * First boot (no stored WiFi credentials): WiFiManager starts an open AP named
 * "RFID-Setup"; connect and open http://192.168.4.1 to enter credentials. The
 * device saves them to NVS and reboots into STA mode.
 *
 * Reader: Paxton P-series proximity (e.g. P50 / 345-110-US), 12 V powered,
 * Clock & Data output (Paxton native; Wiegand also supported by the driver).
 * Wiring (XIAO -> Paxton, matching the Net2's reader-port terminal names):
 *   D8->Data/D0  D9->Clock/D1  (open-collector; 10K pull-ups to 3V3)
 *   D3->Red LED  D10->Green LED  D0->Amber LED  (active-low, open-drain)
 *   Reader 12V/0V from the 12 V supply; grounds commoned with the ESP32.
 * The reader may sit up to 100 m from the ESP32 (Cat5/Belden 8723 class
 * cable; double the 12V/0V cores past 25 m per Paxton's guidance).
 *   OLED: SDA->GPIO22  SCL->GPIO23  addr 0x3C
 *
 * Relay: a standard SONGLE SRD-05VDC-SL-C module (onboard driver transistor +
 * flyback diode) is driven DIRECTLY from D1/GPIO1 -> module IN. No external
 * transistor or base resistor is needed; the module has its own driver. The
 * board fires on a HIGH at IN as wired here. An external 10K pull-down from
 * GPIO1 to GND holds the line LOW (relay de-energised = door locked) during the
 * boot/reset window before setup() configures the pin, preventing a spurious
 * unlock pulse on power-up. The lock is wired fail-secure on the relay's NO
 * contact: 12V reaches the lock only during the grant pulse; it is unpowered
 * (locked) at rest and through reboots.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include "PaxtonReader.h"
#include "UnlockSchedule.h"
#include "WebService.h"
#include <Wire.h>
#include "Pins.h"            // board-guarded peripheral pin map (+ BoardConfig.h)
#include "Display.h"
#include "SplashScreen.h"
#include "WiFiManager.h"
#include "Events.h"
#include "AccessControl.h"
#include "EventLog.h"
#include "WebHandlers.h"
#include "DeviceSettings.h"
#include "DeviceIdentity.h"
#include "JTLogoBitmap.h"

// -- Timing configuration (board-independent) ---------------------------------
// Pin assignments live in Pins.h; only timing/behaviour constants are here.
#define RELAY_HOLD_MS  3000       // how long the relay stays energised (ms)
#define RESULT_HOLD_MS 4000       // how long a GRANTED/DENIED screen stays up (ms)

// Short tag leading this project's device IDs, e.g. "rfid-a1b2c3". The rest of
// the ID comes from the board's MAC, so every unit is unique with no per-door
// build. There is deliberately no hostname #define here: a fixed name would
// make every door answer to the same mDNS label and collide on the network.
#define DEVICE_ID_PREFIX "rfid"

// Local timezone for the unlock schedule (POSIX TZ format; handles DST
// automatically). Default is US Eastern — adjust for your locale.
#define TZ_INFO        "EST5EDT,M3.2.0,M11.1.0"

// -- Globals ------------------------------------------------------------------
// Clock & Data is the P-series' native output on Net2 wiring; pass
// PaxtonReader::WIEGAND instead if the reader has been switched to Wiegand
// (Paxton config card) or a third-party Wiegand reader is fitted.
PaxtonReader  paxton(PIN_PAXTON_DATA, PIN_PAXTON_CLOCK,
                     PIN_PAXTON_LED_R, PIN_PAXTON_LED_G, PIN_PAXTON_LED_A,
                     PaxtonReader::CLOCK_AND_DATA);
WebService     webService(80);
WiFiManager    wifiMgr("RFID-Setup");  // AP SSID shown during first-boot provisioning
DeviceSettings settings;               // NVS store: splash hold, hostname, door labels
DeviceIdentity identity;               // MAC-derived device ID + operator labels
QueueHandle_t  appEventQueue;

// mDNS label resolved once in setup(): the operator's name from /setup if one
// was saved, else the unique MAC-derived device ID. Cached because the OLED
// idle screen redraws it and each read would otherwise hit NVS.
static String gHostname;

// Reusable display helper + a mutex so accessTask and the WiFi callbacks can't
// draw to the panel at the same time.
static Display           gDisplay(OLED_W, OLED_H, -1);
static SemaphoreHandle_t oledMutex;
static bool              gOledOk = false;

// Non-blocking relay release: accessTask sets a deadline, loop() clears the pin
// when it passes, so card processing is never stalled for the full hold time.
static volatile unsigned long relayOffAt = 0;

// Non-blocking result-screen restore: when a GRANTED/DENIED screen is shown,
// accessTask records when it should revert to the idle screen.
static volatile unsigned long resultUntil = 0;

// True while the unlock schedule is holding the door open. Set only from
// loop(); read by the OLED idle screen and the /status provider.
static volatile bool gSchedActive = false;

// LittleFS mount result. The roster and the event spool live here, so a failed
// mount is a real degradation worth surfacing rather than hiding: the door
// keeps working off whatever is already in RAM, but nothing new persists.
static bool gFsOk = false;

// -----------------------------------------------------------------------------
//  OLED screens (built on the Display library primitives, mutex-guarded)
// -----------------------------------------------------------------------------

static void oledConnecting() {
    if (!gOledOk) return;
    xSemaphoreTake(oledMutex, portMAX_DELAY);
    gDisplay.showMessage("RFID Access", "Connecting WiFi...");
    xSemaphoreGive(oledMutex);
}

// Idle screen shown before the first tap: friendly mDNS name, IP, and the
// OTA hint. The hostname lets people reach the unit as <name>.local without
// needing to know the IP.
static void oledShowIP(const String& ip) {
    if (!gOledOk) return;
    Adafruit_SSD1306& d = gDisplay.raw();
    xSemaphoreTake(oledMutex, portMAX_DELAY);
    gDisplay.clear();
    // Title in the yellow band.
    d.setTextColor(SSD1306_WHITE);
    d.setTextSize(1);
    d.setCursor(0, 4);
    d.println("RFID Access");
    // Three info lines in the blue body: hostname, IP, OTA hint.
    d.setCursor(0, 22);
    d.print(gHostname);
    d.println(".local");
    d.setCursor(0, 36);
    d.println(ip);
    d.setCursor(0, 50);
    d.println(gSchedActive ? "** DOOR UNLOCKED **" : "/update  <- OTA");
    gDisplay.display();
    xSemaphoreGive(oledMutex);
}

static void oledShowProvisioning() {
    if (!gOledOk) return;
    xSemaphoreTake(oledMutex, portMAX_DELAY);
    gDisplay.showMessage2("WiFi Setup", "Join AP: RFID-Setup", "-> 192.168.4.1");
    xSemaphoreGive(oledMutex);
}

static void oledShowResult(bool granted, const String& uid, const String& name) {
    if (!gOledOk) return;
    Adafruit_SSD1306& d = gDisplay.raw();
    xSemaphoreTake(oledMutex, portMAX_DELAY);
    gDisplay.clear();
    // Big verdict in the yellow band.
    d.setTextColor(SSD1306_WHITE);
    d.setTextSize(2);
    d.setCursor(0, 0);
    d.println(granted ? "GRANTED" : "DENIED");
    // UID + name in the blue body.
    d.setTextSize(1);
    d.setCursor(0, 22);
    d.println(uid);
    d.setCursor(0, 36);
    d.println((granted && name.length()) ? name : String("Unknown card"));
    gDisplay.display();
    xSemaphoreGive(oledMutex);
    // Schedule a return to the idle screen (handled non-blocking in loop()).
    resultUntil = millis() + RESULT_HOLD_MS;
}

/// Restore the idle screen appropriate to the current WiFi state.
static void oledShowIdle() {
    if (wifiMgr.isProvisioning()) oledShowProvisioning();
    else                          oledShowIP(wifiMgr.localIP().toString());
}

// -----------------------------------------------------------------------------
//  RGB status LED (at-a-glance state; common-cathode = active-HIGH by default)
//
//  Colour scheme:  white = standby/idle   red = denied   blue = granted
//
//  On boards where a channel couldn't be assigned a pin (e.g. the XIAO C6 has
//  no third free GPIO, so PIN_LED_B == -1), the missing channel is skipped.
//  With blue unavailable, "granted" falls back to green so it still reads as a
//  distinct positive colour rather than going dark.
// -----------------------------------------------------------------------------

/// Drive one channel honouring polarity. Safe to call with pin == -1 (no-op).
static inline void ledChannel(int pin, bool on) {
    if (pin < 0) return;
#if RGB_ACTIVE_LOW
    digitalWrite(pin, on ? LOW : HIGH);
#else
    digitalWrite(pin, on ? HIGH : LOW);
#endif
}

/// Set the LED to an arbitrary R/G/B on-off combination.
static void ledSet(bool r, bool g, bool b) {
    ledChannel(PIN_LED_R, r);
    ledChannel(PIN_LED_G, g);
    ledChannel(PIN_LED_B, b);
}

static void ledOff()     { ledSet(false, false, false); }  // dark (idle)
static void ledDenied()  { ledSet(true,  false, false); }   // red
static void ledGranted() {
#if HAS_RGB_FULL
    ledSet(false, false, true );                            // blue
#else
    ledSet(false, true,  false);                            // green fallback (no blue pin)
#endif
}

static void ledInit() {
    if (PIN_LED_R >= 0) pinMode(PIN_LED_R, OUTPUT);
    if (PIN_LED_G >= 0) pinMode(PIN_LED_G, OUTPUT);
    if (PIN_LED_B >= 0) pinMode(PIN_LED_B, OUTPUT);
    ledOff();
}

// -----------------------------------------------------------------------------
//  Reader
// -----------------------------------------------------------------------------

/// Polls the Paxton driver and posts CARD_TAP events; no business logic here.
/// The driver's ISRs capture the bits; poll() hands over each completed,
/// validated read. The credential (card number) travels in evt.card.uid — the
/// field name is historical from the MFRC522 era; it is simply the string the
/// allow-list matches on.
void readerTask(void* pv) {
    String        lastCred;
    unsigned long lastCredMs = 0;
    uint32_t      lastErrCount = 0;
    PaxtonReader::Credential cred;
    for (;;) {
        // Surface undecodable frames in the remote log with their raw bits —
        // turns polarity/edge/format faults from guesswork into reading.
        if (paxton.errorCount() != lastErrCount) {
            lastErrCount = paxton.errorCount();
            webService.log(String("[paxton] undecodable frame: ") +
                           paxton.lastFrame());
        }
        if (paxton.poll(cred)) {
            String number(cred.number);
            // Debounce: a token held in the field re-reads; ignore repeats
            // of the same credential within 1.5 s.
            if (!(number == lastCred && (millis() - lastCredMs) < 1500)) {
                lastCred   = number;
                lastCredMs = millis();
                AppEvent evt;
                evt.type = EVT_CARD_TAP;
                number.toCharArray(evt.card.uid, sizeof(evt.card.uid));
                strncpy(evt.card.cardType, cred.format,
                        sizeof(evt.card.cardType) - 1);
                evt.card.cardType[sizeof(evt.card.cardType) - 1] = '\0';
                xQueueSend(appEventQueue, &evt, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// -- Buzzer feedback (short, blocking beeps are fine inside accessTask) --------
static void buzzerGranted() {
    digitalWrite(PIN_BUZZER, HIGH); vTaskDelay(pdMS_TO_TICKS(80));
    digitalWrite(PIN_BUZZER, LOW);  vTaskDelay(pdMS_TO_TICKS(80));
    digitalWrite(PIN_BUZZER, HIGH); vTaskDelay(pdMS_TO_TICKS(80));
    digitalWrite(PIN_BUZZER, LOW);
}

static void buzzerDenied() {
    digitalWrite(PIN_BUZZER, HIGH); vTaskDelay(pdMS_TO_TICKS(600));
    digitalWrite(PIN_BUZZER, LOW);
}

/// Drive the relay module IN HIGH (energise) and schedule a non-blocking
/// release. loop() drives it LOW again once relayOffAt passes, so accessTask
/// returns immediately to process cards. The module fires on HIGH as wired.
static void relayGrantedNonBlocking() {
    digitalWrite(PIN_RELAY, HIGH);
    relayOffAt = millis() + RELAY_HOLD_MS;
}

/// Receives events from readerTask / the exit button and applies access logic.
void accessTask(void* pv) {
    AppEvent evt;
    for (;;) {
        if (xQueueReceive(appEventQueue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (evt.type == EVT_EXIT_REQUEST) {
                // Request-to-exit is always honoured — the button lives on
                // the secure side of the door, so no allow-list check.
                paxton.ledGranted(); ledGranted();
                digitalWrite(PIN_BUZZER, HIGH); vTaskDelay(pdMS_TO_TICKS(80));
                digitalWrite(PIN_BUZZER, LOW);
                relayGrantedNonBlocking();
                webService.log("[access] EXIT button - door released");
                eventLog.append(EventLog::EVT_EXIT, EventLog::R_EXIT_BUTTON, true, "");
                oledShowResult(true, "Exit button", "Door released");
                continue;
            }

            bool granted = acProcessEvent(evt);
            // Verdict shows both at the door (reader LED) and on the panel LED.
            if (granted) { paxton.ledGranted(); buzzerGranted(); relayGrantedNonBlocking(); ledGranted(); }
            else         { paxton.ledDenied();  buzzerDenied();                             ledDenied();  }
            webService.log(String("[access] ") + (granted ? "GRANTED " : "DENIED  ") + evt.card.uid);

            String uid = String(evt.card.uid);
            String name;
            acNameFor(uid, name);      // leaves name empty if not enrolled
            oledShowResult(granted, uid, name);
        }
    }
}

// -----------------------------------------------------------------------------
//  Setup / loop
// -----------------------------------------------------------------------------

void setup() {
    // Relay first, before anything else runs: drive IN LOW immediately so the
    // relay is de-energised (door locked) the moment our code takes over. The
    // external 10K pull-down covers the even-earlier window before this line,
    // from power-on until the pin is configured. Together they guarantee no
    // spurious unlock pulse on boot or reset for this fail-secure lock.
    pinMode(PIN_RELAY, OUTPUT);
    digitalWrite(PIN_RELAY, LOW);     // relay de-energised (locked)

    // Buzzer to a known state + boot beep to confirm wiring.
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, HIGH); delay(120);   // boot beep confirms buzzer wiring
    digitalWrite(PIN_BUZZER, LOW);

    // RGB status LED to white (standby) as soon as pins are safe.
    ledInit();

    // -- Settings + identity, before anything that needs a name or a stored
    // timing value. Deliberately after the relay/buzzer/LED safing above: the
    // lock must reach its secure state before any other subsystem starts.
    settings.begin();
    identity.begin(&settings, DEVICE_ID_PREFIX);
    gHostname = identity.hostname();

    // Identity banner. Logged before anything else so it is the first thing on
    // the wire: with several doors on one network, "which unit am I talking to?"
    // is the question every other diagnostic depends on. webService.log() echoes
    // to Serial immediately and is buffered for /webserial once the server is up.
    // On boards with a reboot-surviving USB-UART (the classic DevKit) this is
    // visible in the serial monitor without a browser or even a WiFi connection.
    webService.log(String("[id] device=") + identity.deviceId() +
                   "  host=" + gHostname + ".local" +
                   "  board=" BOARD_NAME "  fw=" FW_VERSION);
    {
        String door = identity.doorName();
        String site = identity.siteName();
        webService.log(String("[id] door=") + (door.length() ? door : "(unnamed)") +
                       "  site=" + (site.length() ? site : "(unset)"));
    }

    // -- Filesystem: home of the credential roster and the event spool. -------
    // Mounted before acInit() because the roster loads from here. formatOnFail
    // is deliberate: a brand-new partition (or a unit moving off the old table)
    // arrives unformatted, and a door that refuses to start because its
    // filesystem was never initialised would be a poor failure mode. The cost is
    // that an unmountable-but-recoverable filesystem is reformatted rather than
    // salvaged, which is acceptable while the cloud holds the master record.
    gFsOk = LittleFS.begin(/*formatOnFail=*/true);
    if (gFsOk) {
        webService.log(String("[fs] LittleFS mounted: ") +
                       (LittleFS.usedBytes() / 1024) + " KB used of " +
                       (LittleFS.totalBytes() / 1024) + " KB");
    } else {
        // Not fatal: access control still runs. But nothing persists, so say so
        // loudly rather than letting it look healthy.
        webService.log("[fs] LittleFS MOUNT FAILED - roster and event spool "
                       "will not persist");
    }

    // -- OLED: single shared Wire bus, brought up once, no scan. --------------
    oledMutex = xSemaphoreCreateMutex();
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    Wire.setClock(100000);            // gentle, reliable
    delay(100);                       // let the panel's internal supply settle
                                      // before begin() pushes the init sequence
    // Display::begin() returns the real I2C ACK result, unlike Adafruit's
    // begin() which reports success even with nothing on the bus. The library
    // passes periphBegin=false so this Wire.begin(22,23) binding is preserved.
    gOledOk = gDisplay.begin(Wire, OLED_ADDR);

    if (gOledOk) {
        SplashScreen splash(gDisplay.raw(), JT_LOGO, JT_LOGO_W, JT_LOGO_H,
                            JT_LOGO_REST_Y);
        // Hold is configurable on /setup; 2 s preserves the previous fixed value
        // for units that have never saved a setting.
        splash.play(settings.splashHoldSec(2) * 1000UL);
        oledConnecting();
    }

    // -- Reader --
    // The Paxton transmits spontaneously; begin() just configures the pins and
    // attaches the capture ISRs. Amber = "ready, present token" at the door.
    paxton.begin();
    paxton.ledIdle();
    webService.log("[paxton] reader interface up (Clock&Data mode)");

    // -- Access-control state (allow-list + unlock schedule from NVS) --
    acInit();

    // Durable event spool. After acInit() so a failure here cannot stop the
    // roster loading -- the door must decide correctly even if it cannot record.
    if (!eventLog.begin()) {
        webService.log("[evt] spool unavailable - events will NOT be recorded");
    } else {
        webService.log(String("[evt] spool ready: boot #") + eventLog.bootId() +
                       ", " + eventLog.pending() + " event(s) awaiting upload");
        eventLog.append(EventLog::EVT_BOOT, EventLog::R_NONE, false, "");
    }
    unlockSchedule.begin();
    appEventQueue = xQueueCreate(10, sizeof(AppEvent));

    // -- Exit button (request-to-exit): push-to-make to GND, active low --
#if PIN_EXIT_BTN >= 0
    pinMode(PIN_EXIT_BTN, INPUT_PULLUP);
#endif

    // -- WiFi callbacks reflect state on the OLED --
    wifiMgr.onConnected([]() {
        oledShowIP(WiFi.localIP().toString());
        webService.log("[wifi] connected: " + WiFi.localIP().toString());
    });
    wifiMgr.onProvisioningStarted([]() {
        oledShowProvisioning();
    });
    wifiMgr.setHostname(gHostname.c_str());  // device reachable as <name>.local
    wifiMgr.setTimeSync(TZ_INFO);            // NTP on connect; feeds the unlock
                                             // schedule (locked until first sync)
    wifiMgr.begin();   // blocking: STA on stored credentials, else provisioning AP

    // Reader + access control are independent of WiFi; always run them.
    xTaskCreate(readerTask, "reader", 4096, NULL, 2, NULL);
    xTaskCreate(accessTask, "access", 4096, NULL, 2, NULL);

    // Web + OTA only in STA mode; provisioning mode owns port 80.
    if (!wifiMgr.isProvisioning()) {
        // Project-specific routes register on the module's server.
        registerWebHandlers(webService.routes());

        // Reusable /setup page: WebService renders the common block (splash
        // hold, board, mDNS hostname) and this project appends the location
        // labels that identify the door in central reports.
        webService.enableSetup(&settings);
        webService.setSetupFieldsProvider([](String& html) {
            html += "<label>Door name</label>";
            html += "<input type='text' name='doorName' value='" +
                    WebService::escapeAttr(identity.doorName()) + "' placeholder='e.g. Front Door'>";
            html += "<div class='hint'>Human label for this door, shown in access reports.</div>";

            html += "<label>Site name</label>";
            html += "<input type='text' name='siteName' value='" +
                    WebService::escapeAttr(identity.siteName()) + "' placeholder='e.g. Main Shop'>";
            html += "<div class='hint'>Groups doors that share a building or location.</div>";

            html += "<label>Device ID</label>";
            html += "<input type='text' value='" + WebService::escapeAttr(identity.deviceId()) + "' readonly>";
            html += "<div class='hint'>Fixed identity derived from this board's MAC. Used as the "
                    "mDNS name until a hostname is set above, and as the key a backend "
                    "identifies this door by. Cannot be changed.</div>";

            html += "<div class='hint' style='margin-top:16px;color:#9fb3c8'>"
                    "Door and site names apply immediately. A changed mDNS hostname only "
                    "takes effect after a reboot, since the responder is started at boot."
                    "</div>";
        });
        webService.setSetupSaveHandler([](WebServer& s) {
            if (s.hasArg("doorName")) identity.setDoorName(s.arg("doorName"));
            if (s.hasArg("siteName")) identity.setSiteName(s.arg("siteName"));
        });

        // Show the mDNS label on /status, and inject app-specific status lines.
        webService.setHostname(wifiMgr.getHostname());
        webService.setStatusProvider([](String& body) {
            // Identity first: with several doors on one network, the first
            // question a status page has to answer is "which unit is this?".
            String door = identity.doorName();
            String site = identity.siteName();
            body += "Device:   " + identity.deviceId() + "\n";
            body += "Door:     " + (door.length() ? door : String("(unnamed)")) + "\n";
            body += "Site:     " + (site.length() ? site : String("(unset)"))   + "\n";
            // Board matters operationally: doors may be built on different
            // ESP32 variants, so a firmware image is only valid for one of
            // them. Anything pushing updates has to match this.
            body += "Board:    " BOARD_NAME "\n";
            body += "Firmware: " FW_VERSION "\n";
            if (gFsOk) {
                body += "Filesys:  " + String(LittleFS.usedBytes() / 1024) +
                        " KB used / " + String(LittleFS.totalBytes() / 1024) +
                        " KB (LittleFS)\n";
            } else {
                body += "Filesys:  MOUNT FAILED - nothing persists\n";
            }
            body += "OLED:     " + String(gOledOk ? "OK (0x3C)" : "NOT FOUND") + "\n";
            // Taps that raise this count but never decode usually mean the
            // wrong line format (Clock&Data vs Wiegand) or a swapped pair.
            body += "Reader:   Paxton Clock&Data, " + String(paxton.edgeCount())
                  + " edge(s), " + String(paxton.errorCount())
                  + " error(s), " + String(paxton.repairCount())
                  + " repaired\n";
            if (UnlockSchedule::timeValid()) {
                time_t    now = time(nullptr);
                struct tm tm;
                localtime_r(&now, &tm);
                char tbuf[40];
                strftime(tbuf, sizeof(tbuf), "%a %Y-%m-%d %I:%M %p", &tm);
                body += "Time:     " + String(tbuf) + " (NTP synced)\n";
            } else {
                body += "Time:     not synced yet\n";
            }
            if (unlockSchedule.enabled()) {
                char sched[72];
                snprintf(sched, sizeof(sched),
                         "Schedule: %02u:%02u-%02u:%02u  %s\n",
                         unlockSchedule.startMin() / 60, unlockSchedule.startMin() % 60,
                         unlockSchedule.endMin() / 60,   unlockSchedule.endMin() % 60,
                         gSchedActive ? "UNLOCKED NOW"
                         : (UnlockSchedule::timeValid() ? "locked" : "inactive (no time sync)"));
                body += sched;
            } else {
                body += "Schedule: disabled\n";
            }
            body += "Enrolled: " + String(acCount()) + " card(s)\n";
            // Roster file size is the honest persistence check: a populated
            // in-RAM list with no file on disk means every reboot re-migrates
            // from NVS, silently undoing any removal.
            size_t rb = acRosterFileBytes();
            if (!acPersistent()) {
                body += "Roster:   NOT PERSISTING - filesystem unavailable\n";
            } else if (rb == 0) {
                body += "Roster:   IN RAM ONLY - /roster.dat missing, edits will be lost\n";
            } else {
                body += "Roster:   " + String(rb) + " B on disk (saved OK)\n";
            }

            // Event spool. `pending` is the count the backend has not yet
            // confirmed; nothing acks until Phase 3's sync client exists, so it
            // only grows for now — and it MUST survive a reboot, which is the
            // whole difference between this and the RAM-only tapLog below.
            body += "Events:   " + String(eventLog.pending()) + " pending, " +
                    String(eventLog.bytesOnDisk()) + " B on disk, boot #" +
                    String(eventLog.bootId());
            if (eventLog.overflowed()) body += "  [OVERFLOWED - oldest discarded]";
            body += "\n";

            LOCK();
            if (logCount > 0) {
                int last = (logHead - 1 + LOG_SIZE) % LOG_SIZE;
                TapRecord& t = tapLog[last];
                body += "Last tap: " + t.uid + " — " + (t.granted ? "GRANTED" : "DENIED") + "\n";
                String nm;
                if (acNameFor(t.uid, nm)) body += "Name:     " + nm + "\n";
            } else {
                body += "Last tap: none\n";
            }
            UNLOCK();
        });

        // Project entries for the shared page footer (built-ins are seeded
        // by the module itself: status, log, firmware update).
        // "Setup" is not listed here: WebService adds it itself when /setup is
        // enabled, and addFooterLink() does not de-duplicate.
        webService.addFooterLink("Dashboard", "/");
        webService.addFooterLink("Manage fobs", "/config");

        webService.begin();   // serves /status /update /webserial + project routes
    }
}

void loop() {
    wifiMgr.loop();

    // Exit button (request-to-exit). Debounce/rate-limit: one release per
    // 1.5 s while held — matches how a Net2 treats a held exit button.
#if PIN_EXIT_BTN >= 0
    {
        static unsigned long lastExitMs = 0;
        if (digitalRead(PIN_EXIT_BTN) == LOW &&
            (millis() - lastExitMs) > 1500) {
            lastExitMs = millis();
            AppEvent evt = {};
            evt.type = EVT_EXIT_REQUEST;
            xQueueSend(appEventQueue, &evt, 0);
        }
    }
#endif

    // Scheduled-unlock window. isActiveNow() fails secure without NTP time,
    // so a reboot with no network leaves the door locked.
    bool schedNow = unlockSchedule.isActiveNow();
    if (schedNow != gSchedActive) {
        gSchedActive = schedNow;
        if (schedNow) {
            digitalWrite(PIN_RELAY, HIGH);          // hold the door open
            ledGranted(); paxton.ledGranted();
            webService.log("[sched] unlock window started - door held open");
            eventLog.append(EventLog::EVT_SCHED_ON, EventLog::R_SCHEDULE, true, "");
        } else {
            relayOffAt = 0;
            digitalWrite(PIN_RELAY, LOW);           // window over: lock
            ledOff(); paxton.ledIdle();
            webService.log("[sched] unlock window ended - door locked");
            eventLog.append(EventLog::EVT_SCHED_OFF, EventLog::R_SCHEDULE, false, "");
        }
        oledShowIdle();
    }

    // Non-blocking relay release. The green "granted" LED tracks the relay
    // unlock window: it lights while the door is energised and goes dark the
    // instant the relay releases, so green always means "unlocked right now".
    // While the schedule holds the door open, tap/exit pulses simply expire
    // without touching the relay or the green LEDs.
    if (relayOffAt != 0 && (long)(millis() - relayOffAt) >= 0) {
        relayOffAt = 0;
        if (!gSchedActive) {
            digitalWrite(PIN_RELAY, LOW);
            ledOff();              // granted window over → LED dark
            paxton.ledIdle();      // reader back to amber "ready"
        }
    }

    // Non-blocking result-screen restore: once the GRANTED/DENIED screen has
    // held long enough, revert to the idle screen (IP or provisioning hint).
    // Also clears the red "denied" LED — a denial never fires the relay, so its
    // LED is cleared here on the result timer rather than by the relay block.
    if (resultUntil != 0 && (long)(millis() - resultUntil) >= 0) {
        resultUntil = 0;
        oledShowIdle();
        if (gSchedActive) {
            ledGranted(); paxton.ledGranted();  // still in the unlock window
        } else {
            ledOff();              // denied window over → LED dark
            paxton.ledIdle();      // reader back to amber "ready"
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
}
