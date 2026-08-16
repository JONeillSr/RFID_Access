# RFID Access Control

A door-access controller for ESP32 boards. Tap an enrolled RFID card to release a
door strike; everything else is denied. Built around a small set of reusable
libraries (WiFi provisioning, OLED display, splash screen) so the same codebase
drops onto multiple doors and multiple ESP32 variants with only a build-flag
change.

Status indication is on-device and over the network: a 0.96" OLED shows the
current state, an RGB LED gives an at-a-glance verdict, an active buzzer gives
audible feedback, and a built-in web server exposes status and over-the-air
firmware updates.

---

## Features

- **RFID access control** — Paxton P-series proximity reader (Clock & Data or
  Wiegand), enrolled-card allow-list stored in NVS (survives reboot and
  reflash). The reader can be mounted up to 100 m from the controller.
- **Door-side feedback** — drives the reader's own red/amber/green LED lines:
  amber "ready" when idle, green on grant, red on deny.
- **Fail-secure relay** — drives a door strike through a relay; the strike is
  de-energised (locked) whenever the controller is off or unpowered.
- **Exit button** — request-to-exit input (push-to-make to GND) releases the
  door from the secure side, no fob needed.
- **Unlock schedule** — optionally holds the door unlocked during a configured
  time window on selected days (e.g. 08:00–17:00 weekdays), set from the web
  UI and stored in NVS. Time comes from NTP and the schedule **fails secure**:
  no time sync means the door stays locked.
- **Non-blocking throughout** — no `delay()` in the running system; relay
  release, result-screen revert, and LED timing all run off deadlines checked in
  `loop()`. FreeRTOS tasks separate RFID polling, access processing, and the web
  server.
- **OLED status display** — splash screen on boot, then an idle screen showing
  the mDNS hostname, IP address, and OTA hint; GRANTED / DENIED verdicts on tap.
