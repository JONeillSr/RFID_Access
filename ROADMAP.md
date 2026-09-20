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

## Phase 5 — Admin UI ✅ READ AND WRITE LIVE

Live at **`https://access.jtcustomtrailers.com`** (Preact + Vite on Static Web
Apps, Free tier). Signed in with Entra ID; dashboard, doors, people, groups and
reports all render real fleet data, and the write surface is in.

**Done:** sign-in and role separation, dashboard, door list with sync health,
people, all four reports, custom domain with certificate, branding — plus:

- **Enrol from unknown taps** — click a card on the dashboard and attach it to a
  new or existing person. The number is carried across untouched, which removes
  the most error-prone step in administering this system: retyping ten digits.
  A mistyped-but-valid number produces a fob that silently opens nothing.
- **People** — add, edit, group membership, active toggle, delete (Admin).
- **Fobs** — label, assign, deactivate, delete.
- **Groups** (Admin) — showing what each one actually connects, because a group
  with no doors opens nothing and that is invisible if you only list names.
- **Doors** (Admin) — name, site, groups, relay/result hold, firmware hold, and
  a "who gets in?" roster preview answering *why can't X open this door*.

Dialogs state the consequence before you commit: deactivating a fob says which
doors are **not checking in** and will therefore keep accepting it. That gap is
the system's sharpest edge and the UI now names it at the moment it matters.

**Remaining:** the on-device read-only lockout described below is still not
enforced — `/api/add` and friends stay live on paired doors. That was blocked on
the write UI existing; it now does, so this is the next piece.

`seed.json` remains useful for bulk import, but is no longer the only way in.

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

## Phase 5b — Access follows the Entra account

Disabling someone in Entra revokes their fobs on the next sweep, so offboarding
is one action in the place it already happens. This closes the commonest
access-control failure — not a forced door, but someone leaving and their fob
continuing to work because nobody told the door system.

The delivery half needed nothing new: `effectiveRoster()` already skips inactive
people, so `active = false` reaches every door on its next sync.

**Membership is stated, never inferred.** `managedBy` is an explicit choice on
the person (`entra` or `manual`). Deriving it from "is there an object id?" would
make a contractor who correctly has no account indistinguishable from an employee
somebody forgot to link — and only the second is a hole. Anyone marked as
Entra-governed with no link is reported prominently and never revoked: locking
someone out over a data-entry omission is the wrong failure.

Linked by **object id, not email**. UPNs change with marriages and rebrands; a
link that silently breaks is a revocation that silently stops happening.

### Three properties worth preserving

**It only ever revokes.** Never reactivates, even when the account comes back.
Restoring building access keeps a human's name against it, and a one-way job
cannot silently undo a fob pulled by hand for being lost.

**It never revokes on doubt.** Network failure, 403, a missing `accountEnabled`
field — all leave access untouched. Only a definite *disabled* or *deleted* acts,
and the 404 that means "deleted" is only reachable when the request itself
succeeded, so a blip cannot be mistaken for a deleted account.

**It fails open, loudly.** Failing closed would turn a Graph outage into a
building nobody can enter. The cost is that silence resembles success, so the
time since the last **clean** run is surfaced and a partial sweep does not reset
that clock — the same distinction as the device's NTP line. A sweep switched off
goes stale exactly like a broken one.

### The interval is data, not a CRON

A timer's schedule is fixed at deploy and can only be varied through an app
setting — which is precisely what a Bicep deployment overwrites wholesale. An
interval configured that way would silently revert on the next unrelated deploy.
So the timer is a fixed 5-minute heartbeat, the interval lives in the table and
is edited in the admin app, and the staleness threshold derives from it rather
than being hardcoded.

### Known consequence

Whoever administers this system is usually also governed by it. Both door access
*and* the Admin role come from Entra, so an admin whose account is disabled loses
the building and the ability to undo it in the same moment. Recovery is
`npm run seed` or the table directly. Link the sole admin **last**, after
watching it work on someone else.

Setup — granting the Function App's identity `User.Read.All` — is in
`cloud/infra/README.md`. Until consent exists the sweep fails open and says so.

## Phase 6 — Door position sensing

