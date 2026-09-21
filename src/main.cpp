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
 * device saves them to NVS and reboots into STA mode. The same portal comes up
 * when a configured door boots during a network outage; it then retries the
 * saved network in the background and reboots once it can join.
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
#include "DoorContact.h"
#include "CloudSync.h"
#include "WebHandlers.h"
#include "DeviceSettings.h"
#include "DeviceIdentity.h"
#include "JTLogoBitmap.h"

// -- Timing configuration (board-independent) ---------------------------------
// Pin assignments live in Pins.h; only timing/behaviour constants are here.
// Defaults only. Both are per-door settings the admin app pushes down on sync
// (CloudSync::DoorConfig), kept in NVS so an offline boot still uses the value
// the door was last given rather than reverting to these.
#define RELAY_HOLD_MS  3000       // how long the relay stays energised (ms)
#define RESULT_HOLD_MS 4000       // how long a GRANTED/DENIED screen stays up (ms)

// Door contact (see lib/DoorContact). How long after a release ends an opening
// still counts as released: a door pulled right as the strike re-locks may not
// part the magnet from the reed until a moment later, and that is not a forced
// door. Short, because every second of it is a second a forced door is missed.
#define DOOR_RELEASE_GRACE_MS   2000
#define DOOR_HELD_DEFAULT_SEC   60
// A forced door asks for an immediate sync, but no more than this often: a
// chattering or cut contact must not turn into a stream of back-to-back syncs.
#define DOOR_SYNC_MIN_GAP_MS    60000
// Settings keys (NVS keys are <= 15 characters).
static const char* const KEY_DOOR_CONTACT  = "doorContact";   // bool: a contact is fitted
static const char* const KEY_DOOR_HELD_SEC = "doorHeldSec";   // uint: held-open limit, 0 = off
static const char* const KEY_READER_MODE   = "readerMode";    // uint: PaxtonReader::Mode
static const char* const KEY_RELAY_HOLD_MS  = "relayHoldMs";   // uint: ms
static const char* const KEY_RESULT_HOLD_MS = "resultHoldMs";  // uint: ms
static const char* const KEY_SETUP_AP       = "setupAP";       // bool: raise the setup AP at the next boot
// The last value the cloud sent for the setup AP. The flag above is consumed at
// boot, so comparing the cloud's value against IT would re-arm on the next sync
// and bring the open network back after every reboot. This records what the
// cloud last asked for, so the door acts on the request changing, not on it
// still being set.
static const char* const KEY_SETUP_AP_REQ   = "setupApReq";    // bool: last cloud request

// How long the side setup AP stays up before closing itself. It is an OPEN
// network, so it is a way in for as long as it exists; it should outlast someone
// walking to the door and typing, and nothing more.
#define SETUP_AP_TIMEOUT_MS  (30UL * 60UL * 1000UL)

// How long the exit button must be held AT POWER-ON to force the setup portal.
// Long enough that nobody does it by leaning on the button, short enough to hold
// on a ladder.
#define PORTAL_BUTTON_HOLD_MS  3000

// Short tag leading this project's device IDs, e.g. "rfid-a1b2c3". The rest of
// the ID comes from the board's MAC, so every unit is unique with no per-door
// build. There is deliberately no hostname #define here: a fixed name would
// make every door answer to the same mDNS label and collide on the network.
#define DEVICE_ID_PREFIX "rfid"

// Local timezone for the unlock schedule (POSIX TZ format; handles DST
// automatically). Default is US Eastern — adjust for your locale.
#define TZ_INFO        "EST5EDT,M3.2.0,M11.1.0"

// DEFAULT backend, used only until a door is told otherwise on /setup. The
// effective host lives in NVS beside the device key; see CloudSync::begin().
//
// It is deliberately not the authority. One deployment per customer means one
// hostname per customer, so a compile-time host would force a firmware build per
// customer -- multiplying the per-board OTA matrix by the customer count, making
// version numbers ambiguous ("2.6.0" for whom?), and making images silently
// non-interchangeable: a spare flashed from the wrong folder boots perfectly and
// syncs to somebody else's backend.
//
// Whatever host a door ends up on, its certificate chain must still anchor to
// one of the roots in lib/CloudSync/RootCerts.h. That holds for every
// *.azurewebsites.net deployment; a custom domain on the API would need
// re-checking.
#define CLOUD_HOST_DEFAULT "jtc-prod-rfidaccess-eastus2-func.azurewebsites.net"

// When to call NTP unreachable on /status.
//
// DERIVED FROM THE SDK, NOT PICKED. The first version of this used a flat two
// hours on the assumption that ESP-IDF re-polls hourly. It does not: the
// installed framework sets CONFIG_LWIP_SNTP_UPDATE_DELAY to 10800000 ms, so the
// real interval is THREE hours. A two-hour threshold therefore reported
// "unreachable - drifting" on every healthy door for a third of every cycle --
// and a warning that fires on working hardware is worse than no warning at all,
// because it trains people to ignore the one that matters.
//
// Deriving it means the threshold follows the SDK if that default ever changes,
// instead of silently becoming wrong again.
//
// Two intervals plus ten minutes: one missed poll is a WiFi blip and not worth
// shouting about, two consecutive misses is a real signal.
#ifdef CONFIG_LWIP_SNTP_UPDATE_DELAY
  static const uint32_t NTP_POLL_S  = CONFIG_LWIP_SNTP_UPDATE_DELAY / 1000;
