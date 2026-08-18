# WiFiManager

Reusable, application-agnostic WiFi connectivity module for ESP32 projects.
Owns everything about *being on the network*: first-boot provisioning, STA
lifecycle and reconnect, mDNS, and NTP time sync. It deliberately does **not**
own the application's web/admin surface — that's a separate concern (see the
`WebService` module); a project composes both.

## Features

- **Captive-portal provisioning** — with no stored credentials, raises an AP
  (SSID set in the constructor) with a captive portal and network scanner.
  Credentials are saved to NVS and the device reboots into STA mode.
- **STA lifecycle** — blocking first connect in `begin()` (with timeout and
  fallback to the portal), then non-blocking reconnect supervision from
  `loop()` at a 5 s cadence.
- **mDNS** — optional; the device becomes reachable as
  `http://<hostname>.local`. The responder is restarted on every reconnect so
  it never goes stale.
- **NTP time sync** — optional; SNTP starts (and re-arms on every reconnect)
  with a POSIX timezone string, so `time()` / `localtime_r()` return correct
  local time with DST handled. The system clock keeps ticking between syncs
  and across WiFi drops.
- **State callbacks** — `onConnected`, `onDisconnected`,
  `onProvisioningStarted` for the application to reflect status (OLED, logs).

## API

```cpp
WiFiManager wifiMgr("Setup-AP");            // AP SSID for provisioning mode
                                            // (optional: AP password, connect timeout)

// Configuration — call all of these BEFORE begin():
wifiMgr.setHostname("my-device");           // enables mDNS: my-device.local
wifiMgr.setTimeSync("EST5EDT,M3.2.0,M11.1.0");  // enables NTP (POSIX TZ string)
                                            // optional args: two NTP servers
                                            // (defaults: pool.ntp.org, time.nist.gov)
wifiMgr.onConnected([](){ ... });           // fires on connect AND reconnect
wifiMgr.onDisconnected([](){ ... });
wifiMgr.onProvisioningStarted([](){ ... });

wifiMgr.begin();     // blocking: STA on stored credentials, else portal AP

void loop() {
    wifiMgr.loop();  // non-blocking reconnect supervision (STA mode only)
}

wifiMgr.isConnected();
wifiMgr.isProvisioning();   // true while the portal AP is up
wifiMgr.localIP();
wifiMgr.getHostname();
wifiMgr.clearCredentials(); // wipe NVS credentials and reboot into the portal

WiFiManager::secsSinceTimeSync();   // static; -1 if NTP has never succeeded
```

Unset features cost nothing: no hostname → mDNS never starts; no timezone →
SNTP never starts.

### Why `secsSinceTimeSync()` exists

"The time looks right" and "the time is being maintained" are different claims,
and without this only the first is visible. The clock free-runs on the crystal
between syncs, so a device whose NTP has been unreachable for a day still shows
a plausible time and nothing says otherwise.

This was observed rather than imagined: a door spent 21.7 hours with all outbound
UDP blocked while its status page went on reporting "NTP synced", because that
label meant *has synced since boot*. Report the age instead, and treat anything
past a couple of hours as suspicious — ESP-IDF re-polls roughly hourly, so
consecutive misses mean it is genuinely not getting through.

It is `static` because the SNTP notification callback is a bare C function
pointer with no user-data argument, leaving nowhere to hang an instance.

## Behaviour notes

- **`begin()` blocks** until either the STA connection succeeds or the
  connect timeout expires (then the portal AP starts). It yields to the
  scheduler while waiting, so FreeRTOS tasks created beforehand keep running.
- **Provisioning mode owns port 80** (portal web server + captive DNS). Don't
  start the application's own web server while `isProvisioning()` is true.
- **mDNS and NTP restart on every reconnect** — both are handled inside the
  module, before the `onConnected` callback fires, so application callbacks
  can rely on them being up.
- **Time validity is the application's concern.** The module only *starts*
  the sync; until the first SNTP response lands, the system clock reads as
  1970. Anything security-relevant (e.g. a timed unlock window) must check
  plausibility itself. For a worked example, RFID_Access's
  `UnlockSchedule::timeValid()` treats any pre-2023 clock as "no trusted time"
  and fails secure, so a reboot with no network leaves its door locked.
- **TZ strings are POSIX format** (`std offset dst,start,end`), e.g. US
  Eastern `EST5EDT,M3.2.0,M11.1.0`, US Central `CST6CDT,M3.2.0,M11.1.0`,
  UK `GMT0BST,M3.5.0/1,M10.5.0`. DST transitions are then automatic.

## Dependencies

Arduino-ESP32 core only (`WiFi`, `WebServer`, `DNSServer`, `ESPmDNS`,
`Preferences`; SNTP via `configTzTime` from the core). No third-party
libraries.