**Independent of Phases 3–5.** Detection is entirely on the door; showing it
needs backend and web work (below).

Today the firmware knows a *relay fired* — not that a door opened. Three states
are invisible: a release nobody walks through, a door propped open after a
legitimate release, and a door forced with no release at all. The exit button
makes this sharper, because it releases the door with no record of who.

**Status (2026-09-17):** firmware **2.8.0** is on Test Door 1 (Front Door held at
2.7.3). Backend and web are deployed. **No contact is fitted yet, so detection is
unverified on hardware.** The rules pass 12 scenarios in a line-for-line
JavaScript copy, but the C++ has only been compiled, not run. Next: bench test
with a jumper from GPIO 33 to GND standing in for a closed door.

### Hardware

A reed contact (magnetic door-position switch) on the frame, one GPIO to ground
with an internal pull-up — the same wiring pattern as the exit button.

- **The doors in service are ESP32 DevKit V1, which has spare GPIOs**, so they
  can take a contact now. The earlier worry that there was no pin applied to the
  XIAO C6, which is not what was deployed.
  - DevKit V1: **GPIO 33**, beside the exit button on GPIO 32.
  - ESP32-S3: GPIO 17.
  - **XIAO C6 and ESP32-C3: no free pin.** Every usable GPIO is already
    assigned. They need an I/O expander or a board revision, and that should
    drive the next PCB spin.
- **Use a contact that is CLOSED when the door is shut** (the usual kind for
  access control). A cut or disconnected wire then reads as *open*, so tampering
  with the contact raises "door forced" rather than silently reading "closed".
- **Free mechanical egress will cause false "forced" alerts.** If the inside
  lever opens the door without anyone touching the exit button — common, and
  often required by fire code — the controller sees the door open with no
  release. Before trusting "forced" on such a door, fit a request-to-exit input
  that fires on egress: a REX switch in the lever, or a motion REX sensor. Either
  can share the exit button's input. Confirm how each door actually behaves before
  enabling the contact.

### Firmware

- **Off until enabled on `/setup`.** With nothing wired, a pulled-up input reads
  "open", which would raise a forced alert at once. Enabling is an install-time
  fact about the hardware, so it stays local to the door rather than coming from
  the cloud.
- Uses the two `EventLog::Type` values reserved for this, `EVT_DOOR_FORCED` (8)
  and `EVT_DOOR_HELD` (9). The enum is append-only, so the 40-byte record format
  does not change and older spool files stay readable.
- **Door forced:** the contact opens while no release is in effect — no grant or
  exit press within the relay hold plus a short grace, and no scheduled unlock
  window open. This is the genuine security event. It asks for an immediate sync,
  at most once a minute.
- **Door held open:** open for longer than the configured time (default 60 s)
  **since the last release ended**. So a door propped during a delivery is timed
  from when the relay dropped, and a forced door from when it opened. A
  scheduled unlock window suspends it. When a held door finally closes, a second
  `EVT_DOOR_HELD` records how long it was open.
- **A door already open at boot is not "forced":** no opening was seen. It can
  still be held open.
- The decision logic is a small state machine with no hardware in it
  (`lib/DoorContact`), fed by the main loop. Both decisions are local, so they
  work with no network — the same rule as every other decision in this design.

### Backend and web

Deployed 2026-09-17:

- `DoorForced` (8) and `DoorHeld` (9) in `EventType`, with reasons `NoRelease`,
  `HeldOpen` and `Closed`. Both are **personless**, so they are filed under the
  door only — never under the person table's `unknown` partition, which feeds
  the enrolment list.
- **Door forced is on the dashboard, not buried in an event list:** a tile for
  the last 7 days, and a list of each forced opening.
- Event labels: "DOOR FORCED OPEN", "DOOR HELD OPEN", "closed after being held
  open 95s". Config events show their detail, so switching the contact off on
  `/setup` is visible in the door's history.
- The Person column shows the detail field only for card taps. Firmware, config
  and door events carry a version, setting or duration there, not a card number.

Still to do: the *unattributed exits* report in Phase 5 gets sharper once a
contact is fitted, because a forced entry becomes distinguishable from a
legitimate exit.

