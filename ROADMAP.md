# Multi-Door RFID Access: Central Sync + Azure Backend

## Context

`RFID_Access` today is a single-door controller. Three things make it single-door
by construction:

1. **Identity is compile-time.** `MDNS_HOSTNAME` is a `#define` in
   [src/main.cpp:68](../../../../PlatformIO/Projects/RFID_Access/src/main.cpp#L68), so every door needs its own firmware build.
2. **The roster is local and tiny.** `Entry allowList[30]` in
   [lib/AccessControl/AccessControl.h:13](../../../../PlatformIO/Projects/RFID_Access/lib/AccessControl/AccessControl.h#L13), persisted as a JSON blob in NVS.
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
| API language | TypeScript / Node 20 — shares types with the SWA frontend |
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
  [lib/DeviceSettings/DeviceSettings.h:63](../../../../PlatformIO/Projects/RFID_Access/lib/DeviceSettings/DeviceSettings.h#L63) — no new store).
- **Delete `MDNS_HOSTNAME`.** ⚠️ *Blocking today:* every unit currently claims
  `rfid-door.local`, so the second door onward collides — whichever responds first
  wins, intermittently, and you can't reliably reach a specific unit. Hostname
  becomes `DeviceSettings::hostname()` (`KEY_HOSTNAME` already exists), defaulting
  to `rfid-<mac>` so **two units are never identical out of the box**, and
  renameable to `rfid-front`/`rfid-shop` at `/setup`. Fix this first — it's a
  prerequisite for bench-testing anything else with two boards.
- **Enable the existing `/setup` page.** `WebService::enableSetup()` and
  `setSetupFieldsProvider()` are already built ([WebService.h:100](../../../../PlatformIO/Projects/RFID_Access/lib/WebService/WebService.h#L100)) but
  RFID_Access never calls them. Wire them up and inject door name, site, and the
  cloud pairing field (Phase 4).

## Phase 2 — Device: scalable roster + persistent event spool

**New `lib/Roster/`** replaces the fixed array behind `AccessControl`'s existing API.

- LittleFS-backed binary store; RAM index for O(log n) lookup.
- **Credentials stored as a salted SHA-256 truncated to 8 bytes**, names in clear.
  A door only ever holds hashes, so a stolen controller doesn't yield a clonable
  card list. Costs nothing — mbedTLS is already linked for TLS. Unknown-card
  enrollment still works: the raw number exists transiently at tap time, is shown
  in the UI, and is uploaded with the deny event so the central UI can enroll it.
- **Atomic swap on sync**: build the new roster fully, then swap the active
  pointer under `dataMutex` ([AccessControl.h:20](../../../../PlatformIO/Projects/RFID_Access/lib/AccessControl/AccessControl.h#L20)). Never mutate in place —
  a tap arriving mid-sync must see either the old list or the new one.
- Tracks `rosterRev` so sync can skip unchanged payloads.

**New `lib/EventLog/`** — append-only spool on LittleFS, ~40 B/record.

- **Identity**: `bootId` (one NVS write per boot) + per-boot `idx`. The tuple
  `(deviceId, bootId, idx)` is the server-side idempotency key — retries are free,
  no per-event NVS writes.
- **Timestamps before NTP**: every record stores `uptimeMs`, plus `epoch` if the
  clock is valid. At upload, unresolved records are converted using the batch's
  `bootEpoch` and flagged `approx`. This matters because `UnlockSchedule::timeValid()`
  ([UnlockSchedule.h:36](../../../../PlatformIO/Projects/RFID_Access/lib/UnlockSchedule/UnlockSchedule.h#L36)) is often false for the first seconds after boot.
- Records: tap (with raw credential + reason code), exit-button, schedule
  transition, boot, config change, sync failure.
- Read cursor persisted; records retained until the server acks. Ring-overwrites
  oldest when full.

**Rewire `AccessControl::acProcessEvent`** ([AccessControl.cpp:87](../../../../PlatformIO/Projects/RFID_Access/lib/AccessControl/AccessControl.cpp#L87)) to query
`Roster` and append to `EventLog`. Keep `tapLog` as-is — it backs the existing
`/api/taps` handler and the `/status` "Last tap" line, and is cheap.

**`platformio.ini`**: add LittleFS, `FW_VERSION`, and a custom partition table.
`min_spiffs.csv` leaves only 128 KB of filesystem, which the roster and spool
have to share.

*Measured after Phase 1:* the C6 image is **1.21 MB of the 1.875 MB slot (61.7%)**.
TLS, LittleFS, and the sync client should land it near ~1.5 MB. That rules out the
~1.6 MB slots first sketched here — 1.5 MB into 1.6 MB is ~91% full, too little
margin for an OTA target. Use **1.75 MB slots + ~384 KB filesystem** instead:
still ~83% headroom, and 384 KB is far more than needed (roster ≈ 12 KB at 300
credentials, leaving room for ~9,000 spooled events). Re-check with
`pio run -t size` once TLS is actually linked.

## Phase 3 — Backend: Azure Functions + Table Storage

New `cloud/` tree, outside the PlatformIO project. **TypeScript / Node 20**
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

## Phase 4 — Device: the sync client

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
`gSchedActive` in [main.cpp:93-101](../../../../PlatformIO/Projects/RFID_Access/src/main.cpp#L93-L101). Stagger across the fleet.

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

## Phase 5 — Admin UI

Azure Static Web Apps (free tier) with built-in Entra ID auth — your tenant gives
SSO with zero auth code — and the Function App linked as its managed API.

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
  - Each filterable by granted/denied and exportable to CSV.
- **Unknown taps → one-click enroll** — the central version of today's
  `lastUnknownUid` flow ([AccessControl.cpp:100](../../../../PlatformIO/Projects/RFID_Access/lib/AccessControl/AccessControl.cpp#L100)); pick the card, attach it to a
  new or existing person, done.
- **Fleet health** — every door's last-seen, so a unit that stopped syncing is
  visible at a glance rather than discovered during an incident.

### On-device pages go read-only once paired

`/status`, `/webserial`, and `/update` stay exactly as they are — diagnostics and
OTA must keep working when the cloud doesn't. But **`/config`'s write endpoints
are disabled while a door is paired**: `/api/add`, `/api/rename`, `/api/remove`,
and `POST /api/schedule` in [WebHandlers.cpp](../../../../PlatformIO/Projects/RFID_Access/lib/WebHandlers/WebHandlers.cpp) return `409 Conflict` with a
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
([main.cpp:396](../../../../PlatformIO/Projects/RFID_Access/src/main.cpp#L396)): last sync, roster rev, spool depth, pairing state.

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
    at [main.cpp:326](../../../../PlatformIO/Projects/RFID_Access/src/main.cpp#L326) — **verify that line still runs first**.
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
