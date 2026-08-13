# Reusable page footer (WebService)

WebService now serves a consistent, drop-in footer to every project. It lists
the device's other pages as links, pre-seeded with the module's own built-in
pages and extendable per project.

## How it works

- `/footer.js` — a tiny client-side script the module serves. Any page that
  includes it gets a footer injected just before `</body>`.
- `/footer.json` — the link list the script fetches. Stays in sync with the
  registry automatically.
- Built-in links (added in `begin()`): **Device status** (`/status`),
  **Device log** (`/webserial`), **Firmware update →** (`/update`), and
  **Setup** (`/setup`) when the project called `enableSetup()`.

## Adding the footer to a project's main page

Add ONE line near the end of the dashboard HTML, just before `</body>`:

```html
<script src="/footer.js"></script>
```

That is the entire change to a project page. The page stays a static `PROGMEM`
constant — nothing is rebuilt at runtime, no extra RAM per request.

## Adding a project-specific link (e.g. "Dashboard")

Call `addFooterLink()` BEFORE `webSvc.begin()`:

```cpp
web.addFooterLink("Dashboard", "/");            // normal link
web.addFooterLink("Docs", "https://...", true); // external -> shows a trailing arrow
web.begin();
```

Project links appear after the built-ins, in the order added.

> ⚠️ **`addFooterLink()` does not de-duplicate.** Don't re-add a page the module
> already seeds — in particular `"Setup" → "/setup"`, which `begin()` adds by
> itself whenever `enableSetup()` was called. Doing so shows the link twice.

## Notes

- The footer renders client-side, so it appears a fraction of a second after
  the page loads. For a LAN admin page this is imperceptible. If JavaScript is
  off, the footer simply doesn't appear (the page is otherwise unaffected).
- Labels are HTML/JSON-escaped, so a link label with `<`, `>`, `"`, or `\` is
  safe.
- To replace the built-ins entirely, call `clearFooterLinks()` before adding
  your own and before `begin()` (advanced; most projects should not).