### Open gap: an offline door cannot raise the alarm

A forced door is detected and recorded offline, but nobody hears about it until
the door syncs again. That is the same "nobody is told" gap as the Aug 30 outage,
and it matters more for this event than for any other. A local sounder at the
door, and alerting that does not depend on someone opening the dashboard, are
both worth considering before relying on this.

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

### ✅ The firmware consequence — resolved

The backend host is **runtime configuration**, stored in `DeviceSettings` beside
the device key and set on `/setup`. `CLOUD_HOST_DEFAULT` in `src/main.cpp` is now
only the value a door uses until it is told otherwise, so existing units carried
across the change with nothing to migrate.

One image therefore serves every customer, and `publish-fw` stays per board
rather than per board *per customer*.

Two details that matter more than the storage:

- **Changing the host unpairs the door.** A device key is issued *by* a backend
  and is meaningless to another. Keeping it would leave a door looking paired
  while every sync is rejected — so `setHost()` clears it, which is the honest
  state and the one `/setup` already knows how to resolve.
- **The host is applied before any pairing code in the same submission.** A code
  is redeemed against the deployment that issued it; pairing first would send the
  new customer's code to the old customer's backend while the form on screen
  plainly shows the right host.

`/status` now reports `Backend:` as well, because with one image across all
customers, "who is this door reporting to?" is no longer answerable from the
firmware version and has to be asked of the device.

TLS anchors needed no change: every deployment is `*.azurewebsites.net` and
chains to a root already in `lib/CloudSync/certs/`. A customer wanting a custom
domain on the **API** — as opposed to the admin app, which the device never
contacts — would need that chain re-checked.

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

## Phase 8 — Reader interoperability: Wiegand now, OSDP and encrypted credentials next

**Added 2026-09-18**, prompted by a customer prospect. Paxton P-series on Clock &
Data is the only reader verified end to end, and it stays the reader we sell as
proven. This phase is how the list grows without over-promising.

### Step 1 — Wiegand as a per-door setting ✅ CODE DONE, ⏳ UNTESTED ON HARDWARE

The line format was a constructor argument in `src/main.cpp`, which with one
image for every customer meant a Wiegand site needed a custom build. It is now
**Reader format** (NVS key `readerMode`), applied in `setup()` before
`paxton.begin()` because the mode decides which ISRs attach. A change is saved
immediately, **applied on reboot**, shown as "reboot pending" until then, and
written to the event spool as a config event (`reader=wiegand` / `reader=cnd`) —
the wrong format silently stops every fob at that door, and `/setup` has no
login. `/status` names the running format.

**Set centrally, like everything else a paired door obeys.** It is part of the
cloud door config (`DoorConfig.readerMode`, `'cnd'` or `'wiegand'`), edited in
the admin app under Doors. `/setup` shows it read-only while a door is paired
and remains the way to set it on an unpaired door. The door reports the format
it is RUNNING with every sync, so the admin app distinguishes a change that is
pending from one that has been applied, and the backend rejects any other value
rather than passing it to a door that cannot argue back.

**This uncovered a real gap:** the backend had been sending `config` on every
sync since Phase 3 and **the device never read it**. Relay hold, result-screen
hold and the unlock schedule were authored in the admin app, pushed to the door,
and silently dropped — while the door's own API refused local edits with "make
the change in the admin app". A paired door's schedule could not be changed from
anywhere. The device now applies the whole block (2.8.1), clamping each value,
comparing against what is already in effect so nothing is rewritten every 30
seconds, and recording one config event when something actually changes.

Still open before "Wiegand readers supported" goes on anything customer-facing:

- [ ] Bench-qualify one mainstream third-party reader end to end with
  `docs/reader-qualification.md`.
- [ ] Decode **34-bit** (even/odd parity halves) and **HID Corporate 1000
  35-bit** properly. Today only 26-bit is parity-checked; every other length is
  accepted as a raw number with no validation, so a noisy frame can enrol as a
  different "card".
- [ ] Reader feedback for single-LED / beeper readers. The LED calls assume
  Paxton's separate red/amber/green lines.
