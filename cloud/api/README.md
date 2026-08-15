# API

Azure Functions backend for the door fleet. TypeScript, Node 24, Flex Consumption.

**Run every command below from this directory** (`cloud/api`) — that is where
`package.json` lives, and npm looks no further up the tree.

## Endpoints

| Route | Auth | Purpose |
|---|---|---|
| `POST /api/v1/sync` | `x-device-key` header | Events up, roster/config down. The only call a door makes in normal operation. |
| `POST /api/v1/enroll` | short-lived pairing code | One-time device pairing; returns the long-lived device key. |

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
