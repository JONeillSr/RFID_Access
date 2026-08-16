# Infrastructure

Bicep for the RFID Access backend, deployed at **resource group scope**.

## Why there are no IDs in here

`main.bicep` contains no tenant id, no subscription id, and no secrets.
Resource-group-scope deployment supplies all of that from context, so the
templates stay clean enough to live in a public repo. Your own identifiers
belong in `deploy.local.ps1` (gitignored), never in a tracked file.

## First deploy

**Run these from this directory** (`cloud/infra`) — the paths below are relative
to it:

```powershell
cd cloud\infra

az login --tenant <your-tenant-domain>
az account set --subscription "<your-subscription-name>"

# Create your local parameters file. It is gitignored, which is why it is not
# already here -- the example is the tracked copy.
Copy-Item main.parameters.example.json main.parameters.json
```

Always run `what-if` first. As well as showing what would change, it is the
quickest way to discover that the storage account name is already taken
globally:

```powershell
az deployment group what-if `
  --resource-group JTC-prod-rfidaccess-eastus2-rg `
  --template-file main.bicep `
  --parameters '@main.parameters.json'
```

Then, once that looks right:

```powershell
az deployment group create `
  --resource-group JTC-prod-rfidaccess-eastus2-rg `
  --template-file main.bicep `
  --parameters '@main.parameters.json'
```

## After the first deploy: grant yourself data access

Because `allowSharedKeyAccess` is `false`, **you will not be able to see any
table or blob data** — in the portal or Storage Explorer — until your own
account holds a data-plane role. Account-key access simply fails. This is the
setting working as designed, but it reads exactly like a broken deployment.

Grant yourself the same two roles the Function App gets:

```powershell
$me  = az ad signed-in-user show --query id -o tsv
$acct = az storage account show `
  --name jtcprodrfidaccessst `
  --resource-group JTC-prod-rfidaccess-eastus2-rg --query id -o tsv

az role assignment create --assignee $me --scope $acct `
  --role "Storage Table Data Contributor"
az role assignment create --assignee $me --scope $acct `
  --role "Storage Blob Data Contributor"
```

Role assignments can take a couple of minutes to propagate.

## Required permissions

The two role assignments in the template need **Owner** or **User Access
Administrator** on the resource group. Contributor alone creates every other
resource but fails on those with `AuthorizationFailed`. The template is
idempotent, so re-running after the permission is granted completes the rest.

## What gets created

| Resource | Name | Notes |
|---|---|---|
| Storage account | `jtcprodrfidaccessst` | Tables + firmware/archive/deployment blobs |
| Function App | `JTC-prod-rfidaccess-eastus2-func` | Linux, **Node 24** (EOL Apr 2029) |
| App Service Plan | `JTC-prod-rfidaccess-eastus2-plan` | **FC1 Flex Consumption** |
| Application Insights | `JTC-prod-rfidaccess-eastus2-appi` | |
| Log Analytics | `JTC-prod-rfidaccess-eastus2-log` | 30-day retention |
| Static Web App | `JTC-prod-rfidaccess-eastus2-swa` | **Free** tier; hosts the admin app |

Endpoint: `https://jtc-prod-rfidaccess-eastus2-func.azurewebsites.net/api/v1/`

Tables are pre-created: `People`, `Credentials`, `Groups`, `Doors`, `Meta`,
`EventsByPerson`, `EventsByDoor`.

## Three decisions worth knowing about

**Everything is in East US 2, not East US.** Flex Consumption is not offered in
eastus, and Flex is what allows the deployment package to be read using the
app's managed identity rather than a storage account key — which is what makes
`allowSharedKeyAccess: false` possible at all. Y1 Consumption cannot deploy to a
key-less account. Storage sits in the same region as compute deliberately: the
sync endpoint touches storage on every request, so a cross-region hop would add
~15 ms to a path that runs twice a minute per door.

**No storage account keys anywhere.** `allowSharedKeyAccess` is `false`, and the
Function App reaches storage through a system-assigned managed identity granted
*Storage Table Data Contributor* and *Storage Blob Data Contributor*, scoped to
this account only. Nothing to leak, rotate, or paste into an app setting.

A consequence: to browse tables from Storage Explorer or the portal you must
sign in with Entra ID and hold a data-plane role yourself. Account-key access
will simply fail — that is the setting working, not a fault.

**Storage account naming is constrained.** 3–24 characters, lowercase
alphanumeric only, and globally unique across all of Azure. The full
`{org}-{env}-{app}-{region}` convention exceeds 24, so the region is dropped —
it is already in the resource group name. If `jtcprodrfidaccessst` is taken,
change `storageName` in your parameters file; nothing else depends on it.

## Entra ID: app registration and roles

The admin web app authenticates against a single-tenant app registration named
**JTC RFID Access**, with three app roles. Its identifiers live in
`entra.local.json` (gitignored) — not secrets, but they name the tenant and app
to target, so they stay out of a public repo. `app-roles.json` IS tracked: it is
the source of truth for the role definitions.

### Roles