- [x] Decide whether the format belongs in the cloud door config rather than on
  local `/setup`, consistent with "device pages read-only once paired".
  **Decided 2026-09-18: the cloud owns it when paired.**
- [ ] Prove the pending/applied round trip on Test Door 1: set the format in the
  admin app, confirm the door shows "reboot pending" and the admin app agrees,
  reboot, confirm both say applied. Safe to do with a Paxton attached by setting
  it back to Clock & Data before rebooting.

### Step 2 — OSDP (Open Supervised Device Protocol)

Wiegand is one-way, unsupervised and unencrypted: anyone with access to the
cable can read card numbers off it or replay them, and a cut or swapped reader
looks exactly like a quiet one. OSDP (SIA standard, IEC 60839-11-5) replaces it
with bidirectional RS-485, reader supervision (offline and tamper are reported),
and **Secure Channel** (AES-128) between reader and controller. It is what
security-led buyers increasingly ask for, and it fits the product's Zero Trust
story better than anything else on this list.

Hardware (feeds **PCB Rev 2**):

- 3.3 V half-duplex RS-485 transceiver (MAX3485 / SP3485 class), a 120 Ω
  termination jumper, and a 4-pin terminal (12V, 0V, A, B).
- A UART plus a direction (DE/RE) pin. **DevKit / S3 only** — the C6 and C3 have
  no free pins, same constraint as the door contact.

Firmware:

- Evaluate **libosdp** (portable C, Control Panel mode, Secure Channel) on
  ESP-IDF before writing anything ourselves. Protocol code is not where this
  project should be original.
- The controller is the OSDP Control Panel; the reader is the Peripheral
  Device. The access decision stays local, exactly as today.
- Reader LED/buzzer control comes over OSDP, which removes the per-brand LED
  wiring problem from Step 1.
- Surface reader supervision: *reader offline* and *tamper* become events on the
  dashboard, like forced doors.

Key management — the part that needs design, not just code:

- Secure Channel keys (SCBK) are per reader. Readers ship with a well-known
  install-mode key; the controller must move each reader to a unique key at
  install, and **never leave a reader on the default key** — that is Secure
  Channel in name only.
- Where the per-reader key lives (device NVS vs cloud, and how a replacement
  controller gets it) must be decided before the first install.

### Step 3 — Encrypted credentials (DESFire EV2/EV3, iCLASS SE / Seos class)

125 kHz proximity cards — including the Paxton tokens in use today — carry no
cryptography and can be cloned with cheap handheld tools. The fix is a 13.56 MHz
smart-card credential whose secured data only a keyed reader can read.

- The **card** cryptography happens in the reader. The controller still receives
  a credential number; nothing in the roster model changes.
- **Pair this with OSDP, not Wiegand.** An encrypted card read out over plain
  Wiegand puts the number back on an unencrypted wire, which gives most of the
  security back.
- Card keys must be site-specific (a custom or customer-owned key), not the
  reader vendor's defaults.
- Migration is the real work: sites run old and new cards side by side for a
  while. The roster already keys on the credential string plus `cardType`, so
  one person can hold a prox fob and a smart card during the changeover.

### Order and gating

Step 1 hardware test → Step 2 on the Rev 2 board → Step 3 once OSDP Secure
Channel is proven. Marketing language moves only after each gate: today the slick
says Paxton only, and "most Wiegand readers, confirmed per site" is allowed only
once Step 1's reader test passes.

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

### Field failure: roster writes on a fragmented heap (2026-08-18)

Both doors stopped applying roster changes while reporting themselves healthy.
Every sync succeeded at the HTTP level; the device reported `roster write failed`
with ~130 KB free. Access changes made in the admin app silently never reached
the doors, and nothing on the dashboard said so — the doors were checking in.

`Roster::replaceAll` allocated a **second** full-capacity array,
`MAX_ENTRIES × 48 B = 24 KB contiguous`, to apply a five-fob roster of 240 bytes.
It asked for that while the live array of the same size was still held **and**
the sync's TLS context (~45 KB) was allocated — the most fragmented moment in the
device's life. Free heap was never the constraint: the largest contiguous block
was under 40 KB and falling during a sync.

