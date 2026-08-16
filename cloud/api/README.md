# API

Azure Functions backend for the door fleet. TypeScript, Node 24, Flex Consumption.

**Run every command below from this directory** (`cloud/api`) — that is where
`package.json` lives, and npm looks no further up the tree.

## Endpoints

| Route | Auth | Purpose |
|---|---|---|
| `POST /api/v1/sync` | `x-device-key` header | Events up, roster/config/firmware-offer down. The only call a door makes in normal operation. |
| `POST /api/v1/enroll` | short-lived pairing code | One-time device pairing; returns the long-lived device key. |
| `GET /api/v1/firmware` | `x-device-key` + `x-device-id` | Serves the image for the calling door's board. **No board parameter** — the board comes from the authenticated door's record, so a device cannot request another variant's image. |

These are `authLevel: anonymous` on purpose: devices authenticate with their own
per-door key, not a shared Functions key that would be identical across the
fleet and unrevocable per device.

### Admin surface

Everything under `v1/admin/` is called by the web app with an Entra bearer token.

Each route is one function that dispatches on method, with the role checked
**per method** — so `GET` and `DELETE` on the same path need different roles.

| Route | GET | POST | DELETE |
|---|---|---|---|
| `v1/admin/people` | Viewer | Operator | Admin |
| `v1/admin/credentials` | Viewer | Operator | Admin |
| `v1/admin/groups` | Viewer | Admin | Admin |
| `v1/admin/doors` | Viewer | Admin | — |
| `v1/admin/doors/roster` | Viewer | — | — |
| `v1/admin/doors/pairing-code` | — | Operator | — |
| `v1/admin/reports/{person,door,unknown,unattributed-exits}` | Viewer | — | — |

`doors/roster` returns the effective roster a given door would receive — the
group intersection resolved, which is the quickest way to answer "why can this
person not open that door?" without reading the device.

**The `v1/` prefix on admin routes is not cosmetic.** `admin` is a *reserved
route prefix* in the Azure Functions host — routes beginning `admin/` are
swallowed by the host's own management endpoints and return 404 no matter how
correctly the function is registered. Nothing warns you; the function appears in
the portal and simply never receives a request. Do not "tidy" these to `admin/*`.

## Admin authorization

The web app sends `Authorization: Bearer <token>` and `adminAuth.ts` verifies the
signature against Entra's JWKS, plus issuer, audience and expiry. Roles arrive in
the `roles` claim, populated by Entra from app-role assignments on the security
groups.

The earlier design trusted an `x-ms-client-principal` header injected by Static
Web Apps. **That was exploitable**: the Function App has a public hostname, so
anyone who knew the URL could send that header themselves and be an Admin. It was
demonstrated against this API with a single `curl`. If you are tempted back to
header trust because it is less code, that is why it is not there.

Two settings must exist on the Function App or **every admin call fails closed**
with `401 missing or invalid token`:

| Setting | Purpose |
|---|---|
| `ENTRA_TENANT_ID` | Which tenant's JWKS and issuer to trust |
| `ENTRA_CLIENT_ID` | Expected audience |

Both come from Bicep parameters, **not** `az functionapp config appsettings set`
— see the warning in `../infra/README.md`. Failing closed on missing config is
deliberate: an unconfigured deployment refuses everyone rather than accepting
unverified tokens.

Note that `jose` is loaded with a cached dynamic `import()`. It is ESM-only and
this package emits CommonJS for the Functions host, so a static import will not
compile.

## Local development

```powershell
npm install
npm run build          # tsc
npm run typecheck      # tsc --noEmit
```

## Seeding

Seeds groups, people, credentials and doors, then prints the effective roster
each door would receive — which is the real point: it exercises the group
intersection before any device depends on it.

```powershell
$env:STORAGE_ACCOUNT_NAME = "jtcprodrfidaccessst"

Copy-Item seed.example.json seed.json     # gitignored: holds real fob numbers
npm run seed -- --dry-run                 # validate + preview, writes nothing
npm run seed                              # apply
npm run seed -- --prune                   # apply AND delete rows not in the file
npm run seed -- --code rfid-275044        # issue a 15-minute pairing code
```

**Renaming a `personId` or `credId` leaves the old row behind.** Upserting alone
does not converge. Every run therefore reports rows present in the tables but
absent from `seed.json`, and flags any credential number held by more than one
row — that case makes event attribution non-deterministic, and a stale *person*
row carrying an old group can silently keep granting access you thought you had
revoked. Detection is always on; `--prune` deletes. An orphan door that is
already paired is called out explicitly, since deleting it would unpair a
working device.

Authenticates as whoever ran `az login`, so it needs **Storage Table Data
Contributor** on the account (see `../infra/README.md`). There is no account key
to supply, because the account has none.

Idempotent. Doors are written with `Merge`, so re-seeding never clobbers the
`keyHash` of an already-paired device.

### Before the first device sync

The cloud roster **replaces** the device's roster wholesale on first sync. Any
fob enrolled locally but missing from `seed.json` stops working — silently, at a
door that may be hard to reach. Check the dry-run output against each door's
`/api/list` before pointing a device at the backend.

## Fleet status

```powershell
npm run fleet
```

What every door is running, whether it took the latest image for its board, and
whether any door has stopped checking in. A silent door is worth noticing
precisely because nothing looks wrong at the door itself — it keeps granting
access from its cached roster, so this is the only place the problem shows.

