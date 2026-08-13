# WebService

A reusable device web service for ESP32 projects. It owns a long-lived
`WebServer`, drives it from its own FreeRTOS task, and provides the standard
admin surface most projects want — a status page, an over-the-air firmware
updater, and a remote log viewer — plus a hook for project-specific routes and
a consistent, themeable page footer.

Connectivity (WiFi, provisioning, mDNS) is intentionally **not** part of this
module; it assumes the network is already up and is started only when `begin()`
is called.

## What it serves

| Route          | Purpose                                                      |
|----------------|-------------------------------------------------------------|
| `/status`      | Plain-text status: WiFi IP, mDNS name, uptime, + project lines |
| `/status.txt`  | Content-only status, polled by the `/status` page            |
| `/update`      | OTA firmware update (ElegantOTA)                            |
| `/webserial`   | Self-refreshing remote log viewer (no async dependency)    |
| `/webserial.txt` | Content-only log, polled by the `/webserial` page          |
| `/setup`       | Composed settings page + **Reboot device** button — opt-in, see **Setup page** |
| `/reboot`      | **POST only.** Schedules a restart. Registered with `/setup` |
| `/footer.js`   | Client-side footer injector (see **Footer** below)         |
| `/footer.json` | Footer link list + colors consumed by `/footer.js`         |
| telnet `:23`   | Live log console, one client at a time                     |

The remote log is a ring buffer fed by `log()`, shared by `/webserial` and the
telnet console, and echoed to `Serial`. `log()` is safe to call from any task.

## Quick start

```cpp
#include "WebService.h"

WebService web(80);                       // owns a WebServer on port 80

void onConnected() {                      // call once, in STA mode only
    web.setHostname("mydevice");          // cosmetic label on /status
    web.setStatusProvider([](String& body){
        body += "Custom:   " + String(myValue) + "\n";   // project status lines
    });
    // ... register any project routes via web.routes() here, BEFORE begin() ...
    web.begin();                          // starts server + telnet + driver task
}

void someTask() {
    web.log("[app] something happened");  // -> /webserial, telnet, and Serial
}
```

All setup calls (`setHostname`, `setStatusProvider`, `enableSetup`, project
routes, footer configuration) must happen **before** `begin()`.

## Project-specific routes

Register extra routes on the underlying server before `begin()`:

```cpp
web.routes().on("/", [](){ /* serve your dashboard */ });
web.routes().on("/api/data", [](){ /* serve JSON */ });
```

## Project-specific status lines

`/status` always shows WiFi IP, mDNS name, and uptime. Append your own lines:

```cpp
web.setStatusProvider([](String& body){
    body += "Sensor:   " + String(reading) + "\n";
});
```

---

## Setup page

`/setup` is **opt-in**. Enabling it also registers `POST /reboot` and adds a
Setup link to the footer. Like `/status`, the page is *composed*: this module
renders a block of reusable settings, and the project appends its own fields.

```cpp
DeviceSettings settings;
settings.begin();                     // before enableSetup()

web.enableSetup(&settings);           // serves /setup + /reboot

web.setSetupFieldsProvider([](String& html){
    html += "<label>Door name</label>";
    html += "<input type='text' name='doorName' value='" +
            WebService::escapeAttr(identity.doorName()) + "'>";
});
web.setSetupSaveHandler([](WebServer& s){
    if (s.hasArg("doorName")) identity.setDoorName(s.arg("doorName"));
});
```

The reusable block covers splash hold, boot-diagnostics hold, preferred board,
and mDNS hostname; those are persisted (and where applicable applied)
automatically. A project that has no use for one of them can simply ignore it —
they are stored either way.

### Escaping injected values

Any project value rendered into the page must be escaped, and **which helper you
need depends on where it lands**:

| Helper | Use for | Also escapes |
|--------|---------|--------------|
| `WebService::escapeText(s)` | text between tags | `& < >` |
| `WebService::escapeAttr(s)` | values inside a quoted attribute | `& < > ' "` |

Attributes here are single-quoted, so an apostrophe in an operator-entered value
("John's Office") would close the attribute early and corrupt every field after
it. Use `escapeAttr()` for anything going into `value='...'`.

### Reboot

The page carries a **Reboot device** button, so a unit can be restarted from a
browser instead of in person — which matters once there are more devices than
you want to walk to.

- The route is **POST-only**, so a link prefetch, a crawler, or a mistyped URL
  can never restart a device.