#else
  static const uint32_t NTP_POLL_S  = 3600;   // conservative if the SDK is silent
#endif
static const uint32_t NTP_STALE_S = NTP_POLL_S * 2 + 600;

// -- Globals ------------------------------------------------------------------
// Clock & Data is the P-series' native output on Net2 wiring and the default.
// The mode here is only the fallback: setup() replaces it with the format saved
// on /setup ("Reader format"), so a door with a Wiegand reader -- a Paxton
// switched with a config card, or a third-party reader -- needs no custom build.
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

// Timings in force right now: the defaults above until NVS or the admin app
// says otherwise. Written by the sync task, read by accessTask and loop() --
// aligned 32-bit scalars, so a reader either sees the old value or the new one.
static volatile uint32_t gRelayHoldMs  = RELAY_HOLD_MS;
static volatile uint32_t gResultHoldMs = RESULT_HOLD_MS;

// A reader format arrived that differs from the one attached at boot. The format
// decides which ISRs are attached, so it cannot be applied to a running reader;
// /setup and /status say so until someone reboots the door.
static volatile bool gReaderRebootPending = false;

// Non-blocking result-screen restore: when a GRANTED/DENIED screen is shown,
// accessTask records when it should revert to the idle screen.
static volatile unsigned long resultUntil = 0;

// True while the unlock schedule is holding the door open. Set only from
// loop(); read by the OLED idle screen and the /status provider.
static volatile bool gSchedActive = false;

// When the side setup AP should close itself, or 0 if it is not up. Nothing to
// do with the door contact below: the AP exists on every board.
static unsigned long gSetupApCloseAt = 0;

#if PIN_DOOR_CONTACT >= 0

// Door contact. The state machine is driven only from loop(); the web task only
// reads its scalars for /status, and asks for a reload through gDoorReload rather
// than touching it, since begin() racing update() would corrupt its state.
static DoorContact   gDoor;
static volatile bool gDoorContactOn = false;
static volatile bool gDoorReload    = false;
#endif

// Until when an opening counts as released: set by every grant and exit press,
// and by the end of an unlock window. gReleaseArmed stays false until the first
// release, so a meaningless initial value can never excuse an opening.
static volatile unsigned long gReleaseUntil = 0;
static volatile bool          gReleaseArmed = false;

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
    resultUntil = millis() + gResultHoldMs;
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
    relayOffAt = millis() + gRelayHoldMs;
    gReleaseUntil = millis() + gRelayHoldMs + DOOR_RELEASE_GRACE_MS;
    gReleaseArmed = true;
}

// -----------------------------------------------------------------------------
//  Door contact
// -----------------------------------------------------------------------------

#if PIN_DOOR_CONTACT >= 0
static bool doorContactReadsOpen() {
    return digitalRead(PIN_DOOR_CONTACT) == DOOR_CONTACT_OPEN_LEVEL;
}

/// (Re)load the contact settings and restart the state machine from the pin's
/// current reading. loop() only.
static void doorContactLoad() {
    gDoorContactOn = settings.getBool(KEY_DOOR_CONTACT, false);
    uint32_t heldSec = settings.getUInt(KEY_DOOR_HELD_SEC, DOOR_HELD_DEFAULT_SEC);
    if (gDoorContactOn) {
        gDoor.begin(millis(), doorContactReadsOpen(), heldSec * 1000UL);
        webService.log(String("[door] contact enabled on GPIO ") + PIN_DOOR_CONTACT +
                       ", door " + (gDoor.isOpen() ? "OPEN" : "closed") +
                       ", held-open limit " +
                       (heldSec ? String(heldSec) + "s" : String("off")));
    } else {
        webService.log("[door] contact disabled");
    }
}

