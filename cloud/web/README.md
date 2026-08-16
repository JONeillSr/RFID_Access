# Admin web app

Preact + Vite single-page app: dashboard, doors, people and reports. Hosted on
Azure Static Web Apps (Free tier), signed in with Entra ID.

**Run every command from this directory** (`cloud/web`).

## Setup

```powershell
npm install
Copy-Item .env.example .env.local     # gitignored; fill in your values
npm run dev                            # http://localhost:5173
```

`localhost:5173` is registered as both a redirect URI on the app registration and
an allowed CORS origin, so sign-in works locally against the real backend. The
port is pinned in `vite.config.js` for exactly that reason — changing it breaks
local sign-in until both registrations are updated.

## Publishing

```powershell
npm run deploy        # builds, then uploads dist/ to Static Web Apps
```

`npm run build` alone just produces `dist/`. Anything in `public/` is copied to
the deploy root, which is how `logo.svg`, `favicon.svg` and
`staticwebapp.config.json` get there.

## How auth works

The browser signs in with MSAL (authorization-code flow with PKCE) and calls the
Function App **directly** with an `Authorization: Bearer` token. The API verifies
that token's signature, issuer and audience against Entra.

It deliberately does **not** use Static Web Apps' linked-backend mode, which
would inject an `x-ms-client-principal` header the API has to *trust*. That trust
only holds while every request arrives through SWA — and the Function App is on a
public hostname, so anyone who knew the URL could send that header themselves and
become an Admin. It was demonstrated against this API with one `curl`. Verifying
a signature removes the assumption; it also means SWA Free is sufficient, since
linked backends need Standard.

Consequence: the app is a **cross-origin caller**, so its origin must be in the
Function App's CORS list (`allowedOrigins` in `../infra/main.parameters.json`).

**Roles are not a UI concern.** The role in the token decides what the app
*shows*, but every route is enforced again server-side against the same token.
Hiding a button is a courtesy, not a control.

## Branding

Everything customer-identifying lives in exactly two places:

| What | Where |
|---|---|
| Company name, product name, address, phone, logo path | `src/branding.js` |
| Colours, radius | the `:root` block in `src/styles.css` |

Drop the logo at `public/logo.svg`. The header falls back to a text wordmark if
it is missing, so a forgotten asset never leaves a broken image.

Branding is baked in at **build time**, which is correct given one deployment per
customer (see Phase 7 in `../../ROADMAP.md`). Each customer's bundle is built
with their `branding.js` and palette. A runtime branding page would only add
customer self-service, not capability — and the expensive part would be logo
upload and serving, not the colours.

## Configuration

`.env.local`, gitignored. Only `VITE_`-prefixed variables reach the browser,
which makes "is this shipped to the client?" a visible property of the name.

| Variable | Purpose |
|---|---|
| `VITE_TENANT_ID` | Entra tenant to authenticate against |
| `VITE_CLIENT_ID` | App registration; also the token audience |
| `VITE_API_BASE` | Function App base URL, ending `/api` |

There is **no client secret**, here or anywhere in this app. A single-page app
cannot hold one — anything shipped to a browser is readable by whoever opens dev
tools. PKCE exists precisely so a public client can authenticate without one.

## Adding a page

1. A component in `src/pages/`.
2. A `<Route>` in `src/main.jsx` and an entry in `Nav`.

Filters belong in the URL (see `Reports.jsx`, which uses `useSearch`) so a view
can be bookmarked or sent to someone. That was the main thing the pre-Vite
version could not do.

## Custom domain

Live at `https://access.jtcustomtrailers.com`. Adding another domain means four
changes **together**, or sign-in half-works in a way that is hard to diagnose:

1. CNAME at the DNS provider pointing to the SWA default hostname. Azure will not
   accept the domain until the record resolves.
2. `az staticwebapp hostname set` — the certificate is then issued automatically.
3. The new origin added to the app registration's SPA redirect URIs.
4. The new origin added to `allowedOrigins` in the infra parameters, and to
   `connect-src` in `public/staticwebapp.config.json` if the API host changes.

## Content Security Policy

In `public/staticwebapp.config.json`. `script-src` is `'self'` — MSAL is bundled
rather than loaded from a CDN. **Anything new loaded from another origin** (fonts,
a chart library) needs an entry here, or the browser blocks it silently. That
failure looks like the feature simply not working, with only a console message to
explain it.