- The restart is **deferred ~1.2 s** by the driver task rather than performed in
  the handler: `send()` only queues the response, so restarting immediately
  would cut the connection before the confirmation page reached the browser.
- The reboot button lives in its own `<form>`, outside the settings form. Nested
  forms are invalid HTML and browsers drop the inner one, which would make the
  button submit settings instead of rebooting. Keep that separation if you
  restyle the page.
- **Rebooting does not save the settings form** — they are separate submissions.
- Changing the mDNS hostname only takes effect after a reboot, since the
  responder is started at boot. That is the main reason the button exists.

---

## Footer

The module serves a consistent footer to every project's pages — a row of links
to the device's other pages — without each project hand-coding it. It is
rendered client-side, so a project's main page can stay a static `PROGMEM`
constant.

### Adding the footer to a page

Add **one line** to the page's HTML, just before `</body>`:

```html
<script src="/footer.js"></script>
```

That is the whole change. The script fetches `/footer.json` and appends a styled
`<footer>` to the page. Nothing is rebuilt at runtime and no extra RAM is used
per request.

### What populates the links

`begin()` pre-seeds built-in links for the pages this module serves:

- **Device status** → `/status`
- **Device log** → `/webserial`
- **Firmware update →** → `/update`
- **Setup** → `/setup` — *only when `enableSetup()` was called*

A project adds its own links with `addFooterLink()` **before** `begin()`. They
appear after the built-ins, in the order added:

```cpp
web.addFooterLink("Dashboard", "/");              // normal link
web.addFooterLink("Docs", "https://...", true);   // external -> trailing arrow
```

> ⚠️ **`addFooterLink()` does not de-duplicate.** Adding a link the module
> already seeds — most easily `"Setup" → "/setup"` when `/setup` is enabled —
> produces two identical entries in the footer. Let the module supply its own.

To replace the built-ins entirely, call `clearFooterLinks()` before adding your
own (advanced; most projects should not).

### Theming the footer

The footer defaults to a dark-theme green accent. Override the colors to match a
project's UI with `setFooterColors(link, separator, linkHover)`:

```cpp
web.setFooterColors("#5fd3a0",   // link text color
                    "#2a3645",   // " | " separator color
                    "#7fe0b5");  // hover color (optional; omit to reuse link)
```

Colors are any CSS color string (`#rrggbb`, `rgb(...)`, named colors). They are
delivered via `/footer.json` and applied client-side.

### Notes and caveats

- The footer renders client-side, so it appears a fraction of a second after the
  page loads. On a LAN admin page this is imperceptible. With JavaScript
  disabled the footer simply does not appear; the rest of the page is unaffected.
- Link labels are HTML/JSON-escaped, so labels containing `<`, `>`, `"`, or `\`
  are safe.
- `/footer.json` is hand-built (no ArduinoJson dependency), keeping this module
  self-contained.

## API reference

| Method | Purpose |
|--------|---------|
| `WebService(uint16_t port = 80)` | Construct; owns a `WebServer` on `port`. |
| `setHostname(host)` | Cosmetic mDNS label shown on `/status`. |
| `setStatusProvider(fn)` | Append project lines to `/status`. |
| `routes()` | Access the underlying `WebServer` to add routes (before `begin()`). |
| `addFooterLink(label, href, external=false)` | Add a project footer link. Does **not** de-duplicate. |
| `clearFooterLinks()` | Remove all footer links incl. built-ins (advanced). |
| `setFooterColors(link, separator, linkHover="")` | Theme the footer. |
| `enableSetup(&settings)` | Serve `/setup` + `POST /reboot`, backed by a `DeviceSettings`. |
| `setSetupFieldsProvider(fn)` | Append project fields to the `/setup` form. |
| `setSetupSaveHandler(fn)` | Persist project fields from the `/setup` POST. |
| `escapeText(s)` *(static)* | Escape for text between tags (`& < >`). |
| `escapeAttr(s)` *(static)* | Escape for a quoted attribute (adds `' "`). |
| `log(line)` | Append to the remote log (`/webserial`, telnet, `Serial`). |
| `begin()` | Register routes, start server + telnet, spawn driver task. |

## Dependencies

- [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) (sync mode:
  build with `-D ELEGANTOTA_USE_ASYNC_WEBSERVER=0`)
- Arduino-ESP32 core `WebServer` (sync)

## License

MIT — AWS Solutions LLC dba Azure Innovators.