/// One tick of the contact. loop() only.
static void doorContactPoll() {
    DoorContact::Inputs in;
    in.rawOpen      = doorContactReadsOpen();
    in.released     = relayOffAt != 0 ||
                      (gReleaseArmed && (long)(millis() - gReleaseUntil) < 0);
    in.scheduleOpen = gSchedActive;

    switch (gDoor.update(millis(), in)) {
    case DoorContact::FORCED: {
        webService.log("[door] FORCED OPEN - no grant, exit press or unlock window");
        eventLog.append(EventLog::EVT_DOOR_FORCED, EventLog::R_NO_RELEASE, false, "");
        // The one event worth not waiting 30 s -- or a whole backoff -- to report.
        static unsigned long lastSyncAsk = 0;
        static bool          asked       = false;
        if (!asked || millis() - lastSyncAsk >= DOOR_SYNC_MIN_GAP_MS) {
            asked       = true;
            lastSyncAsk = millis();
            cloudSync.requestSyncNow();
        }
        break;
    }
    case DoorContact::HELD:
        webService.log(String("[door] HELD OPEN past ") +
                       String(gDoor.heldLimitMs() / 1000) + "s");
        eventLog.append(EventLog::EVT_DOOR_HELD, EventLog::R_HELD_OPEN, false, "");
        break;
    case DoorContact::HELD_CLOSED: {
        // Sized for any value so the compiler cannot see a truncation; the spool
        // keeps 15 characters, and "4294967s" (the most a uint32 ms can say) is 8.
        char open[24];
        snprintf(open, sizeof(open), "%lus", (unsigned long)(gDoor.lastOpenMs() / 1000));
        webService.log(String("[door] closed after being held open ") + open);
        eventLog.append(EventLog::EVT_DOOR_HELD, EventLog::R_CLOSED, false, open);
        break;
    }
    default:
        break;
    }
}
#endif

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
    // The line format is per door, saved on /setup, and applied here only: it
    // decides which ISRs attach, so it cannot change under a running reader.
    // An out-of-range stored value falls back to Clock & Data.
    paxton.setMode(settings.getUInt(KEY_READER_MODE, PaxtonReader::CLOCK_AND_DATA)
                       == PaxtonReader::WIEGAND
                   ? PaxtonReader::WIEGAND : PaxtonReader::CLOCK_AND_DATA);
    webService.log(String("[reader] format: ") +
                   PaxtonReader::modeName(paxton.mode()));
    paxton.begin();
    paxton.ledIdle();
    // Format is on the line above; repeating it here would only be a second
    // place to forget to update.
    webService.log("[paxton] reader interface up");

    // Timings the admin app owns. Read from NVS here so a door that boots with
    // no network still uses what it was last told, not the compile-time default.
    gRelayHoldMs  = settings.getUInt(KEY_RELAY_HOLD_MS,  RELAY_HOLD_MS);
    gResultHoldMs = settings.getUInt(KEY_RESULT_HOLD_MS, RESULT_HOLD_MS);

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

    // -- Door contact: off unless enabled on /setup. With nothing wired, the
    // pulled-up input reads "open", which would be an instant forced alert. --
#if PIN_DOOR_CONTACT >= 0
    pinMode(PIN_DOOR_CONTACT, INPUT_PULLUP);
    delay(5);                         // let the pull-up settle before the first read
    doorContactLoad();
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
    // -- Physical way back into the portal ---------------------------------
    // Hold the exit button while powering on and the door opens the setup portal
    // instead of joining its saved network. This is the answer to a door left on
    // a network nobody can reach any more: it needs no network, no cloud and no
    // USB. Credentials are not erased, so releasing the button and rebooting
    // returns the door to normal.
#if PIN_EXIT_BTN >= 0
    if (digitalRead(PIN_EXIT_BTN) == LOW) {
        webService.log("[wifi] exit button held at boot - hold for the setup portal");
        unsigned long held = millis();
        while (digitalRead(PIN_EXIT_BTN) == LOW &&
               millis() - held < PORTAL_BUTTON_HOLD_MS) {
            delay(50);
        }
        if (digitalRead(PIN_EXIT_BTN) == LOW) {
            wifiMgr.forcePortal();
            webService.log("[wifi] FORCED SETUP PORTAL - saved network skipped");
            if (gOledOk) {
                xSemaphoreTake(oledMutex, portMAX_DELAY);
                gDisplay.showMessage2("WiFi Setup", "Join AP: RFID-Setup", "-> 192.168.4.1");
                xSemaphoreGive(oledMutex);
            }
        }
    }
