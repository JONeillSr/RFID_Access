# Multi-Door RFID Access: Central Sync + Azure Backend

## Context

`RFID_Access` today is a single-door controller. Three things make it single-door
by construction:

1. **Identity is compile-time.** `MDNS_HOSTNAME` is a `#define` in
   [src/main.cpp:68](src/main.cpp#L68), so every door needs its own firmware build.
2. **The roster is local and tiny.** `Entry allowList[30]` in
   [lib/AccessControl/AccessControl.h:13](lib/AccessControl/AccessControl.h#L13), persisted as a JSON blob in NVS.
   Enrolling a fob means visiting each door's web page.
3. **The audit trail is volatile.** `tapLog[20]` is a RAM ring, lost on every
   reboot, and there is no notion of *which* door a read happened at.

The goal is a fleet of 5–20 doors sharing one fob database, with a durable,
queryable read log, administered centrally through Entra ID — while keeping each
door fully functional when the network is down.

**The governing constraint: the access decision stays 100% local, always.** The
cloud syncs the roster down and drains events up. A WAN outage, an expired
certificate, or an Azure incident must never change whether a valid fob opens a
door. This is why the answer to "device or cloud?" is *both, with different
jobs*: the device holds a cached roster plus a persistent event spool; the cloud
holds the system of record.

### Decisions locked in

| Area | Choice |
|------|--------|
| Backend | Azure Functions (HTTPS REST) + Table Storage |
| Scale | 5–20 doors, 50–300 fobs |
| Permissions | People → groups; doors allow groups. Server flattens; device stays dumb |
| Admin | Static Web App + Entra ID — the single place doors are viewed and configured |
| Device pages | **Read-only once paired.** Enrollment is cloud-only; no local writes |
| API language | TypeScript / Node 24 — shares types with the SWA frontend |
| Primary report | **"Did this person enter any door?"** — person-centric, spanning the fleet |
| Secondary report | "This person at this door" and "everyone at this door" |

Azure IoT Hub was considered and rejected: device twins fit the roster-sync shape
well, but MQTT + SAS-token renewal on the ESP32 is a large firmware surface and a
recurring failure mode, for benefits this scale doesn't need.

---

## Architecture

```
   ┌───────── Azure ─────────────────────────────────┐
   │  Static Web App (Entra ID)  ──► Function App    │
   │                                    │            │
   │                             Table Storage       │
   │              People / Credentials / Groups /    │
   │              Doors / Meta                       │
   │              EventsByPerson ⭐ / EventsByDoor    │
   │                             Blob (firmware)     │
   └────────────────────┬────────────────────────────┘
                        │  HTTPS, one POST /api/v1/sync every ~30 s
        ┌───────────────┼───────────────┐
    ┌───▼───┐       ┌───▼───┐       ┌───▼───┐
    │ door  │       │ door  │       │ door  │   each: cached roster (LittleFS)
    │  A    │       │  B    │       │  C    │         + event spool (LittleFS)
    └───────┘       └───────┘       └───────┘         + local decision path
```

**One combined sync endpoint**, not separate roster/event calls: a single TLS
handshake per cycle carries the event batch up and the roster/schedule/firmware
state down. On a constrained device the handshake is the expensive part.

---

## Phase 1 — Device: runtime identity ✅ COMPLETE

> **Verified on hardware 2026-08-13** — two classic ESP32 DevKit units, one
> flashed over USB and one over OTA (a unit already mounted above a ceiling).
> Distinct MAC-derived identities, independent `.local` names, `/setup` labels,
> and the reboot button all confirmed in place.

One binary for every door. Identity comes from hardware + NVS, never from a build
flag.

- **New `lib/DeviceIdentity/`** — `deviceId` derived from the efuse MAC
  (e.g. `rfid-a1b2c3`), immutable and unique. `doorName` and `siteName` read from
  `DeviceSettings` (reuse the existing generic `getString`/`setString` accessors in
  [lib/DeviceSettings/DeviceSettings.h:63](lib/DeviceSettings/DeviceSettings.h#L63) — no new store).
- **Delete `MDNS_HOSTNAME`.** ⚠️ *Blocking today:* every unit currently claims
  `rfid-door.local`, so the second door onward collides — whichever responds first
  wins, intermittently, and you can't reliably reach a specific unit. Hostname
  becomes `DeviceSettings::hostname()` (`KEY_HOSTNAME` already exists), defaulting
  to `rfid-<mac>` so **two units are never identical out of the box**, and
  renameable to `rfid-front`/`rfid-shop` at `/setup`. Fix this first — it's a
  prerequisite for bench-testing anything else with two boards.
- **Enable the existing `/setup` page.** `WebService::enableSetup()` and
  `setSetupFieldsProvider()` are already built ([WebService.h:100](lib/WebService/WebService.h#L100)) but
  RFID_Access never calls them. Wire them up and inject door name, site, and the
  cloud pairing field (Phase 4).

## Phase 2 — Device: scalable roster + persistent event spool ✅ COMPLETE

> **Verified on hardware 2026-08-15**, firmware 2.2.1. Partition table applied
> over USB to both DevKits with **NVS preserved** — enrolled fobs survived
> untouched, exactly as the offset analysis predicted. Roster confirmed
> persisting (`Roster: 272 B on disk`), spool confirmed surviving reboot.
> Migration off the NVS allow-list ran automatically on first boot.

**New `lib/Roster/`** replaces the fixed array behind `AccessControl`'s existing API.

- LittleFS-backed binary store; RAM index for O(log n) lookup.
- **Credentials stored as a salted SHA-256 truncated to 8 bytes**, names in clear.
  A door only ever holds hashes, so a stolen controller doesn't yield a clonable
  card list. Costs nothing — mbedTLS is already linked for TLS. Unknown-card
  enrollment still works: the raw number exists transiently at tap time, is shown
  in the UI, and is uploaded with the deny event so the central UI can enroll it.
- **Atomic swap on sync**: build the new roster fully, then swap the active
  pointer under `dataMutex` ([AccessControl.h:20](lib/AccessControl/AccessControl.h#L20)). Never mutate in place —
  a tap arriving mid-sync must see either the old list or the new one.
- Tracks `rosterRev` so sync can skip unchanged payloads.

**New `lib/EventLog/`** — append-only spool on LittleFS, ~40 B/record.

- **Identity**: `bootId` (one NVS write per boot) + per-boot `idx`. The tuple
  `(deviceId, bootId, idx)` is the server-side idempotency key — retries are free,
  no per-event NVS writes.
- **Timestamps before NTP**: every record stores `uptimeMs`, plus `epoch` if the
  clock is valid. At upload, unresolved records are converted using the batch's
  `bootEpoch` and flagged `approx`. This matters because `UnlockSchedule::timeValid()`
  ([UnlockSchedule.h:36](lib/UnlockSchedule/UnlockSchedule.h#L36)) is often false for the first seconds after boot.
- Records: tap (with raw credential + reason code), exit-button, schedule
  transition, boot, config change, sync failure.
- Read cursor persisted; records retained until the server acks. Ring-overwrites
  oldest when full.

**Rewire `AccessControl::acProcessEvent`** ([AccessControl.cpp:87](lib/AccessControl/AccessControl.cpp#L87)) to query
`Roster` and append to `EventLog`. Keep `tapLog` as-is — it backs the existing
`/api/taps` handler and the `/status` "Last tap" line, and is cheap.

**`platformio.ini`**: add LittleFS, `FW_VERSION`, and a custom partition table.
`min_spiffs.csv` leaves only 128 KB of filesystem, which the roster and spool
have to share.

**Settled** — `partitions_rfid.csv` is drafted and validated against the real
`min_spiffs.csv` by `tools/check_partitions.py`:

| | app0 / app1 | filesystem |
|---|---|---|
| `min_spiffs.csv` | 0x1E0000 (1.875 MB) | 0x20000 (128 KB) |
| `partitions_rfid.csv` | 0x1D0000 (1.8125 MB) | **0x40000 (256 KB)** |

The budget is fixed — after the 64 KB header and the 64 KB coredump,
`2*app + fs = 0x3E0000` — so every 64 KB removed from each app slot buys 128 KB
of filesystem. Taking just 64 KB doubles the filesystem and moves the DevKit
image from 64.1% to 66.3% of its slot (625 KB still free for TLS). App size is
the hard wall: an image that outgrows its slot cannot flash at all, whereas a
smaller spool only shortens how long a door tolerates being disconnected. 256 KB
holds the roster (~12 KB at 300 credentials) plus ~5,900 events — roughly a month
of total outage at a busy door. The spool is a *buffer*; the cloud is the archive.

**`nvs` and `otadata` keep byte-identical offsets and sizes** (0x9000/0x5000 and
0xe000/0x2000), so enrolled fobs, WiFi credentials and the unlock schedule
survive the change. A USB upload writes only 0x1000/0x8000/0xe000/0x10000 and
never touches the NVS sectors at 0x9000–0xdfff.

> ⚠️ **This step cannot be delivered over OTA.** OTA writes only the inactive app
> slot; the partition table at 0x8000 is not part of that payload. Every unit
> needs a USB flash for this one change — including any mounted somewhere
> awkward. Mitigation for a mixed fleet: the filesystem partition is still
> *named* `spiffs`, which is the label `LittleFS.begin()` defaults to, so an
> image built for the new table still mounts a filesystem on a device left on
> the old one — degraded to 128 KB rather than failing.

## Phase 3 — Backend: Azure Functions + Table Storage ✅ DEPLOYED

> **Live as of 2026-08-15.** `POST /api/v1/sync` and `POST /api/v1/enroll` are
> deployed and verified rejecting unauthenticated, wrong-key and malformed
> requests. Data model seeded and verified.
>
> Endpoint: `https://jtc-prod-rfidaccess-eastus2-func.azurewebsites.net/api/v1/`
> Resource group: `JTC-prod-rfidaccess-eastus2-rg` — see `cloud/infra/README.md`.
>
> **Region is East US 2, not East US.** Flex Consumption is not offered in
> eastus, and Flex is what allows the deployment package to be read by managed
> identity instead of a storage account key — which is what lets the data
> account keep `allowSharedKeyAccess: false`. Y1 Consumption and a key-less
> storage account are mutually exclusive; that constraint drove the region.
>
> Still outstanding in this phase: admin CRUD endpoints (they arrive with the
> Phase 5 UI), firmware offers in the sync response (the per-board gate below
> needs care), and the retention/archive timer.

New `cloud/` tree, outside the PlatformIO project. **TypeScript / Node 24**
Function App, with the sync contract, entity shapes, and report types in a shared
`cloud/shared/types.ts` that both the API and the Static Web App import — so the
two halves cannot drift.

### People are separate from credentials

Reports are about *people*, not fobs. One person may carry a card and a keyfob;
a fob may be reassigned. So the model splits them, and **groups attach to the
person**, not the credential:

| Table | PK | RK | Fields |
|-------|----|----|--------|
| `People` | `person` | personId | name, email, active, groups, notes |
| `Credentials` | `cred` | credId | number, personId, label ("blue fob"), active, validFrom/To |
| `Groups` | `group` | groupId | name |
| `Doors` | `door` | deviceId | name, site, **board**, groups, schedule, config, keyHash, lastSeen, fw, rosterRev |
| `Meta` | `meta` | `rosterRev` | global monotonic counter |

### Events are written twice, on purpose

Table Storage has **exactly one index** (PK+RK) and no secondary indexes. A
schema keyed by door answers "who came through door A" in one partition scan, but
answers "did Alice enter *anywhere*" only by scanning all 20 doors × every month —
which is precisely the query you said you'd run most.

The standard fix is to denormalize: write each event into two partition schemes.

| Table | PK | RK | Serves |
|-------|----|----|--------|
| `EventsByPerson` | `{personId}-{yyyyMM}` | `{invTicks}-{deviceId}-{bootId}-{idx}` | ⭐ **"Did this person enter any door?"** — one partition scan |
| `EventsByDoor` | `{deviceId}-{yyyyMM}` | `{invTicks}-{bootId}-{idx}` | "Everyone at this door" — one partition scan |

Both carry the full denormalized row, so neither query needs a join.
*"This person at this door"* is the `EventsByPerson` partition with a
`deviceId eq` filter — still a single small partition, so no extra scheme needed.
Inverted ticks (`long.MaxValue - ticks`) in the row key make every query return
newest-first with no sorting.

Storage cost of duplication is irrelevant here (events are ~200 bytes; the whole
fleet generates maybe a few MB a year).

**Three consequences to get right:**

1. **The two writes are not atomic.** Table Storage entity-group transactions
   only span a single partition, and these are different PKs by definition. Write
   `EventsByPerson` first (the primary query path), then `EventsByDoor`. Both use
   `InsertOrReplace` keyed on `(deviceId, bootId, idx)`, so a retry after partial
   failure converges. Log — and alert on — any write that lands in one table only.
2. **Resolve identity at ingest, then freeze it.** The device sends a raw *card
   number*, not a person. The Function resolves number → credential → person at
   write time and **copies `personName` and `doorName` into the event row**. If a
   fob is later reassigned from Alice to Bob, historical events must still read
   Alice — a report that rewrites history is worse than no report.
3. **Unknown cards have no person.** They land in `EventsByPerson` under
   `unknown-{yyyyMM}`, which conveniently *is* the "unknown taps" feed the admin
   UI enrolls from.
4. **Some events have no person at all** — exit-button presses, schedule
   transitions, boots, sync failures. These go to `EventsByDoor` **only**; there
   is no sensible person partition for them and inventing one would pollute the
   person report. The door timeline is the complete record; the person timeline
   is a filtered view of it.

   The exit button matters most here: it releases the door with **no record of
   who**, by design — it sits on the secure side and is honoured unconditionally
   (fire egress). So a door timeline reads as a mix of attributable entries and
   unattributable releases, and that is the truth of the installation rather than
   a modelling failure. Worth an explicit report: *exit events with no preceding
   grant at that door* — someone leaving who never badged in.

**Date ranges crossing months** fan out across N monthly partitions and merge in
the Function. Fine for the report windows you'd actually run; if you ever want
open-ended analytics ("everyone in the building between 2–3pm last quarter"),
that's the point to export rather than contort the schema — see retention below.

**Retention.** Table Storage has no TTL. A timer-triggered Function archives
partitions older than N months to Blob as JSON/CSV and deletes them. That archive
doubles as the analytics escape hatch (queryable with Synapse serverless / Data
Explorer, or just downloaded).

> **Why not Azure SQL, given the reporting?** It would make these queries trivial.
> But doors sync every 30 s, so a serverless SQL instance would never auto-pause
> and would bill continuously — roughly $15–30/mo versus pennies. The query shapes
> here are a small, closed set, and the dual-write is ~15 lines. Cosmos DB
> serverless is the middle option if you later want ad-hoc query freedom; keeping
> all storage behind the Function layer means that swap stays contained.

**`POST /api/v1/sync`** — the single device endpoint.

```
→ { deviceId, board, fw, bootId, rosterRev, bootEpoch, events: [...] }
← { ackSeq, rosterRev, roster?: [...], schedule?: {},
    fw?: {board, version, url, sha256}, serverTime }
```

`roster` is sent only when the device's `rosterRev` is stale. A **single global
`rosterRev`** means any change resyncs every door — at 20 doors × ~15 KB that's
negligible, and it removes a whole class of per-door bookkeeping bugs.

Effective roster for a door = every credential whose **person** shares a group with
that door, filtered by both `person.active` and `credential.active`/validity dates.
Computed server-side; the ESP32 never evaluates group logic and never learns that
people or groups exist — it receives a flat list of credential hashes.

**`POST /api/v1/enroll`** — pairing. Admin UI issues a short-lived code; it's typed
into the device's `/setup` page; the device exchanges it for a long-lived key
stored in NVS. No per-device firmware, no baked-in secrets.

**Auth**: `x-device-key` header over TLS. Key stored hashed in `Doors`.

**Also move the unlock schedule server-side.** `UnlockSchedule` stays as the local
enforcement engine (its fail-secure-without-NTP behaviour is exactly right) but the
values arrive in the sync response instead of being set per door.

## Phase 4 — Device: the sync client ✅ CORE WORKING

> **Verified on hardware 2026-08-15**, firmware 2.3.2, both DevKits paired and
> syncing. Full path proven: pair → roster down → decision local → events up →
> acked → spool drained. Both doors at roster rev 3; `EventsByDoor` and
> `EventsByPerson` both populated, with personless events correctly appearing
> only in the door timeline and pre-NTP events resolved and flagged `approx`.
>
> **The permission model works end to end on real doors:** Front Door holds 5
> credentials, Test Door 1 holds 4, and Carl reaches only the front door —
> from group membership alone, with no per-door fob list anywhere.
>
> Trust anchors were *extracted and verified*, not recalled: the chain was
> pulled from the live endpoint with `openssl` and both anchors proved to
> validate it with the system trust store excluded. Sources and the generator
> are in `lib/CloudSync/certs/` and `tools/gen_certs.py`.
>
> **Central OTA verified 2026-08-16.** `jtc-test1` took 2.4.4 over the air with
> no intervention: offered, downloaded, SHA-256 verified, flashed, rebooted,
> roster and spool intact. `jtc-main` was held back throughout via `fwHold`.
>
> Two real bugs surfaced getting there, both worth remembering:
>
> 1. **The OTA ran while the sync's TLS context was still allocated.**
>    `HTTPClient::end()` closes the socket but does not free the ~45 KB mbedTLS
>    context — that lives until the `WiFiClientSecure` is destroyed. Calling
>    `applyFirmware()` from inside `syncOnce()` meant two contexts at once, which
>    does not fit. The symptom was a bare `HTTP -1` from a host the device had
>    been talking to a second earlier, which points at the network rather than at
>    heap. Fixed by queueing the approved offer and applying it from the task
>    loop, after `syncOnce()` has returned.
> 2. **The diagnostics were deleted by the code that followed them.** CloudSync
>    never logged at all, and the one field it did set (`lastError`) was cleared
>    by the task loop on success — and a firmware refusal *is* a successful sync.
>    So a door that declined an update looked identical to one never offered any.
>    Fixed with a logger callback and a separate `fwNote` that survives success.
>
> **Publishing rule learned the hard way:** the backend offers whenever the
> published version *differs*, not only when it is newer — deliberately, so a
> rollback is possible. The cost is that publishing an older version pulls the
> fleet back to it. Publish the version you want doors running.
>
> **Still outstanding in this phase:**
> - **The staleness path is proven only by construction.** No door has yet been
>   offline for six hours. Worth forcing once — block a device at the firewall
>   and confirm `/status` reports `[STALE]` while taps keep working from the
>   cached roster. That is the failure this entire design exists to survive, so
>   it deserves a real test rather than an argument.

**New `lib/CloudSync/`** — its own FreeRTOS task, never in the decision path.

- `WiFiClientSecure` with **embedded root CAs**: DigiCert Global Root G2 (what
  `*.azurewebsites.net` currently chains to), plus G3 and Microsoft RSA Root 2017
  for headroom. ⚠️ **This is the maintenance trap in the whole design** — a root
  rotation bricks sync fleet-wide. Mitigations: embed several, surface cert
  failures loudly on `/status` and `/webserial`, and treat OTA as the escape
  hatch (which is why OTA must never depend on sync succeeding).
- Poll every 30 s **with per-device jitter** so 20 doors don't stampede.
- **Stream the roster response** via ArduinoJson's stream overload rather than
  buffering the body — the TLS handshake alone wants ~40–50 KB of heap.
- Exponential backoff on failure; the cached roster stays authoritative
  indefinitely. Never fail-open, never fail-closed-to-everyone.
- **Staleness signal**: after N hours without a successful sync, flag it on
  `/status` and the OLED idle screen. A silently stale door is the dangerous
  failure mode — a revoked fob that still works and nobody knows.

**Central OTA**: compare the sync response's `fw.version` to `FW_VERSION`, pull
from Blob Storage, verify sha256 before commit.

*Partly de-risked already:* on 2026-08-13 a ceiling-mounted DevKit was upgraded
to the Phase 1 build entirely over ElegantOTA and came back healthy, so the
manual OTA path — image size against the `min_spiffs` slot (1.20 MB of 1.875 MB,
64%), reboot, reconnect — is proven on real hardware. What Phase 4 adds on top is
only the *automation*: version comparison, the pull, and the sha256 gate. Note
that a unit which cannot be reached physically has no recovery path if an image
fails to boot (Arduino-ESP32 does not enable automatic OTA rollback by default),
which is why staged rollout and the per-board hard gate below matter. **Never update while the relay is
energised or a schedule-unlock window is active** — check `relayOffAt` and
`gSchedActive` in [main.cpp:93-101](src/main.cpp#L93-L101). Stagger across the fleet.

> ⚠️ **Firmware is per board type, not per fleet.** Doors may be built on
> different ESP32 variants (C6, classic DevKit, C3, S3) — that is why
> `platformio.ini` carries a separate environment per board, and a C6 image
> flashed to a DevKit bricks the unit until someone walks over with a USB cable.
> So:
>
> - The device reports its **`board`** (the `BOARD_NAME` macro from
>   `BoardConfig.h`, now on `/status`) in the sync request, alongside `fw`.
> - Blob Storage holds one binary **per board per version**, and the `Meta`
>   table tracks the rolled-out version per board — not one global version.
> - The Function returns a `fw` block only when it has an image matching *that
>   device's* board. No match means no update offered, never a fallback image.
> - The device makes this a hard gate: it refuses a payload whose declared board
>   doesn't equal its own compiled-in `BOARD_NAME`, so a server-side mistake
>   can't brick a door. Belt and braces, because the failure is unrecoverable
>   remotely.
>
> The admin UI's Doors page should therefore show board type per door, and the
> rollout screen should be per board.

## Phase 5 — Admin UI 🟡 READ SIDE LIVE

Live at **`https://access.jtcustomtrailers.com`** (Preact + Vite on Static Web
Apps, Free tier). Signed in with Entra ID; dashboard, doors, people and reports
all render real fleet data.

**Done:** sign-in and role separation, dashboard, door list with sync health,
people, all four reports, custom domain with certificate, branding.
**Remaining:** every **write**. There are no forms yet — enrolling a fob, adding
a person, editing groups or pushing door config still happen through `seed.json`
and the CLI tools. Until those exist, the on-device read-only lockout below must
stay unenforced, or a paired door could not be administered at all.

> **The auth design changed during implementation.** The original plan here was
> SWA's built-in Entra auth with the Function App as a *linked backend* — no auth
> code at all. That was built, and then found to be **exploitable**: a linked
> backend trusts an `x-ms-client-principal` header injected by SWA, but the
> Function App has its own public hostname, so anyone who knew the URL could send
> that header and become Admin. One `curl` demonstrated it.
>
> It now uses MSAL in the browser and cryptographic token verification in the
> API. The cost is real — a build step, CORS configuration, and roughly 150 lines
> of auth code that the linked-backend design would not have needed. The benefit
> is that authorization no longer depends on requests arriving by a particular
> route. It also keeps SWA on **Free**, since linked backends need Standard.
>
> The general lesson, worth carrying into Phase 7: *"the platform handles auth"*
> is only true while the platform is the only way in.

This is **the** place doors are configured. Anything currently set per-unit —
door name, site, groups, unlock schedule, relay hold time, result-screen hold —
moves into the `Doors` row and is pushed down in the sync response as a `config`
block. Editing a door in the web app is the normal path; walking to it is not.

**Pages:**

- **People** — CRUD, group assignment, and the fobs each person carries.
- **Fobs** — enroll, label, assign to a person, deactivate. Deactivating a lost
  fob revokes it fleet-wide within one poll without touching the person.
- **Groups** and **Doors** (name, site, groups, schedule, config, last-seen,
  firmware version, roster rev, sync health).
- **Reports** ⭐ — the primary surface:
  - *Person across the fleet* — "where has Alice been, over this date range",
    every door, newest first. The headline query.
  - *Person at one door* — the same view filtered to one `deviceId`.
  - *Door* — everyone through door A.
  - *Unattributed exits* ⭐ — **exit-button releases with no preceding grant at
    that door**, within a configurable window. The exit button opens the door
    with no record of who, by design, so this is the report that surfaces the
    gap: someone leaving who never badged in. Expect legitimate hits (a visitor
    let in by hand, someone following another person in), which is the point —
    it turns an invisible blind spot into a reviewable list. Once a door-position
    sensor exists (see Phase 6) this sharpens considerably, because a *forced*
    door becomes distinguishable from a legitimate exit.
  - Each filterable by granted/denied and exportable to CSV.
- **Unknown taps → one-click enroll** — the central version of today's
  `lastUnknownUid` flow ([AccessControl.cpp:100](lib/AccessControl/AccessControl.cpp#L100)); pick the card, attach it to a
  new or existing person, done.
- **Fleet health** — every door's last-seen, so a unit that stopped syncing is
  visible at a glance rather than discovered during an incident.

### On-device pages go read-only once paired

`/status`, `/webserial`, and `/update` stay exactly as they are — diagnostics and
OTA must keep working when the cloud doesn't. But **`/config`'s write endpoints
are disabled while a door is paired**: `/api/add`, `/api/rename`, `/api/remove`,
and `POST /api/schedule` in [WebHandlers.cpp](lib/WebHandlers/WebHandlers.cpp) return `409 Conflict` with a
pointer to the web app. The read endpoints (`/api/list`, `/api/taps`,
`GET /api/schedule`) stay live, so a door still explains itself locally.

The reasoning: a controller that can self-enroll fobs while offline is an attack
surface — anyone who reaches one door's page could add themselves, and the central
DB wouldn't know until it silently overwrote the entry. It also removes any
possibility of local/central drift, since the roster then has exactly one writer.
The accepted cost is that **you cannot enroll a fob during a WAN outage**; the
doors keep working on their cached rosters, but the fob list is frozen until the
link returns.

Unpaired devices keep full local CRUD, so a bench unit or a not-yet-paired door
behaves exactly as today.

Add sync state to the existing `setStatusProvider` block
([main.cpp:396](src/main.cpp#L396)): last sync, roster rev, spool depth, pairing state.

## Phase 6 — Door position sensing (hardware-gated)

**Independent of Phases 3–5** and can land whenever the hardware exists; it needs
no backend. Listed last only because it waits on parts.

Today the firmware knows a *relay fired* — not that a door opened. Three states
are invisible: a release nobody walks through, a door propped open after a
legitimate release, and a door forced with no release at all. The exit button
makes this sharper, because it releases the door with no record of who.

**Hardware:** a reed contact (magnetic door-position switch) on the frame, one
GPIO to ground with an internal pull-up — the same wiring pattern as the exit
button. The classic ESP32 DevKit has spare pins; **the XIAO C6 does not** (Phase 1
notes every pad is already in use), so C6 doors need an I/O expander or a board
revision. That constraint should drive the next PCB spin.

**Firmware:**

- Two new `EventLog::Type` values, `EVT_DOOR_FORCED` and `EVT_DOOR_HELD`. The
  enum is append-only by design, so the 40-byte record format does not change
  and older spool files stay readable.
- **Door forced** — contact opens with no grant and no exit press inside a short
  window. This is the genuine security event.
- **Door held open** — still open N seconds after the relay dropped. Usually
  operational (a delivery, a propped door) rather than malicious, but it is what
  makes a door-forced signal trustworthy by ruling out the benign case.
- Both are local decisions on local state, so they work with no network — the
  same rule as every other decision in this design.

**Reporting:** door-forced belongs on the fleet-health surface, not buried in an
event list. It also sharpens the *unattributed exits* report in Phase 5: with a
contact fitted, a forced entry becomes distinguishable from a legitimate exit.

## Phase 7 — Multi-customer: one deployment per customer tenant

**Decided 2026-08-16.** This is a product, and every deployment lives in the
*customer's own Entra tenant and subscription* — not a shared instance, and not
even separate resource groups in one tenant.

### Why isolation is physical, not logical

A cross-customer leak in an access-control system means someone opening another
company's doors. With a deployment per tenant, that is not a bug you can write:
the other customer's data is in a different subscription, behind a different
identity boundary. The alternative — one instance with `WHERE tenantId = …` in
every query — has to be correct in every query, forever, and a single omission is
catastrophic and silent.

Three things fall out for free:

- **Identity.** Each customer's staff already sign in with their own work
  accounts. A shared instance would need a multi-tenant app registration and
  cross-tenant consent; per-tenant needs neither.
- **Billing and data residency** are the customer's, which is usually what they
  want to hear.
- **Blast radius.** A bad firmware publish, a bad roster, a bad deploy affects
  one customer.

Cost is not the obstacle: the whole footprint sits inside free tiers (Flex
Consumption grant, SWA Free, a few cents of Table Storage), so customer number
two costs roughly what customer number one does.

### ⚠️ The firmware consequence

`CLOUD_HOST` is a `#define` in `src/main.cpp`. One backend per customer means
either a firmware build per customer — which multiplies the per-board OTA matrix
by the customer count and makes images non-interchangeable — or making the host
**runtime configuration**.

It must become runtime configuration:

- Store it in `DeviceSettings` alongside the device key, set at `/setup` or
  returned by `/api/v1/enroll` during pairing.
- Then one image serves every customer, and `publish-fw` stays per board rather
  than per board *per customer*.
- The TLS anchors need no change while every backend is `*.azurewebsites.net`;
  a customer wanting a custom domain on the *API* would need its chain checked
  against `lib/CloudSync/certs/`.

Do this **before** the second customer exists. Retrofitting it means reflashing
the first one.

### Provisioning

The Bicep is already parameterised (`baseName`, `storageName`), so this is mostly
a script that takes a customer name and stands up, in their tenant:

1. Resource group, storage, tables, Function App, SWA, Application Insights.
2. Entra app registration with the three app roles, plus the three security
   groups, plus the role assignments.
3. `.env.local` and `branding.js` for their bundle; build and deploy the web app.
4. Custom domain and its CNAME, then redirect URIs, CORS and CSP for that origin
   — all four together, or sign-in half-works in a way that is hard to diagnose.
5. First firmware publish per board type they use.

Turns "onboard a customer" from a two-hour checklist into a command, and the
checklist is where the mistakes live. Roughly a day's work.

### What this settles about branding

Per-deployment builds mean **build-time branding is already correct** — each
customer's bundle is built with their `branding.js` and palette. A runtime
branding admin page buys exactly one thing: letting the customer change it
without involving you. Worth having as self-service eventually; not
architecture, and explicitly not a prerequisite.

---

## Files

**New (device)** — all written project-agnostic; `DeviceIdentity`, `EventLog`, and
`CloudSync` are reusable in FilamentTagReader, which already shares
`DeviceSettings`/`WebService`/`WiFiManager`/`Display`:

```
lib/DeviceIdentity/    MAC-derived id + door/site naming
lib/Roster/            LittleFS roster, hashed creds, atomic swap
lib/EventLog/          persistent spool + drain cursor
lib/CloudSync/         TLS sync task, enrollment, OTA trigger (incl. root CAs)
partitions_rfid.csv
```

**Modified (device)**:

```
lib/AccessControl/*    decision API kept; storage delegated to Roster
lib/Events/Events.h    new event types + reason codes
lib/WebHandlers/*      pairing UI, sync status; write endpoints 409 when paired
lib/WebHandlers/HtmlPages.h
src/main.cpp           runtime hostname, /setup, sync task, status lines
platformio.ini         LittleFS, FW_VERSION, partitions
```

**New (cloud)**:

```
cloud/shared/types.ts   sync contract + entity/report types, imported by both
cloud/api/              Functions (TypeScript) + Table Storage access
cloud/web/              Static Web App: people, fobs, groups, doors, reports
cloud/infra/            Bicep: resource group, storage, function app, SWA
```

Note the README's own constraint: PlatformIO compiles each `lib/` in isolation and
libraries **cannot** see `src/`. Anything shared between a new lib and `main.cpp`
must live in a lib, the way `Events/` already does.

---

## Verification

Bench with two units — the **XIAO C6** and the **classic ESP32 DevKit**, whose
pin maps the README marks as verified (the C3/S3 maps are unverified defaults, so
don't debug this on those).

1. **Distinct identity** — flash the *same binary* to both boards with no config.
   Confirm they come up as two different `rfid-<mac>.local` names, both resolve
   independently, and renaming one at `/setup` survives reboot. This is the
   regression test for the collision that blocks multi-door today.
2. **Fleet sync** — pair both. Enroll a fob centrally → confirm it opens both doors
   within one poll. Deactivate it → confirm it's denied at both within one poll.
3. **Group scoping** — put door A in a group the person lacks. Confirm the fob is
   granted at B and denied at A, and that A's roster genuinely never contains it.
4. **Person-centric report** ⭐ — tap the same person's fob at both doors, then run
   the "any door" report. Confirm both reads appear, newest first, with the correct
   door names — served from a single `EventsByPerson` partition, not a fan-out.
   Then run the person-at-one-door report and confirm it's a subset.
5. **Two fobs, one person** — give a person a card *and* a keyfob, tap each at a
   different door. Confirm the person report shows both, attributed to one person.
6. **History doesn't rewrite** — after logging taps for Alice, reassign that fob to
   Bob. Confirm the historical rows still read Alice and only new taps read Bob.
7. **Dual-write consistency** — after a burst of taps, confirm `EventsByPerson` and
   `EventsByDoor` hold the same event count for that window.
8. **Central is the only writer** — change a door's unlock schedule and relay hold
   time in the web app only; confirm the device picks both up on the next sync and
   enforces them with no local interaction. Then confirm the paired device's
   `/api/add`, `/api/rename`, `/api/remove`, and `POST /api/schedule` all return
   `409`, while `/api/list`, `/api/taps`, `/status`, and `/update` still work — and
   that an *unpaired* board still has full local CRUD.
9. **Offline** ⭐ *the critical one* — pull the WAN (or power the router down) with
   the doors up. Confirm: known fobs still grant, unknown still deny, exit button
   works, schedule still fires, events spool. Restore the link → confirm every event
   drains, in order, with correct doorId, **no duplicates and no loss**, and that
   the drained events land in *both* event tables.
10. **Power-cut durability** — yank power mid-spool. Confirm spooled events survive
    and `bootId` increments so sequence numbers can't collide with the previous boot.
11. **Clock resolution** — boot with NTP unreachable, tap several fobs, then restore
    NTP. Confirm the spooled events resolve to correct absolute times, are flagged
    `approx`, and land in the right monthly partition.
12. **Idempotency** — replay a batch by hand (curl the same body twice). Confirm the
    row count is unchanged in both event tables.
13. **Regressions on the existing safety properties** — re-run the README's relay
    checks: meter the relay pin through several power cycles for spurious pulses, and
    confirm fail-secure still holds through a reboot. The partition-table change and
    the new boot-time filesystem mount both land before `setup()`'s relay-safing code
    at [main.cpp:326](src/main.cpp#L326) — **verify that line still runs first**.
14. **OTA safety** — trigger a central rollout while a schedule-unlock window is
    active. Confirm the device defers the update until the door relocks.
15. **Per-board OTA** ⭐ — with a C6 door and a DevKit door both paired, publish a
    C6-only build. Confirm the C6 updates and the DevKit is offered *nothing*
    (not a fallback image). Then deliberately mis-tag a DevKit image as C6 and
    confirm the device refuses it on the board check rather than flashing it —
    this failure is unrecoverable without physically reaching the door, so the
    device-side gate has to hold even when the server is wrong.
16. **Cert failure** — point the device at a host with a bad chain. Confirm it fails
    closed *on sync only*, keeps granting on the cached roster, and says so loudly on
    `/status`.

### 🔧 TODO: expose free heap on `/status`

`/status` and `/status.txt` report filesystem, roster, spool and reader health but
**not free heap**. That is the one number that would show a slow leak, and the
place a leak is most likely is repeated *failed* TLS handshakes — a door that has
been offline for hours retrying, which is exactly the state nobody is watching.

Discovered during the staleness test of 2026-08-16: six hours of failed syncs
could be observed in every respect *except* the one that would reveal a leak. The
fallback signal is the boot counter, which only catches a leak large enough to be
fatal within the test window — a slower one would look like a perfectly healthy
run and then kill a door weeks later, in the field, with no diagnostic trail.

Add to the status block: current free heap, **minimum free heap since boot**
(`esp_get_minimum_free_heap_size()`), and largest free block. The minimum-since-
boot figure matters most — it survives the transient spike that caused the
trouble, so a door can be interrogated after the fact rather than needing to be
caught in the act. Cheap: three numbers, no new state.

While in there, the same case applies to `CloudSync`'s consecutive-failure count
and current backoff interval — both exist in memory already and neither is
visible, so "is this door backing off correctly or hammering?" currently needs a
six-hour sampling harness to answer. That is far too much work for a question the
device could simply answer.

### Test log

**#9 Offline** — in progress, started `2026-08-16T20:06:02Z`. Test Door 1
(`rfid-275044`) firewalled outbound, LAN kept reachable so internals stay
observable; Front Door left online as a control. Predictions recorded **before**
observation (backoff settling at the 15-minute cap → ~28 failures at +6h, versus
~720 if it hammered at 30s), so the result can falsify rather than merely
illustrate. Cloud-side detection already confirmed: flagged `NOT CHECKING IN` at
T0+11m, as designed.

Note that the spool's **overflow** path is not reached by this test — 5000 records
needs 5000 taps. That path drops the oldest event and is the only one that
silently loses history, so it needs a separate test with a temporarily lowered cap
rather than being taken on trust.