Three things are worth carrying forward.

**The bug was latent for months** because most syncs send no roster at all — only
ones where the revision moved. It surfaces the moment access changes, which is
the worst possible time to find it. Rarely-exercised allocation paths deserve the
same suspicion as rarely-exercised error paths.

**`largest block` is what diagnosed it**, and that number only existed because of
the earlier offline test. Free heap looked fine throughout. Had `/status` shown
only free heap — the obvious thing to show — this would have been chased as a
network or backend fault.

**It could not self-heal.** A roster failure returned early, before the firmware
offer was parsed, and the queued OTA was gated on sync success. So a door that
could not apply a roster could never receive the update that fixed it: reboot or
cable only. *The door least able to sync is the one that most needs the new
image* — any early return on the path to an update offer deserves that question
asked of it.

Fixed in 2.7.2 (allocation) and 2.7.3 (deadlock). The write error now carries
free heap, largest block and entry count, so a recurrence names its own cause.

### Field failure: both doors dropped off Wi-Fi (2026-08-30, found 2026-09-16)

Front Door went silent at 19:35 UTC on Aug 30. Test Door 1 went silent on Aug 31,
recovered for about 100 minutes after rebooting on Sep 1, then went silent again
at 17:01 UTC. It went unnoticed for **17 days**. Access kept working from cached
rosters, and the roster was still at rev 18 on both sides, so no revocation was
left stranded.

The backend was healthy throughout. Every sync request that arrived returned 200
— they simply stopped arriving. TLS was checked against the rotated certificate
(Aug 29) on every axis testable from outside, and passed. The actual answer was
that **the doors were not on the network**: neither answered ARP on the subnet,
and Front Door was broadcasting the open `RFID-Setup` portal.

**Recovery:** both doors were rejoined through the portal to **the same network
with the same credentials** they had before, and came straight back (Test Door 1
boot #24, Front Door boot #18, both fw 2.7.3, rev 18). So the network had not
changed in a way the doors could not use — WPA3 and a router change were ruled
out by that, not confirmed.

#### Why a door stays in the portal: a one-way trap

This is the mechanism, and it does not need anything to be wrong with the network
for longer than a few seconds:

1. At boot, `WiFiManager::begin()` tries the saved network for **10 s**
   (`connectTimeoutMs`, left at its default by `wifiMgr("RFID-Setup")`).
2. If that fails, it calls `startAP()` and the door becomes the setup portal.
3. From then on `WiFiManager::loop()` returns immediately
   (`if (_state != STATE_STA) return;`). **Nothing ever retries the saved
   network.** Only someone joining the portal, or another reboot that happens to
   find the network up, gets the door back.

Any reboot that coincides with the network being unavailable for 10 s strands the
door indefinitely. A power cut that also takes down the access point, or a
router restart that overlaps a door rebooting, is enough.

**The trigger was most likely power, though that is not proven.** The evidence
that points that way:

- The events recorded while offline had **no clock**. A software restart normally
  keeps the ESP32's time; a power-on reset loses it (there is no battery-backed
  RTC).
- Front Door **rebooted twice while stuck** (boots 16 and 17 both logged offline),
  which a door idle in its portal should not do on its own.
- Test Door 1 rebooted at 15:01 and 15:20 UTC on Sep 1 (boots 21 and 22), synced
  for about 100 minutes, and went silent at 17:01. Its next boot (#23) logged
  events with no clock.

**Fixed in firmware 2.7.4** (`lib/WiFiManager`, shared with FilamentTagReader).
While the portal is up, the door retries its saved network in the background —
the ESP32 can do both in `WIFI_AP_STA` — and reboots into normal operation as soon
as it joins. That turns a 17-day outage into one that lasts about as long as the
network does:

- a 15 s attempt every 60 s, with the STA side stopped in between so the portal
  keeps its radio;
- a reboot on joining by **any** route, including the connect `begin()` started;
- the boot after that waits 60 s rather than 10 s, so a network that is slow to
  join cannot cause a reboot loop;
- no attempts for 3 minutes after someone loads a portal page, because they may
  be entering new credentials. Phones' automatic captive-portal probes do not
  count, or a remembered phone in range would hold the retry off indefinitely.

Details in `lib/WiFiManager/README.md` under *Leaving the portal*.

**Still to verify on hardware:** block the door's Wi-Fi (or change the SSID's
password on a test AP), reboot the door so it lands in the portal, restore the
network, and confirm it returns without anyone touching it. Also confirm the
portal stays usable from a phone during the retries.