#endif

    wifiMgr.begin();   // blocking: STA on stored credentials, else provisioning AP

    // Reader + access control are independent of WiFi; always run them.
    xTaskCreate(readerTask, "reader", 4096, NULL, 2, NULL);
    xTaskCreate(accessTask, "access", 4096, NULL, 2, NULL);

    // The admin app can ask for the setup AP to be raised at the next boot, for
    // a door that has to move to a network it cannot see from the current one.
    // Consumed here, so it fires once and only once: an open AP that came back
    // after every reboot would be a standing way in.
    if (!wifiMgr.isProvisioning() && settings.getBool(KEY_SETUP_AP, false)) {
        settings.setBool(KEY_SETUP_AP, false);
        wifiMgr.openSetupAP();
        gSetupApCloseAt = millis() + SETUP_AP_TIMEOUT_MS;
        webService.log("[wifi] setup AP raised as requested by the admin app");
        eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false, "setupAP=on");
    }

    // Web + OTA only in STA mode; provisioning mode owns port 80.
    if (!wifiMgr.isProvisioning()) {
        // Project-specific routes register on the module's server.
        // Once paired, the cloud owns this door's roster and schedule, so the
        // local write endpoints refuse with 409. Evaluated per request rather
        // than captured once: pairing and unpairing both happen at runtime on
        // /setup, and a value read at boot would be wrong until the next reboot.
        registerWebHandlers(webService.routes(),
                            []() { return cloudSync.paired(); });

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

            // ---- WiFi ----
            // NOT gated on pairing, unlike the roster, schedule and reader
            // format: which network a door sits on is local infrastructure, and
            // the cloud cannot set it -- a wrong value would take the door off
            // the air, which is precisely what the cloud could not then fix.
            {
                html += "<h2>WiFi</h2>";
                html += "<div class='hint'>On <b>" + WebService::escapeText(WiFi.SSID()) + "</b>";
                if (wifiMgr.setupAPOpen()) html += ", setup AP also up";
                html += ".";
                switch (wifiMgr.changeState()) {
                case WiFiManager::CHANGE_TRYING:
                    html += " <span style='color:#e0a458'>Trying " +
                            WebService::escapeText(wifiMgr.changeSsid()) + "&hellip;</span>";
                    break;
                case WiFiManager::CHANGE_REVERTING:
                    html += " <span style='color:#e0a458'>Could not join " +
                            WebService::escapeText(wifiMgr.changeSsid()) +
                            "; going back.</span>";
                    break;
                case WiFiManager::CHANGE_FAILED:
                    html += " <span style='color:#e0556b'>Could not join " +
                            WebService::escapeText(wifiMgr.changeSsid()) +
                            " &mdash; check the name and password. The door stayed "
                            "on its previous network.</span>";
                    break;
                case WiFiManager::CHANGE_DONE:
                    html += " <span style='color:#5fd3a0'>Moved to " +
                            WebService::escapeText(wifiMgr.changeSsid()) + ".</span>";
                    break;
                default: break;
                }
                html += "</div>";
                html += "<label>Move to network</label>";
                html += "<input type='text' name='wifiSsid' value='' autocomplete='off' "
                        "spellcheck='false' placeholder='Leave blank to stay on the current network'>";
                html += "<label>Password</label>";
                html += "<input type='password' name='wifiPass' value='' "
                        "autocomplete='new-password' placeholder='Blank for an open network'>";
                html += "<div class='hint'>The door tries the new network and <b>keeps it only "
                        "once it has an address</b>; if it cannot join, it returns to the one "
                        "that works. You will lose this page while it tries &mdash; reconnect to "
                        "whichever network it ends up on and reload.<br>"
                        "If the new network is not reachable from here, arm the setup AP in the "
                        "admin app, or hold the exit button while powering the door on.</div>";
            }

            // ---- Reader format ----
            {
                bool saved = settings.getUInt(KEY_READER_MODE, PaxtonReader::CLOCK_AND_DATA)
                             == PaxtonReader::WIEGAND;
                // Once paired, the admin app owns this, exactly as it owns the
                // roster and the schedule. Shown, not editable: two places that
                // can set the format would disagree, and the cloud would win at
                // the next sync anyway -- silently, a reboot later.
                bool managed = cloudSync.paired();
                html += "<h2>Reader</h2>";
                html += "<label>Reader format</label>";
                html += String("<select name='readerMode'") + (managed ? " disabled" : "") + ">";
                html += String("<option value='0'") + (saved ? "" : " selected") +
                        ">Clock &amp; Data (Paxton default)</option>";
                html += String("<option value='1'") + (saved ? " selected" : "") +
                        ">Wiegand (Paxton switched by config card, or third-party reader)</option>";
                html += "</select>";
                html += "<div class='hint'>";
                if (managed) {
                    html += "Set in the admin app, which is the only writer while this "
                            "door is paired. Unpair below to set it here again.<br>";
                }
                html += "Takes effect after a <b>reboot</b>. Running now: <b>" +
                        String(PaxtonReader::modeName(paxton.mode())) + "</b>";
                if ((saved ? PaxtonReader::WIEGAND : PaxtonReader::CLOCK_AND_DATA) != paxton.mode())
                    html += " <span style='color:#e0a458'>(reboot pending)</span>";
                html += ".<br>Wiegand wiring: D0 to the Data terminal, D1 to the Clock "
                        "terminal. Taps that raise the error count on /status but never "
                        "read usually mean the wrong format here or a swapped pair. "
                        "Only 3.3&nbsp;V-safe (open-collector) data outputs may connect "
                        "directly.</div>";
            }

            // ---- Door contact ----
            html += "<h2>Door contact</h2>";
#if PIN_DOOR_CONTACT >= 0
            {
                bool     on      = settings.getBool(KEY_DOOR_CONTACT, false);
                uint32_t heldSec = settings.getUInt(KEY_DOOR_HELD_SEC, DOOR_HELD_DEFAULT_SEC);
                // Same rule as the reader format: while paired the admin app is
                // the only writer. Both writing it meant the cloud silently won
                // at the next sync, so a change made here looked applied and
                // then undid itself minutes later with nothing said.
                bool managedContact = cloudSync.paired();
                html += "<label>Contact on GPIO " + String(PIN_DOOR_CONTACT) + "</label>";
                html += String("<select name='doorContact'") +
                        (managedContact ? " disabled" : "") + ">";
                html += String("<option value='0'") + (on ? "" : " selected") + ">Not fitted</option>";
                html += String("<option value='1'") + (on ? " selected" : "") + ">Fitted</option>";
                html += "</select>";
                html += "<div class='hint'>";
                if (managedContact) {
                    html += "Set in the admin app, which is the only writer while this door is "
                            "paired. Unpair below to set it here again.<br>";
                }
                html += "A magnetic contact on the frame, <b>closed when the door is "
                        "shut</b>, between this pin and GND. Leave <b>Not fitted</b> until one is "
                        "wired: an empty input reads as an open door and raises a forced alert.<br>"
                        "If the door can be opened from inside without the exit button (a lever "
                        "handle), expect false forced alerts until a request-to-exit switch or "
                        "sensor is wired to the exit button input.</div>";
                html += "<label>Held-open alert (seconds)</label>";
                html += "<input type='number' name='doorHeldSec' min='0' max='3600' value='" +
                        String(heldSec) + "'" + (managedContact ? " disabled" : "") + ">";
                html += "<div class='hint'>How long the door may stay open after a release ends "
                        "before it is reported held open. 0 turns the held-open alert off; "
                        "forced alerts still work.</div>";
            }
#else
            html += "<div class='hint'>This board has no free input for a door contact.</div>";
#endif

            // ---- Cloud pairing ----
            CloudSync::Status cs = cloudSync.status();
            html += "<h2>Cloud</h2>";
            if (cs.paired) {
                html += "<div class='hint'>Paired with " +
                        WebService::escapeText(cloudSync.host()) + ".<br>";
                if (cs.everSynced) {
                    html += "Last sync " + String(cs.secsSinceSuccess) + "s ago, roster rev " +
                            String(cs.rosterRev) + ".";
                } else {
                    html += "Paired but has not completed a sync yet.";
                }
                if (cs.lastError.length()) {
                    html += "<br><span style='color:#e0a458'>Last error: " +
                            WebService::escapeText(cs.lastError) + "</span>";
                }
                html += "</div>";
                html += "<label>Re-pair with a new code</label>";
            } else {
                html += "<div class='hint'>Not paired. This door decides access entirely "
                        "from its local roster and reports to nobody. Generate a pairing "
                        "code in the admin app and enter it here.</div>";
                html += "<label>Pairing code</label>";
            }
            html += "<input type='text' name='pairCode' value='' placeholder='8 characters' "
                    "autocapitalize='characters' autocomplete='off'>";

            // Which backend this door talks to. Runtime configuration rather
            // than a compile-time constant, so one firmware image serves every
            // customer instead of one build per deployment.
            html += "<label>Backend host</label>";
            html += "<input type='text' name='cloudHost' value='" +
                    WebService::escapeAttr(cloudSync.host()) +
                    "' autocomplete='off' spellcheck='false'>";
            html += "<div class='hint'>Hostname only, no https:// and no path. "
                    "Changing this <b>unpairs the door</b> &mdash; a device key belongs to "
                    "the backend that issued it &mdash; so enter a pairing code from the new "
                    "one at the same time.</div>";
            html += "<div class='hint'>Codes expire after 15 minutes and work once. "
                    "Pairing contacts the backend, so it may take a few seconds.</div>";
        });
        webService.setSetupSaveHandler([](WebServer& s) {
            if (s.hasArg("doorName")) identity.setDoorName(s.arg("doorName"));
            if (s.hasArg("siteName")) identity.setSiteName(s.arg("siteName"));

            // WiFi change. Handed to WiFiManager, which tries it from loop()
            // and reverts on failure -- deliberately not done here, because
            // switching networks drops the connection this request arrived on
            // and the answer below has to get out first.
            if (s.hasArg("wifiSsid")) {
                String newSsid = s.arg("wifiSsid");
                newSsid.trim();
                if (newSsid.length() && newSsid != WiFi.SSID()) {
                    if (wifiMgr.changeNetwork(newSsid, s.arg("wifiPass"))) {
                        webService.log("[wifi] asked to move to " + newSsid);
                        eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false, "wifi");
                    } else {
                        webService.log("[wifi] change to " + newSsid +
                                       " refused - one already in progress");
                    }
                }
            }

            // Reader format. Saved now, applied at the next boot (see setup()).
            // Logged as a config event for the same reason as the door contact:
            // /setup is unauthenticated, and a wrong format silently stops every
            // fob working at this door -- that change must leave a trace.
            // A disabled select submits nothing, so a paired door normally never
            // gets here -- but the check is what makes that a rule rather than a
            // property of the HTML, since /setup has no login.
            if (s.hasArg("readerMode") && !cloudSync.paired()) {
                uint32_t was = settings.getUInt(KEY_READER_MODE, PaxtonReader::CLOCK_AND_DATA);
                uint32_t now = s.arg("readerMode") == "1" ? PaxtonReader::WIEGAND
                                                          : PaxtonReader::CLOCK_AND_DATA;
                if (now != was) {
                    settings.setUInt(KEY_READER_MODE, now);
                    eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false,
                                    now == PaxtonReader::WIEGAND ? "reader=wiegand"
                                                                 : "reader=cnd");
                    webService.log(String("[reader] format set to ") +
                                   PaxtonReader::modeName((PaxtonReader::Mode)now) +
                                   " - reboot to apply");
                    gReaderRebootPending = (now != (uint32_t)paxton.mode());
                }
            }