## Firmware rollout

Firmware is published **per board type**, never per fleet. An image built for one
ESP32 variant does not merely misbehave on another: it fails to boot, and the
door is dead until someone reaches it with a USB cable.

```powershell
npm run publish-fw -- --list
npm run publish-fw -- --board "ESP32 DevKit V1" --version 2.5.1 `
                      --file ../../.pio/build/esp32dev/firmware.bin
```

The board string must match the device's `BOARD_NAME` exactly.

**Guards, because this failure is unrecoverable without physical access:**

- Refuses `firmware.factory.bin` — that is the merged image for USB flashing at
  offset 0; in an OTA slot it produces a device that will not boot.
- Refuses an image larger than the OTA slot in `partitions_rfid.csv`.
- **Reads the `fw=` marker back out of the binary** and refuses if it disagrees
  with the version you typed. Publishing a 2.5.0 image labelled 2.5.1 puts every
  door on that board into a permanent update loop — it flashes, reboots, still
  reports 2.5.0, is offered 2.5.1 again, forever. The device cannot detect this;
  each offer looks legitimately newer from its side.
- Uploads the blob **before** writing the metadata, so an interrupted publish
  leaves the previous offer intact rather than pointing doors at a missing image.

### Staging a rollout

Firmware is offered to every door of that board at once. To prove an image on an
accessible door first, hold the others back:

```powershell
az storage entity merge --table-name Doors --account-name jtcprodrfidaccessst `
  --auth-mode login --entity PartitionKey=door RowKey=rfid-6f24f0 `
  fwHold=true fwHold@odata.type=Edm.Boolean
```

`npm run fleet` shows held doors as `HELD at <version>`. Set `fwHold=false` to
release. Worth doing for any door that is awkward to reach.

### Rollback

The backend offers whenever the published version **differs**, not only when it
is newer — deliberately, so publishing an older good image rolls the fleet back.
The cost is that publishing an older version pulls doors back to it, so the rule
is simply: publish the version you want doors running.

## Deploying

```powershell
npm run build
npm prune --omit=dev                      # keep the bundle small
python tools/pack.py -o deploy.zip        # NOT Compress-Archive -- see below
az functionapp deployment source config-zip `
  --resource-group JTC-prod-rfidaccess-eastus2-rg `
  --name JTC-prod-rfidaccess-eastus2-func `
  --src deploy.zip
npm install                               # restore dev deps afterwards
```

`host.json` and `dist/` must sit at the **root** of the zip, not inside a folder.

### ⚠️ Never package with `Compress-Archive`

It produces a zip that is perfectly valid on Windows and unusable here. Two
defects, both invisible until runtime:

- It writes **almost no directory entries** — 65 for this tree, and not the one
  holding the functions.
- It writes `external_attr = 0`, i.e. Unix mode **000**. Every file arrives with
  no read permission.

Flex Consumption **mounts** the zip rather than extracting it, so both survive to
runtime. `tools/pack.py` writes Unix modes and an entry for every directory, and
refuses to produce a package that fails either check.

**The failure this causes is worth recognising**, because nothing points at the
zip. `az` reports `"Deployment was successful."`, the package appears in the
`deployment` container at the right size, the host starts cleanly, and:

```
Error: Worker was unable to load entry point "dist/api/src/functions/*.js":
Found zero files matching the supplied pattern
```

A glob has to enumerate directories and stat files; it can do neither. The host
registers **zero** functions, so every route 404s — including `/api/v1/sync`,
which takes the entire door fleet offline. Doors keep granting from their cached
rosters, so nothing looks wrong at any door.

`az functionapp function list` returning empty is the fastest confirmation. The
host's own explanation is in Application Insights:

```powershell
$appId = az resource show -g JTC-prod-rfidaccess-eastus2-rg `
  -n JTC-prod-rfidaccess-eastus2-appi `
  --resource-type "microsoft.insights/components" --query "properties.AppId" -o tsv
$tok = az account get-access-token --resource "https://api.applicationinsights.io" `
  --query accessToken -o tsv
# then POST to https://api.applicationinsights.io/v1/apps/$appId/query with
#   union traces,exceptions | where timestamp > ago(30m) | where severityLevel >= 2
```

Query it directly like this rather than via `az extension add application-insights`
— installing that extension can leave the CLI unable to run at all.

### Always verify registration, not just deployment

"Deployment was successful" only means the bytes arrived. Check that the host
actually indexed them:

```powershell
az functionapp function list -g JTC-prod-rfidaccess-eastus2-rg `
  -n JTC-prod-rfidaccess-eastus2-func --query "length(@)"
```

Expect **14**. Zero means the host loaded nothing. An unauthenticated
`GET /api/v1/admin/doors` returning **401** proves routes are live; **404** means
they are not registered.

Two things that will bite if forgotten:

- **`main` in package.json is `dist/api/src/functions/*.js`.** `rootDir` is `..`
  so that `shared/types.ts` compiles in too, which puts the api sources under
  `dist/api/`. If that glob stops matching the build layout, the host registers
  **zero** functions and deployment still reports success.
- **Flex Consumption is required.** Y1 Consumption deploys via a storage account
  key, and the data account has `allowSharedKeyAccess: false`. Flex reads its
  package using the app's managed identity instead, which is what keeps that
  setting possible.