#### It also exposed a timestamp bug: 22 events dated in the future

A door with no clock records uptime instead of a time, and ingest dated those
events as *start of the boot that reported them* + uptime. That is only right for
events from **that** boot. When Front Door came back on boot #18 and flushed taps
recorded during boots 16 and 17, they were dated from boot 18's start — up to
sixteen days in the future. Reports sorted them above today's activity.

**Fixed in the backend** (`cloud/api/src/eventTime.ts`, tested by
`npm run check-event-time`). Every event is now one of:

| | When | Stored as |
|---|---|---|
| **Observed** | the door's clock was set | its time |
| **Derived** | no clock, but from the boot that is reporting it, whose start is known | boot start + uptime, marked ≈ |
| **Unknown** | anything else | `timeUnknown`, with the **window** it must lie in — after the last trustworthy event before it in the door's own sequence, before the next one (and never after it was received). `at` is only a placement inside that window, so it sorts in order. |

Nothing is invented: an unknown event is shown as *"time unknown — between X and
Y"*, never as a time. The boot's start is stored once per boot on the door row, so
a retried sync resolves the same events to the same storage keys.

**Existing rows repaired** with `npm run repair-event-times` (dry run first, then
`--apply` with a backup written outside the repo before any change — it holds card
numbers). 27 events: 25 on Front Door (boots 16–17) and 2 on Test Door 1 (boot 20,
between Aug 27 15:22 and Sep 1 15:01 UTC; boot 23, between Sep 1 15:20 and Sep 16
20:35 UTC). Verified afterwards: EventsByDoor 187 → 187 and EventsByPerson 55 → 55
rows, none lost or duplicated, future-dated rows 22 → 0, every placement inside its
window, sequence order preserved, and a rerun finds nothing to repair.

The earlier reading of "three reboots on Sep 1" came from the bad timestamps and
was wrong; the repaired sequence is the one above.

**Finding a door without its IP:** the device ID is the last three MAC bytes
(`rfid-6f24f0` → `…6f:24:f0`), and its setup network's BSSID is that MAC plus one
(`…6f:24:f1`). Sweep the subnet and look for the MAC in `arp -a`, or scan for the
BSSID with `netsh wlan show networks mode=bssid`.

#### ✅ Changing a door's Wi-Fi — fixed in 2.8.2

Previously the setup portal was reachable only at boot, and only when the saved
network could not be joined, so **a door that was still connected could not be
moved to a new network at all.** The only routes were taking its old network away
and power-cycling it, or erasing it over USB — a visit to every door, some of
them above ceilings.

Three ways now, in the order to reach for them:

| | Use when | If it goes wrong |
|---|---|---|
| **Wi-Fi fields on `/setup`** | the new network is reachable from where you are | the door returns to the network that worked |
| **Admin app: *Open the setup network at the next restart*** | the new network is not reachable from the current one | the AP closes itself after 30 minutes |
| **Hold the exit button at power-on** | the door is on a network nobody can reach | release and reboot; nothing was erased |

Against the three requirements this section originally set:

- [x] **Try, then keep or revert.** `changeNetwork()` tries for 25 s, keeps the
  new credentials only once the door has an address, and otherwise writes back
  the old ones and reconnects. A typo costs a reconnect, not a ladder. It runs
  from `loop()` as a state machine, because the switch drops the connection the
  request arrived on — and because blocking would stall the strike release.
- [~] **It must be authenticated.** `/setup` still has no login: this is the
  unauthenticated local surface tracked below, and the Wi-Fi fields are now part
  of what that gap exposes. Someone on the LAN can move a door to a network they
  control. **This is the remaining piece of this fix**, and it is why the setup
  AP self-closes and the portal trigger is physical.