#if PIN_DOOR_CONTACT >= 0
            // Recorded as a config event when it changes. /setup is not
            // authenticated, and switching the contact off is exactly how someone
            // would silence a forced-door alert -- so it must leave a trace.
            // Disabled controls submit nothing, so a paired door normally never
            // reaches here -- the check is what makes it a rule rather than a
            // property of the HTML, since /setup has no login.
            if ((s.hasArg("doorContact") || s.hasArg("doorHeldSec")) && !cloudSync.paired()) {
                bool     wasOn   = settings.getBool(KEY_DOOR_CONTACT, false);
                uint32_t wasHeld = settings.getUInt(KEY_DOOR_HELD_SEC, DOOR_HELD_DEFAULT_SEC);
                bool     on      = s.hasArg("doorContact") ? s.arg("doorContact") == "1" : wasOn;
                long     held    = s.hasArg("doorHeldSec") ? s.arg("doorHeldSec").toInt() : (long)wasHeld;
                if (held < 0)    held = 0;
                if (held > 3600) held = 3600;
                if (on != wasOn || (uint32_t)held != wasHeld) {
                    settings.setBool(KEY_DOOR_CONTACT, on);
                    settings.setUInt(KEY_DOOR_HELD_SEC, (uint32_t)held);
                    // At most "contact=1,3600s" (15), which the spool keeps whole.
                    char detail[32];
                    snprintf(detail, sizeof(detail), "contact=%d,%lds", on ? 1 : 0, held);
                    eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false, detail);
                    gDoorReload = true;           // applied by loop(), not here
                }
            }
