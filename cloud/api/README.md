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

Both are `authLevel: anonymous` on purpose: devices authenticate with their own
per-door key, not a shared Functions key that would be identical across the
fleet and unrevocable per device.

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
Compress-Archive -Path host.json,package.json,dist,node_modules `
  -DestinationPath deploy.zip -CompressionLevel Fastest -Force
az functionapp deployment source config-zip `
  --resource-group JTC-prod-rfidaccess-eastus2-rg `
  --name JTC-prod-rfidaccess-eastus2-func `
  --src deploy.zip
npm install                               # restore dev deps afterwards
```

`host.json` and `dist/` must sit at the **root** of the zip, not inside a folder.

Two things that will bite if forgotten:

- **`main` in package.json is `dist/api/src/functions/*.js`.** `rootDir` is `..`
  so that `shared/types.ts` compiles in too, which puts the api sources under
  `dist/api/`. If that glob stops matching the build layout, the host registers
  **zero** functions and deployment still reports success.
- **Flex Consumption is required.** Y1 Consumption deploys via a storage account
  key, and the data account has `allowSharedKeyAccess: false`. Flex reads its
  package using the app's managed identity instead, which is what keeps that
  setting possible.