- [x] **A physical way back.** Holding the exit button through power-on forces
  the portal with no network and no USB. Credentials are not erased, and the
  2.7.4 background retry is suppressed while it is up — otherwise the portal
  would reboot away underneath whoever was standing there using it.

**There is deliberately no remote "reset Wi-Fi".** It is the one command that
cannot be confirmed, undone or retried: it arrives over the network it destroys,
and the door comes back reachable only by someone standing next to it. The admin
app instead *arms* the setup AP for the next boot, which pairs the remote intent
with physical presence and leaves the door working in the meantime.
`clearCredentials()` still exists, and still has no caller.

**Untested on hardware.** The AP-alongside-STA path in particular assumes
switching to `WIFI_AP_STA` does not drop a live station link, and that one
listener on port 80 answers on both interfaces. Both need proving on a door
before this is relied on.

#### Three more gaps from the same investigation

- **The setup network is open.** Anyone in radio range of a door that has booted
  without Wi-Fi can point it at a network they control. There, its
  unauthenticated local web surface is theirs. The portal needs a password, a
  physical trigger, or a time limit.
- **`/update` has no authentication** (`ElegantOTA.begin()` with no credentials).
  Combined with the gap above, that is firmware upload for anyone standing
  outside the door.
- **An offline door cannot say why, and nobody is told.** `EVT_SYNC_FAIL` is
  defined but nothing in the current source appends it, so no reason reaches the
  cloud even after a door reconnects. The dashboard did show both doors as not
  checking in, but only to someone who looked. A door silent for more than a few
  hours should notify someone rather than waiting to be noticed.

### Test log

**#9 Offline — ✅ PASSED**, `2026-08-16T20:06:02Z` → `2026-08-17T17:51:12Z`
(**21h 45m**, against the 6h planned). Test Door 1 (`rfid-275044`) with **all
outbound TCP and UDP** blocked at the firewall — NTP included, which matters for
the clock result below. LAN left reachable so internals stayed observable; Front
Door online as a control.

Predictions were written down **before** observing, so the run could falsify
rather than illustrate. All eight held:

| # | Prediction | Result |
|---|---|---|
| 1 | Cached roster still decides access | 4 granted (3 John Sr., 1 Avery), 3 denied — no cloud contact |
| 2 | Events spool, none lost | 7 spooled, 7 delivered |
| 3 | Backoff settles at the 15-min cap | twelve consecutive gaps, 15.0–16.1m |
| 4 | ~28 failures at +6h | model tracked all the way to **90 observed vs 91 predicted at 21.6h — 1.1%** |
| 5 | No crash: boot #15 holds | held through **90 failed TLS handshakes** |
| 6 | Clock free-runs within ~1 min | <35s over 21.7h with NTP provably blocked; every event `timeApprox=false` |
| 7 | Cloud flags it at T0+10m | `NOT CHECKING IN` at T0+11m |
| 8 | Flush keeps original timestamps, no duplicates | timestamps span the whole outage; **62 rows, 62 distinct `(bootId, idx)`** |

The flush is the part worth reading twice. Events recorded at 20:19 and 21:44 on
the 16th arrived at 17:51 on the 17th carrying **their own times**, not the
reconnect time, and resolved to the right people — attribution is done at ingest
against the credential index, so a 21-hour-old tap still names its holder.

Row accounting came out at **+8**, not +7. The extra is a `config` event the
device generated at 17:50:43 when it applied the roster it had missed
(`rev 3 → 8`), which is the outage's other half working: every change made in the
admin app while the door was dark landed on the first sync. The dual-write split
was exactly right — 4 attributed taps to person partitions, 3 unknown-card taps
to `unknown-202608` (now offered in the enrolment feed), and the personless
`config` event to the door table only.

**Two gaps this run exposed**, both now addressed in the ⚠️ TODO above: free heap
was invisible for the entire test, and `Time: … (NTP synced)` kept claiming a sync
for 21.7 hours during which NTP was provably unreachable. It means "has synced
since boot", not "is in sync" — reassuring in precisely the situation where it
should not be.

Still **untested**: the spool's overflow path. 5000 records needs 5000 taps, so
this run never approached it. It drops the oldest event and is the only path that
silently loses history, so it needs its own test with a temporarily lowered cap
rather than being taken on trust.