- **Panel status LED** — green while the door is unlocked (plus red on denied,
  on boards with a free red channel; the C6's went to the exit button). Primary
  grant/deny indication is at the door, on the reader's own LEDs. Note the P50
  also beeps by itself on every token read and holds its amber LED as a
  power/ready indicator — both are built into the reader.
- **WiFi provisioning** — first boot raises a captive-portal AP with a network
  scanner; credentials are stored and reused on subsequent boots.
- **mDNS** — reachable at `http://<hostname>.local` once connected.
- **Per-door identity** — one firmware image for every door. Each board derives a
  unique device ID from its MAC, so units never collide on the network, and
  hostname / door name / site name are set per unit on `/setup`.
- **OTA updates** — firmware can be flashed over the network via ElegantOTA at
  `/update`.
- **Remote logging** — live device log over the network at `/webserial` and via
  a telnet console (port 23), showing boot diagnostics and access events with no
  USB cable needed.
- **Multi-board** — one codebase targets the XIAO ESP32-C6, ESP32-C3, ESP32-S3,
  and the classic ESP32 DevKit, selected by a single build flag.

---

## Hardware

### Bill of materials

| Part | Notes |
|------|-------|
| ESP32 board | XIAO ESP32-C6 (primary), or ESP32-C3 / ESP32-S3 / classic ESP32 DevKit |
| Paxton reader | P-series 125 kHz proximity, e.g. P50 / 345-110-US; Clock & Data output |
| Pull-up resistors | 2× 10K, Data and Clock lines to a **clean 3.3 V rail** (not the ESP32's 3V3 pin — see wiring) |
| Exit button | any normally-open momentary switch (e.g. 30 mm arcade button) |
| SSD1306 OLED | 0.96", I²C, address `0x3C` |
| Relay module | e.g. SONGLE SRD-05VDC-SL-C, active-LOW input with onboard driver + coil flyback diode |
| Door strike | DC, fail-secure (e.g. RCI L65) |
| Flyback diode | 1N4007 across the door strike — **required**, see Troubleshooting |
| RGB LED | common-cathode, with a current-limiting resistor per channel (220–330 Ω) |
| Active buzzer | |
| Power | 12 V supply; buck converters to 5 V and 3.3 V |

### Pin assignments

Pins are defined per board in `src/Pins.h`, with board-identity facts (I²C
defaults, capability flags, strapping-pin warnings) in `lib/BoardConfig/BoardConfig.h`.
Application code never references a raw GPIO — only the `PIN_*` names — so
retargeting a board is purely a build-environment change.

> **Verification status.** The **XIAO ESP32-C6** map is fully bench-verified
> in a working build: relay fail-secure confirmed clean through reboot with a
> meter, and Paxton P50 reads confirmed end-to-end (Clock & Data decode with
> LRC validation, reader LED control, exit button). The **classic ESP32
> DevKit** relay/buzzer/OLED/RGB pins are bench-verified from the RC522-era
> build; its Paxton pins reuse GPIOs proven there but haven't carried reader
> traffic yet. The **C3** and **S3** maps are plausible, strapping-pin-safe
> defaults that have **not** been hardware-verified. Before deploying to a C3
> or S3, confirm the relay pin shows no spurious pulse at boot (see
> Troubleshooting → "Verify the relay pin at boot"). Adjust `src/Pins.h` as
> needed.

**XIAO ESP32-C6:**

| Function | GPIO |
|----------|------|
| Paxton Data/D0 | D8 |
| Paxton Clock/D1 | D9 |
| Paxton Red / Green / Amber LED | D3 / D10 / D0 |
| Buzzer | D2 |
| Relay IN | GPIO1 |
| OLED SDA / SCL | 22 / 23 |
| Exit button | D6 (to GND) |
| Panel LED G | D7 (lights while door unlocked) |
| Panel LED R / B | not assigned — D6 was repurposed for the exit button; the reader's red LED shows "denied" at the door |

On the C6 every pad is now in use, so the panel LED is reduced to its green
channel only (dark idle, green while the door is unlocked). Full at-the-door
feedback — amber ready, green granted, red denied — comes from the Paxton
reader's own LEDs.

**Classic ESP32 DevKit:**

| Function | GPIO |
|----------|------|
| Paxton Data/D0 | 19 |
| Paxton Clock/D1 | 18 |
| Paxton Red / Green / Amber LED | 17 / 23 / 4 |
| Buzzer | 13 |
| Relay IN | 14 |
| OLED SDA / SCL | 21 / 22 |
| Exit button | 32 (to GND) |
| LED R / G / B | 25 / 26 / 27 |

### Paxton reader wiring

The reader connects with the same terminal roles as on a Paxton Net2 ACU —
if migrating from a Net2, each reader wire moves from the Net2 reader-port
terminal to the matching ESP32 connection.

Paxton P50 (345-110-US) wire colors — bench-verified on this unit by
grounding the LED lines and watching the data lines during a tap. Note the
loom differs from older Paxton manuals (no brown wire; Red LED is purple
here, not brown):

| Wire color | Function | Connection (XIAO C6) |
|-----------|----------|----------------------|
| Red | 12V | PSU + |
| Black | 0V | PSU − / ESP32 GND (plus cable screen) |
| Yellow | Data/D0 | D8 (10K pull-up to 3V3) |
| Blue | Clock/D1 | D9 (10K pull-up to 3V3) |
| Purple | Red LED | D3 |
| Orange | Amber LED | D0 |
| Green | Green LED | D10 |
| White | Media detect (reader output; unconfirmed) | not connected |

Yellow/blue both carry the transmission, so a meter can't tell Data from
Clock — wire as above and tap a token: card numbers reading correctly
confirms it, while a rising decode-error counter on `/status` with no reads
means the pair is swapped (swap the two wires and retest).

> **Warning:** Paxton colors do NOT follow the common Wiegand convention.
> On most third-party readers green = D0 and white = D1; on a Paxton, green
> is an LED line — the data pair is **yellow and blue**. Grounding an LED
> wire lights that LED; the data/clock wires blip low during a card read.

- **12V / 0V** — from the 12 V supply. The reader's 0V must be commoned with
  the ESP32's ground, or the data lines have no reference.
- **Data/D0 and Clock/D1** — open-collector outputs from the reader. Fit a
  **10K pull-up from each line to a clean 3.3 V source** (never to 5 V or
  12 V — ESP32 pins are not 5 V tolerant). Verify with a meter that both lines
  idle at ~3.3 V before connecting to the GPIOs.

  > **Use a stiff 3.3 V rail for the pull-ups, not the ESP32 module's own
  > 3V3 pin.** Bench-proven the hard way: pull-ups referenced to the ESP32's
  > 3V3 pin produced randomly duplicated clock bits (the module's rail
  > bounces with WiFi transmit bursts, so slow rising edges re-cross the
  > input threshold). Moving the pull-ups to a dedicated 3.3 V buck output
  > fixed it completely. The pull-up rail must share ground with the ESP32,
  > power up/down with it, and measure ≤ ~3.5 V.
- **Red / Amber / Green LED** — active-low control inputs; the firmware drives
  them open-drain (sinks to light, floats to release), the same way the Net2
  does. Measure each LED line's idle voltage first: if one floats above
  3.3 V, sink it through a small NPN transistor or N-MOSFET instead of the
  GPIO directly.
- **Media Detect** — unused; leave disconnected.

Cable: Cat5 or Belden 8723 class, up to **100 m**. Past 25 m, double up the
12V and 0V cores to limit voltage drop (Paxton's own guidance for Net2 runs).

### Exit button and unlock schedule

**Exit button:** any push-to-make button wired between `PIN_EXIT_BTN` and
GND (internal pull-up; active low). Pressing it releases the door for the
normal relay hold time — no fob required. Wiring is identical to the Net2's
Exit/0V terminal pair.

**Unlock schedule:** configured on the `/config` page — enable, start/end
time, and days of week; an end time before the start time makes an overnight
window. While active, the relay is held energised, the reader LED shows
green, and the OLED idle screen reads `** DOOR UNLOCKED **`. The clock comes
from NTP (synced on WiFi connect; timezone is `TZ_INFO` in `src/main.cpp`,
default US Eastern). Fail-secure rule: if the clock has never synced since
boot, the schedule stays inactive and the door stays locked. Note that a
fail-secure strike is energised for the whole window — check your strike's
duty-cycle rating, or use a maglock/continuous-duty strike for long windows.

The reader outputs **Clock & Data** (Paxton's native format) by default, which
is what the firmware expects. If the reader has been switched to Wiegand with a
Paxton configuration card — or a third-party Wiegand reader is fitted — change
the mode argument in the `PaxtonReader` constructor in `src/main.cpp` to
`PaxtonReader::WIEGAND`.

### Relay and door-strike wiring

The relay switches +12 V to the door strike's positive terminal (high-side
switching); the strike's other lead returns to ground. **A 1N4007 flyback diode
must be fitted directly across the door strike's two terminals**, banded end
(cathode) toward the strike's positive (relay-NO) lead. Without it, the strike's
inductive kick on relay release couples into the shared supply and can wedge the
RFID reader. The relay module's own onboard diode protects only the relay coil,
not the strike — see Troubleshooting for the full story.

---

## Software setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- The **pioarduino** fork of the ESP32 platform — required for current
  ESP32-C6 support. This is pinned in `platformio.ini`, so PlatformIO fetches it
  automatically; no manual install needed.

### Build environments

Each board has its own environment in `platformio.ini`. The default is the C6.

| Environment | Board | Build flag |
|-------------|-------|------------|
| `seeed_xiao_esp32c6` | XIAO ESP32-C6 (default) | `BOARD_XIAO_ESP32C6` |
| `esp32c3` | ESP32-C3 DevKitM | `BOARD_ESP32C3` |
| `esp32s3` | ESP32-S3 DevKitC | `BOARD_ESP32S3` |
| `esp32dev` | classic ESP32 (WROOM) | `BOARD_ESP32DEV` |

All environments use the `min_spiffs` partition scheme to keep two OTA app slots
on 4 MB flash. ElegantOTA is forced into sync-WebServer mode via
`-D ELEGANTOTA_USE_ASYNC_WEBSERVER=0`.

### Build and flash

Build for the default board (C6):

```
pio run
```

Build for a specific board:

```
pio run -e esp32dev
```

Flash over USB:

```
pio run -e seeed_xiao_esp32c6 -t upload
```

Serial monitor (115200 baud):

```
pio device monitor
```

> **Note on the ESP32-C6 and serial:** the C6 uses native USB CDC, which drops on
> reboot and re-enumerates too slowly to catch boot messages. Diagnostics on the
> C6 therefore go to the OLED and the web `/status` page, not the serial monitor.
> The classic ESP32 DevKit uses an external USB-UART chip whose serial **survives
> reboot**, so on that board the monitor is fully usable for boot debugging.

---

## First-time use

1. **Flash** the firmware for your board.
2. **Provision WiFi.** On first boot (no stored credentials), the device raises a
   WiFi access point named `RFID-Setup`. Join it from a phone or laptop; a
   captive portal opens automatically (or browse to `http://192.168.4.1`). Pick
   your network, enter the password, and save. The device reboots and connects.
3. **Find the device.** Once connected, the OLED shows the hostname, the IP
   address, and the OTA hint. Out of the box the hostname is this board's unique
   device ID (`rfid-a1b2c3.local`) — read it off the OLED and browse to it, or
   use the IP. Give it a friendlier name on `/setup`.
4. **Enrol cards** via the web interface, then tap to test: a granted card
   energises the relay (green LED, door unlocks); an unknown card is denied (red
   LED).

### Web endpoints and remote logging

The web server, OTA, status page, and remote log are owned by the reusable
`WebService` module (`lib/WebService/`), which creates the server, serves the
common admin pages, and lets the project register its own routes and inject
app-specific status fields.

| Path | Purpose |
|------|---------|
| `/` | main web UI (card enrolment) |
| `/config` | fob management + unlock-schedule configuration |
| `/setup` | device settings: splash hold, mDNS hostname, door name, site name; plus a **Reboot device** button |
| `/api/schedule` | GET current schedule/state, POST to update |
| `/api/add` `/api/rename` `/api/remove` | fob management (POST, JSON) |
| `/status` | live status page: device ID, door and site, WiFi IP, mDNS name, uptime, OLED, reader counters, time, schedule, enrolled count, last tap |
| `/footer.js` | shared footer the WebService module injects into every page |
| `/update` | ElegantOTA firmware upload |
| `/webserial` | live device log (self-refreshing); shows boot diagnostics and access events |

A **telnet log console** is also available on port 23 (`telnet <host>.local`),
showing the same log stream as `/webserial`. Both are fed by `webService.log()`
calls placed at key events (reader-interface boot line, WiFi connect, each
access grant or deny). Note the telnet console is plaintext and unauthenticated — fine for a
trusted LAN or bench, but consider disabling it on field units exposed to
untrusted networks.

### Per-door deployment

**One firmware image serves every door.** Identity comes from hardware and NVS,
not from a build flag, so nothing needs recompiling per unit.

Each board derives a permanent **device ID** from its MAC — `rfid-a1b2c3` — which
is unique out of the box, survives reflashing and an NVS erase, and is what a
backend keys on. Until you set a hostname, that device ID *is* the mDNS name, so
**two units can never collide on the network by default**.

On `/setup` you can give each unit friendlier labels:

| Field | Purpose |
|-------|---------|
| mDNS hostname | Network name, e.g. `rfid-front` → `http://rfid-front.local` |
| Door name | Human label for reports, e.g. "Front Door" |
| Site name | Groups doors sharing a building, e.g. "Main Shop" |
| Device ID | Read-only; the fixed MAC-derived identity |

Door and site names apply immediately. A changed **hostname takes effect after a
reboot**, since the mDNS responder is started at boot — and `/setup` carries a
**Reboot device** button for exactly that, so renaming a door doesn't mean walking
to it. The reboot endpoint is POST-only, so no link prefetch or crawler can
restart a unit. The lock is fail-secure, so a reboot leaves the door locked.

Hostnames are sanitised before use — lowercased, restricted to `a-z0-9-`, spaces
and underscores folded to `-`. Typing "Front Door" yields `front-door` rather than
an invalid label that would make the unit silently unresolvable.

Identity flows from `DeviceIdentity` into `wifiMgr.setHostname()`, the OLED idle
screen, and the `/status` page, which now leads with device ID, door, and site so
you can tell units apart at a glance.

---

## Project structure

```
lib/                    reusable, application-agnostic libraries
  BoardConfig/          per-board identity + capability flags (compile-time)
  Display/              SSD1306 OLED helpers (caller owns the I2C bus)
  SplashScreen/         boot splash
  WiFiManager/          provisioning, STA lifecycle, mDNS, NTP time sync
  WebService/           owns the web server: /status, /update (OTA),
                        /webserial, telnet log console, shared page footer,
                        optional /setup; projects add routes
  DeviceSettings/       NVS settings store (WebService dependency)
  DeviceIdentity/       MAC-derived device ID, mDNS hostname, door/site labels
  Roster/               LittleFS credential store: hash-keyed, CRC-checked,
                        atomic save, atomic wholesale replace for cloud sync
  EventLog/             durable append-only event spool held until the backend
                        acknowledges it (taps, exit, schedule, boot, firmware)
  CloudSync/            backend sync client + OTA: uploads spooled events,
                        applies the roster/config the server returns, and takes
                        firmware updates. Runs on its own task and is NEVER in
                        the access decision path. Embeds its own TLS trust
                        anchors (certs/ + RootCerts.h) -- an ESP32 has no system
                        trust store.
  AccessControl/        access decision + recent-tap log (storage lives in Roster)
  Events/               app event types + queue (card-tap, exit-request)
  PaxtonReader/         Clock & Data / Wiegand reader driver + reader LEDs
  UnlockSchedule/       NVS-backed timed-unlock window (fails secure w/o NTP)
  WebHandlers/          project-specific web UI routes
src/                    project-specific code
  main.cpp              wiring, tasks, screens, relay/LED/buzzer logic
  Pins.h                per-board peripheral pin map
  JTLogoBitmap.h        splash bitmap
tools/                  operational scripts
  backup-doors.ps1      snapshot a door's fobs + schedule before a reflash
  check_partitions.py   validate partitions_rfid.csv (incl. NVS preservation)
  check_roster_logic.py exercise Roster's ordering/lookup algorithms
  gen_certs.py          regenerate lib/CloudSync/RootCerts.h from the PEMs in
                        lib/CloudSync/certs/ (see that header for how to
                        re-extract and verify an anchor before trusting it)
cloud/                  Azure backend (TypeScript, deployed separately)
  shared/types.ts       contract shared by the API and the admin web app
  api/                  Azure Functions: device sync/enroll + the admin API
  web/                  admin web app (Preact + Vite) on Static Web Apps
  infra/                Bicep: storage, tables, Function App, SWA, observability
```

Each of those three has its own README covering deployment and the decisions
behind it; the admin app is live at `https://access.jtcustomtrailers.com`.

`ROADMAP.md` tracks the multi-door / cloud programme: what is built, what is
next, and — more usefully — why each decision went the way it did.

The split is deliberate: `lib/` holds code reusable across projects and visible
to other libraries; `src/` holds this project's specifics. Two complementary
networking modules divide the work cleanly: **WiFiManager** owns *connectivity*
(provisioning, STA lifecycle, reconnect, mDNS), while **WebService** owns the
*web/admin surface* (the long-lived server, OTA, status, remote logging).
Neither reaches into the other; a project composes both.

---

## Design principles

- **Non-blocking only.** `delay()` is forbidden in the running system; all timing
  uses deadlines checked in `loop()` or `vTaskDelay` inside tasks.
- **Canonical storage, convert at the edges.** State is stored in base units and
  formatted only at the display/input boundary.
- **Modular and board-agnostic.** Reusable modules in `lib/`, project specifics in
  `src/`, board differences behind a single build flag.
- **Fail-secure.** The lock defaults to secured; the relay is brought to a safe
  state before anything else at boot.

---

## Troubleshooting

### Granted tap locks up the reader (no further cards read until power-cycle)

**Cause:** inductive flyback. On a granted tap the relay energises the door
strike; when the relay releases, the strike's collapsing magnetic field produces
a voltage spike. On a shared supply that spike couples into the RFID reader and
wedges it. The tell is that the lockup happens on relay **release**, and only on
granted taps (denied taps never fire the relay).

**Fix:** fit a 1N4007 flyback diode **directly across the door strike's two
terminals**, banded end (cathode) toward the strike's positive (relay-NO) lead.
Mount it at the strike, not back at the board, so the spike is clamped at its
source. The relay module's onboard diode only protects the relay coil — the
external strike is a separate inductor and needs its own clamp.

This applies to **DC** strikes. An AC strike needs an RC snubber or MOV instead;
a diode across an AC coil will conduct every half-cycle and fail.

### Reading the reader counters on /status

`Reader: Paxton Clock&Data, E edge(s), N error(s), R repaired` is a built-in
diagnostic ladder (a tap should add ~80-100 edges):

- **Edges don't move on a tap** → no signal reaches the GPIOs at all. The
  reader beeping proves nothing — its sounder fires on every read, powered or
  not by your wiring. Check, in order: reader 0V **commoned with the ESP32
  ground** (without a shared reference the open-collector lines never swing);
  the pull-ups (both lines should idle at ~3.3 V at the ESP32); the wires on
  the right pads. Tip: the exit button test — press it and watch for the
  reader's green LED — proves ground + LED drive in one shot.
- **Edges jump, errors increment, never a card number** → bits arrive but
  won't parse. Every failed frame is logged with its raw bits
  (`[paxton] undecodable frame: 0001101...` at `/webserial`), which makes
  the cause readable: wrong line format (reader switched to Wiegand → change
  the constructor mode in `src/main.cpp`), swapped Data/Clock, or all-same
  bits (the data pin is on the wrong wire — Media Detect also dips during a
  read and fools a multimeter).
- **Reads work but `repaired` keeps climbing** → line quality is marginal.
  The driver absorbs one duplicated bit per frame (ISR glitch filter +
  parity/LRC-gated single-bit repair), but fix the electricals: the classic
  cause is pull-ups referenced to the ESP32 module's own 3V3 pin, which
  bounces with WiFi bursts — move them to a stiff 3.3 V rail (bench-proven
  here). On long runs also drop to 4.7K and pair each signal with 0V, not
  with each other.

### Reader LEDs don't respond (or an LED is dimly on)

The LED lines are active-low and driven open-drain. A line that floats above
3.3 V at idle exceeds what the GPIO can safely sink directly — buffer it with a
small NPN transistor or N-MOSFET (gate/base from the GPIO, drain/collector to
the LED line, source/emitter to ground) and the firmware logic is unchanged.

### OLED stays dark / not found

On the ESP32-C6, `Wire1` (the second I²C peripheral) is broken
([arduino-esp32 #10685](https://github.com/espressif/arduino-esp32/issues/10685));
all I²C devices must share the single `Wire` bus on GPIO 22/23. Separately, the
Adafruit SSD1306 `begin()` defaults to re-initialising the I²C peripheral with no
pin arguments, which overrides explicitly configured pins. The Display library
works around this by passing `periphBegin=false` and adding a short settle delay
before `begin()`. If you port the display code elsewhere, carry those workarounds
with it.

### Can't see boot messages on the C6

Expected. The C6's native USB CDC serial drops on reboot and reconnects too
slowly to catch boot output. Use the OLED and the `/status` web page for
diagnostics, or build for the classic ESP32 DevKit (external USB-UART) when you
need to watch boot. This is a hardware characteristic, not a bug.

### Verify the relay pin at boot (non-C6 boards)

The relay pin must not glitch high during boot, or a fail-secure lock could
momentarily release at power-up. The C6's relay pin (GPIO1) is bench-verified
clean. For any other board, before connecting the strike: flash the firmware with
the strike disconnected, put a meter on the relay output, and power-cycle several
times watching for any spurious pulse. If you see one, move the relay to a
different non-strapping GPIO in `src/Pins.h` and retest. Only connect the strike
once the pin is proven quiet.

### `.local` address doesn't resolve

mDNS `.local` resolution works from most macOS, iOS, Linux, and modern Windows
clients, but some Android devices and older Windows versions can't resolve it.
Use the IP address shown on the OLED as a fallback. The IP is always displayed
alongside the hostname for this reason.

### Build fails: a library can't find a header

PlatformIO compiles each `lib/` library in isolation — a library can see other
libraries but **cannot** see `src/`. If a library include fails, the header it
wants is probably in `src/`. Move shared headers into their own `lib/` module
(as is done for `Events/`); keep project-only headers in `src/`.

---

## License

Released under the MIT License. See [`LICENSE`](LICENSE) for details.