| Role | Can |
|---|---|
| `Viewer` | View doors, people, credentials and reports; export them. No writes. |
| `Operator` | Everything above, plus enrol and label fobs, assign them to people, deactivate a lost fob, add or remove people from **existing** groups, and issue pairing codes. |
| `Admin` | Everything above, plus create/delete groups, change which groups a door honours, edit door config and schedules, publish firmware, set `fwHold`, and delete people. |

> **Known deviation:** deleting a *credential* is currently **Operator**, not
> Admin (`DELETE v1/admin/credentials` in `../api/src/functions/admin.ts`).
> Deleting people is correctly Admin. Arguably fine — retiring a fob is squarely
> "working within the model" — but it is not what this table originally said, and
> it removes the audit trail rather than deactivating. Worth settling explicitly
> before the write UI exposes a delete button.

The Operator boundary is deliberate: an Operator works **within** the access
model, an Admin **redefines** it. Operators can grant a person access to doors
their group already opens; only an Admin can change which doors a group opens.

### Roles come from group membership

Tenant has Entra ID P2, so app roles are assigned to **security groups** rather
than individual users — the roles then arrive in the token as a `roles` claim
with no group-ID mapping needed in code, and "who is an admin?" is answered by
looking at one group.

| Group | Role |
|---|---|
| `JTC Access Control` | Viewer |
| `JTC Access Control Operators` | Operator |
| `JTC Access Control Admins` | Admin |

A user in several groups receives several roles; the app takes the highest. So
adding someone to Admins does **not** mean removing them from the base group.

### Reproducing it

```powershell
az ad app create --display-name "JTC RFID Access" `
  --sign-in-audience AzureADMyOrg --app-roles '@app-roles.json'
az ad sp create --id <appId>

# then, per group, POST to the service principal's appRoleAssignedTo:
#   { principalId: <groupObjectId>, resourceId: <spObjectId>, appRoleId: <roleId> }
```

Assigning app roles to groups requires Entra ID **P1 or above**. On a free
tenant the fallback is to emit a `groups` claim and map group object IDs to
roles in a `rolesSource` function.

### Redirect URIs

Registered as **SPA** redirect URIs (not Web), because the admin app uses the
authorization-code flow with PKCE and holds no client secret:

- `https://access.jtcustomtrailers.com`
- the SWA default `*.azurestaticapps.net` hostname
- `http://localhost:5173` — local development

These are the app's **own origins**, not `/.auth/login/aad/callback`. That
callback path belongs to Static Web Apps' built-in authentication, which this
system deliberately does not use (see the CORS section below). Registering the
wrong style fails at sign-in with a reply-URL mismatch.

A redirect URI registered under the **Web** platform rather than **SPA** fails
differently and more confusingly: sign-in appears to work, then the token request
is rejected because Entra expects a client secret for a confidential client.

## ⚠️ App settings are declared here, and only here

`siteConfig.appSettings` is **authoritative**. A deployment replaces the entire
collection, so anything set out-of-band with `az functionapp config appsettings
set` is silently erased by the next `az deployment group create`.

This has already broken production once: `ENTRA_TENANT_ID` and `ENTRA_CLIENT_ID`
were set by hand, worked fine, and were wiped by an unrelated redeploy that added
the Static Web App. Sign-in still succeeded — the failure surfaced only as
`missing or invalid token` on every API call afterwards, because the API cannot
verify a token when it does not know which tenant to trust.

Nothing warns you. `what-if` shows the setting being removed, but among many
other changes.

**So: every setting the app needs must be a parameter in `main.bicep`**, with
values in the gitignored `main.parameters.json`. If you find yourself reaching
for `appsettings set` to fix something quickly, that fix has an expiry date.

## CORS

The admin app is served from a different origin than the API, so the Function App
must name the origins allowed to call it. The `allowedOrigins` parameter carries
them:

```json
"allowedOrigins": { "value": [
  "https://access.jtcustomtrailers.com",
  "https://<swa-default>.azurestaticapps.net",
  "http://localhost:5173"
]}
```

This is a consequence of a deliberate choice: the browser calls the Function App
**directly** with a bearer token the API verifies, rather than being proxied
through Static Web Apps' linked-backend feature. Proxying would make everything
same-origin and remove the need for CORS entirely — but it would also mean the
API trusting an injected identity header, which is forgeable against a public
hostname (see `../api/README.md`). Cross-origin configuration is the price of
verifying signatures instead of trusting a proxy. It also keeps SWA on **Free**,
since linked backends require Standard.

CORS is browser-enforced and is **not** a security control: `curl` ignores it
entirely. Authorization is the token check. A missing origin here shows up as
what looks like a network failure in the browser console, not a permission error
— which is why it reads as "the API is down".

Adding a customer domain means updating this list, the SPA redirect URIs, and
`connect-src` in the web app's `staticwebapp.config.json` together.

## Cost

At roughly 20 doors syncing every 30 s (~86k executions/month) this sits inside
the Flex Consumption free grant. Table Storage and the log workspace are a few
cents.
Application Insights includes 5 GB/month free, which this will not approach.