---

## Hardware — PCB Rev 2

The `RFID_Door_Controller` carrier (rev A) is in hand — five boards from JLCPCB,
being hand-assembled with workarounds. Rev 2 folds in the fixes below. The
generator (`hardware/tools/gen_board.py`) is **not** the source of truth for
what was ordered: the rev-A `.kicad_pcb` carries hand tweaks made in the editor
before ordering. **First step before cutting rev 2: reconcile those hand edits
back into the generator**, or a regeneration silently discards them.

### Root cause of the connector fixes

Every header's pin order was generated from the **schematic net order**, never
matched to the pinout of the **physical module it mates with**. So nearly every
connector is scrambled against its part. The rev-2 rule is therefore not just
the spot-fixes below but a pass over **every** connector — buck, OLED, panel
LED, reader, exit button, strike — pinning each to its actual mating part, with
**per-pin silkscreen labels on all of them**. (The polarity marks on the
2-terminal parts — diodes, caps, transistor — are stock-footprint art and were
verified correct; those are not affected.)

Two of the four would **damage a part** if plugged in straight, so the rev-A
boards must be wired by function, not seated directly:

1. **Buck rows (J2/J10) — swap +/− on both rows.** The board puts `+` on the
   left for both IN and OUT, but the symmetric LM2596 has IN and OUT on opposite
   ends, so seating it forces an end-flip that swaps left↔right — no orientation
   gets IN/OUT *and* polarity right at once. Put `+` on the right for both rows
   and move the IN+/IN−/OUT+/OUT− silk to match. *(Rev A: wire the four terminals
   to the same-named holes; never power it seated — one way reverse-feeds 12 V
   into the buck, the other pushes 5 V backward into the board.)*
2. **OLED header (J6) — reorder to GND-3.3V-SCL-SDA** to match the display
   module (currently 3.3V-GND-SDA-SCL). *(Rev A: a straight cable **reverse-powers
   the OLED** — cross the wires by function.)*
3. **Panel LED (J7) — reorder to Red-GND-Green-Blue** to match the board-mount
   RGB LED (currently Red-Green-Blue-GND). No damage risk; it just won't seat.
4. **Relay (K1) — add silk note "bare relay only — driver onboard."** The
   footprint is a bare SRD-05VDC relay; the driver (Q1/D3/R3/R4) is on the
   board, so the whole relay *module* must not be jammed in. Cosmetic.

*(The 1000 µF-input-cap idea was considered and dropped — C4's 100 µF is
sufficient and the bigger can isn't wanted.)*

### New requirement — door-contact (reed switch) connection point ⭐

This is what makes **Phase 6 (Door position sensing)** deployable. The firmware,
the `PIN_DOOR_CONTACT` pin map, and `lib/DoorContact` are already in place, but
the rev-A board has **no terminal to land the contact on** — the input pin isn't
broken out.

Rev 2 adds a **new 2-pin terminal: door-contact signal + a GND**, for a reed
(magnetic door-position) contact — internal pull-up, contact **closed when the
door is shut**, exactly the exit-button wiring pattern. The signal pin per board:

- **DevKit V1: GPIO 33** (beside the exit button on GPIO 32) — this is what the
  production doors need.
- **ESP32-S3: GPIO 17** (if an S3 socket is carried).
- **XIAO C6 / ESP32-C3: no free pin** — every GPIO is assigned; they need an I/O
  expander before a contact is possible. Unchanged from the Phase 6 notes.

Wire it as its own terminal so the reed and its ground are a clean field
connection, and give it per-pin silk (`DOOR`, `GND`) like the exit button.

### New requirement — RS-485 reader port for OSDP

For **Phase 8 Step 2**: a 3.3 V half-duplex RS-485 transceiver, a 120 Ω
termination jumper, and a 4-pin terminal (`12V`, `0V`, `A`, `B`) with per-pin
silk. Keep the existing Clock & Data / Wiegand terminal as well, so one board
serves both reader types. DevKit / S3 only (UART + DE/RE pin); fit it as an
optional footprint so boards that don't need it aren't populated.