#endif

            // The backend host is applied BEFORE any pairing code below, because
            // a code is issued by one deployment and redeemed against it. Pairing
            // first would send the new customer's code to the old customer's
            // backend, which rejects it -- and the operator would be looking at a
            // form that plainly shows the right host.
            if (s.hasArg("cloudHost")) {
                String h = s.arg("cloudHost");
                h.trim();
                // Tolerate a pasted URL rather than rejecting it: this is typed
                // by hand at install time, and "https://" is the natural thing to
                // paste from a browser.
                if (h.startsWith("https://")) h = h.substring(8);
                else if (h.startsWith("http://")) h = h.substring(7);
                int slash = h.indexOf('/');
                if (slash >= 0) h = h.substring(0, slash);

                if (h.length() && cloudSync.setHost(h)) {
                    webService.log("[cloud] backend set to " + h + " - device unpaired");
                }
            }

            // Pairing is deliberately handled last: it blocks for a TLS
            // handshake and a round trip, and the name fields above should be
            // saved even if the backend is unreachable.
            String code = s.arg("pairCode");
            code.trim();
            if (code.length()) {
                String err;
                if (cloudSync.pair(code, err)) {
                    webService.log("[cloud] paired successfully, syncing now");
                } else {
                    // Surfaced on /webserial and telnet rather than swallowed:
                    // a failed pairing with no explanation is the sort of thing
                    // that gets blamed on the device.
                    webService.log("[cloud] pairing FAILED: " + err);
                }
            }
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
            if (gReaderRebootPending) {
                // The format in NVS is not the one attached to the pins. Until
                // someone reboots, this door is reading with the old one.
                body += "Reader:   REBOOT PENDING - configured " +
                        String(settings.getUInt(KEY_READER_MODE, PaxtonReader::CLOCK_AND_DATA)
                                   == PaxtonReader::WIEGAND ? "Wiegand" : "Clock&Data") +
                        ", still running:\n";
            }
            body += "Reader:   " + String(PaxtonReader::modeName(paxton.mode())) + ", "
                  + String(paxton.edgeCount())
                  + " edge(s), " + String(paxton.errorCount())
                  + " error(s), " + String(paxton.repairCount())
                  + " repaired\n";
            if (UnlockSchedule::timeValid()) {
                time_t    now = time(nullptr);
                struct tm tm;
                localtime_r(&now, &tm);
                char tbuf[40];
                strftime(tbuf, sizeof(tbuf), "%a %Y-%m-%d %I:%M %p", &tm);
                body += "Time:     " + String(tbuf);
                // WHEN it last synced, not merely that it once did. The clock
                // free-runs on the crystal between syncs, so a device whose NTP
                // has been unreachable for a day still shows a plausible time --
                // and the old "(NTP synced)" label kept saying so for 21.7 hours
                // with all outbound UDP blocked. The threshold comes from the
                // SDK's own poll interval (NTP_STALE_S above), not a guess.
                int32_t since = WiFiManager::secsSinceTimeSync();
                if (since < 0) {
                    // Time is valid but no sync this boot: the RTC survived a
                    // warm reset, so it is real but nothing has confirmed it.
                    body += " (no NTP sync this boot - unverified)\n";
                } else {
                    // Hours carry their minutes. The earlier form switched to a
                    // bare "Nh" at 90 minutes, so 88m became "1h" -- a LARGER
                    // elapsed time rendering as a smaller-looking number. It
                    // fooled a parser watching for the counter to reset, and it
                    // would fool a person reading the page for the same reason.
                    String ago;
                    if      (since < 90)   ago = String(since) + "s";
                    else if (since < 3600) ago = String(since / 60) + "m";
                    else                   ago = String(since / 3600) + "h " +
                                                 String((since % 3600) / 60) + "m";
                    body += (since > NTP_STALE_S)
                        ? " (NTP unreachable for " + ago + " - drifting)\n"
                        : " (NTP synced " + ago + " ago)\n";
                }
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
#if PIN_DOOR_CONTACT >= 0
            if (!gDoorContactOn) {
                body += "Contact:  not fitted (enable on /setup)\n";
            } else if (!gDoor.isOpen()) {
                body += "Contact:  door closed\n";
            } else {
                body += "Contact:  door OPEN " + String(gDoor.openForMs(millis()) / 1000) + "s";
                if (gDoor.forcedThisOpening()) body += "  [FORCED]";
                if (gDoor.heldReported())      body += "  [HELD OPEN]";
                body += "\n";
            }
#else
            body += "Contact:  none (no free input on this board)\n";
#endif
            body += "WiFi:     " + WiFi.SSID();
            if (wifiMgr.setupAPOpen()) body += "  [SETUP AP OPEN]";
            switch (wifiMgr.changeState()) {
            case WiFiManager::CHANGE_TRYING:
                body += "  [trying " + wifiMgr.changeSsid() + "]"; break;
            case WiFiManager::CHANGE_REVERTING:
                body += "  [reverting from " + wifiMgr.changeSsid() + "]"; break;
            case WiFiManager::CHANGE_FAILED:
                body += "  [could not join " + wifiMgr.changeSsid() + " - stayed put]"; break;
            default: break;
            }
            body += "\n";
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
            {
                CloudSync::Status cs = cloudSync.status();
                // Which backend, not just whether it is reachable. With one
                // deployment per customer and one image for all of them, "who is
                // this door reporting to?" stops being answerable from the
                // firmware version and has to be asked of the device.
                body += "Backend:  " + cloudSync.host() + "\n";
                body += "Cloud:    ";
                if (!cs.paired) {
                    body += "not paired\n";
                } else if (!cs.everSynced) {
                    body += "paired, NEVER SYNCED";
                    if (cs.lastError.length()) body += " - " + cs.lastError;
                    body += "\n";
                } else {
                    body += "synced " + String(cs.secsSinceSuccess) + "s ago, rev " +
                            String(cs.rosterRev) + ", " + String(cs.eventsSent) +
                            " event(s) sent";
                    if (cs.stale) body += "  [STALE - roster may be out of date]";
                    if (cs.failures) body += "  [" + String(cs.failures) + " failure(s): " +
                                              cs.lastError + "]";
                    if (cs.fwNote.length()) body += "\n          OTA: " + cs.fwNote;
                    body += "\n";
                }
                // Retry schedule, whenever the door is not on the normal poll.
                // Without this, telling "backing off correctly" apart from
                // "hammering the backend" needs hours of outside observation.
                if (cs.paired && cs.backoffSecs) {
                    body += "Retry:    backing off " + String(cs.backoffSecs) +
                            "s, next attempt in " + String(cs.nextAttemptSecs) + "s\n";
                }
            }
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

        // Sync client last: it depends on WiFi, the roster, the spool and the
        // settings store, and it must never delay any of them coming up. It
        // starts its own task and does nothing at all until the device is
        // paired, so an unpaired door costs no network traffic.
        // Whether it is safe to reboot into new firmware right now. CloudSync
        // knows nothing about doors; this is the project telling it.
        //
        // An OTA ends in a restart, and a restart is only harmless when the door
        // is already locked and idle:
        //   - relayOffAt != 0 means the strike is energised for a grant or an
        //     exit press. Rebooting drops it mid-release.
        //   - gSchedActive means a scheduled-unlock window is open. Rebooting
        //     locks a door that is meant to be open, and the schedule cannot
        //     resume until NTP re-syncs — so people get locked out during
        //     exactly the hours they were told they could walk in.
        // Neither is worth doing to a working door on the backend's schedule.
        // Declining just defers the update to the next sync cycle.
        cloudSync.setSafeToUpdate([]() {
            return relayOffAt == 0 && !gSchedActive;
        });

        // Give CloudSync a voice. Without this its decisions -- refused image,
        // deferred update, download failure -- happen silently, and "the door
        // did not update" is indistinguishable from "it was never offered
        // anything". That ambiguity cost real debugging time once already.
        cloudSync.setLogger([](const String& m) { webService.log(m); });

        // What this door is running, so the admin app can show a format change
        // as pending rather than applied. Wire values, not display labels.
        cloudSync.setReaderMode(paxton.mode() == PaxtonReader::WIEGAND ? "wiegand" : "cnd");
#if PIN_DOOR_CONTACT >= 0
        cloudSync.setHasDoorContact(true);
#else
        cloudSync.setHasDoorContact(false);
#endif

        // Per-door configuration from the admin app, applied on the sync task.
        // Every value is compared against what is already in effect, because
        // this arrives on EVERY sync: acting unconditionally would rewrite NVS
        // and log a config event every 30 seconds.
        cloudSync.setConfigHandler([](const CloudSync::DoorConfig& c) {
            bool changed = false;

            if (c.hasRelayHoldMs) {
                // Clamped rather than trusted: a zero would make every grant a
                // relay glitch nobody could walk through, and an hour would leave
                // the door open. The bounds are what a strike can survive.
                uint32_t v = c.relayHoldMs;
                if (v < 250)   v = 250;
                if (v > 30000) v = 30000;
                if (v != gRelayHoldMs) {
                    gRelayHoldMs = v;
                    settings.setUInt(KEY_RELAY_HOLD_MS, v);
                    webService.log("[cloud] relay hold set to " + String(v) + "ms");
                    changed = true;
                }
            }
            if (c.hasResultHoldMs) {
                uint32_t v = c.resultHoldMs;
                if (v < 500)   v = 500;
                if (v > 30000) v = 30000;
                if (v != gResultHoldMs) {
                    gResultHoldMs = v;
                    settings.setUInt(KEY_RESULT_HOLD_MS, v);
                    webService.log("[cloud] result screen hold set to " + String(v) + "ms");
                    changed = true;
                }
            }
            if (c.hasSchedule) {
                if (c.schedEnabled  != unlockSchedule.enabled()  ||
                    c.schedStartMin != unlockSchedule.startMin() ||
                    c.schedEndMin   != unlockSchedule.endMin()   ||
                    c.schedDaysMask != unlockSchedule.daysMask()) {
                    unlockSchedule.set(c.schedEnabled, c.schedStartMin,
                                       c.schedEndMin, c.schedDaysMask);
                    webService.log("[cloud] unlock schedule updated");
                    changed = true;
                }
            }
#if PIN_DOOR_CONTACT >= 0
            if (c.hasDoorContact && c.doorContact != settings.getBool(KEY_DOOR_CONTACT, false)) {
                settings.setBool(KEY_DOOR_CONTACT, c.doorContact);
                webService.log(String("[cloud] door contact ") +
                               (c.doorContact ? "enabled" : "disabled"));
                gDoorReload = true;          // applied by loop(), not here
                changed = true;
            }
            if (c.hasDoorHeldSec) {
                uint32_t v = c.doorHeldSec > 3600 ? 3600 : c.doorHeldSec;
                if (v != settings.getUInt(KEY_DOOR_HELD_SEC, DOOR_HELD_DEFAULT_SEC)) {
                    settings.setUInt(KEY_DOOR_HELD_SEC, v);
                    webService.log("[cloud] held-open limit set to " + String(v) + "s");
                    gDoorReload = true;
                    changed = true;
                }
            }
#endif
            // Armed, not acted on: raising the AP takes effect at the next boot,
            // so the open network only appears when someone is there to restart
            // the door -- and an admin who changes their mind can clear it first.
            //
            // Edge-triggered against the last REQUEST, not against the armed
            // flag: that flag is cleared when the boot honours it, so a level
            // comparison would re-arm on the very next sync and the open network
            // would return after every reboot until someone noticed.
            if (c.hasSetupAP && c.setupAP != settings.getBool(KEY_SETUP_AP_REQ, false)) {
                settings.setBool(KEY_SETUP_AP_REQ, c.setupAP);
                settings.setBool(KEY_SETUP_AP, c.setupAP);
                webService.log(String("[cloud] setup AP ") +
                               (c.setupAP ? "ARMED for the next boot" : "disarmed"));
                changed = true;
            }

            if (c.hasReaderMode) {
                uint32_t want = c.readerWiegand ? PaxtonReader::WIEGAND
                                                : PaxtonReader::CLOCK_AND_DATA;
                if (want != settings.getUInt(KEY_READER_MODE, PaxtonReader::CLOCK_AND_DATA)) {
                    settings.setUInt(KEY_READER_MODE, want);
                    // Its own event, like a local change: picking the wrong format
                    // stops every fob at this door, so it must be traceable to the
                    // moment it happened.
                    eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false,
                                    want == PaxtonReader::WIEGAND ? "reader=wiegand"
                                                                  : "reader=cnd");
                    webService.log(String("[cloud] reader format set to ") +
                                   PaxtonReader::modeName((PaxtonReader::Mode)want) +
                                   " - reboot to apply");
                }
                // Recomputed every sync, so clearing a pending change by putting
                // the old format back does not leave a stale warning behind.
                gReaderRebootPending = (want != (uint32_t)paxton.mode());
            }

            // One event for the batch. The reader format logs its own above.
            if (changed) {
                eventLog.append(EventLog::EVT_CONFIG, EventLog::R_NONE, false, "cloud");
            }
        });

        cloudSync.begin(&settings, &identity, CLOUD_HOST_DEFAULT);
        webService.log(String("[cloud] ") +
                       (cloudSync.paired() ? "paired - sync task started"
                                           : "not paired - enter a code at /setup"));
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
            // Someone pulling the door as the window closes is not forcing it.
            gReleaseUntil = millis() + DOOR_RELEASE_GRACE_MS;
            gReleaseArmed = true;
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

    // The setup AP is an open network, so it closes itself rather than waiting
    // for someone to remember. A Wi-Fi change in progress keeps it alive: that
    // is exactly when someone is using it.
    if (gSetupApCloseAt != 0 && (long)(millis() - gSetupApCloseAt) >= 0) {
        if (wifiMgr.changeState() == WiFiManager::CHANGE_TRYING ||
            wifiMgr.changeState() == WiFiManager::CHANGE_REVERTING) {
            gSetupApCloseAt = millis() + 60000;      // look again in a minute
        } else {
            gSetupApCloseAt = 0;
            wifiMgr.closeSetupAP();
            webService.log("[wifi] setup AP closed after its timeout");
        }
    }

    // Door contact, after the schedule and relay updates above so it judges an
    // opening against the release state as of this tick.
#if PIN_DOOR_CONTACT >= 0
    if (gDoorReload) {
        gDoorReload = false;
        doorContactLoad();
    }
    if (gDoorContactOn) doorContactPoll();
#endif

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
