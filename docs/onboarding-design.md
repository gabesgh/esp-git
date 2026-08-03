# Onboarding: flash from a browser, set up on the device

How a stranger goes from a bare board to a working display without a toolchain,
a cable driver, or a text editor.

## What is in the way today

Three things are compiled into `firmware/include/config.h`:

| Baked in | Why it blocks other people |
|---|---|
| `WIFI_SSID` / `WIFI_PASS` | their network is not yours |
| `DEVICE_TOKEN` | every device would share one secret |
| `PULSE_HOST` | points at one specific deployment |

None of these can stay at compile time if the binary is going to be handed out.
They all have to move into NVS and be set on the device or from a browser.

## The flow

```
1. FLASH      browser -> WebSerial -> board          (no toolchain, no cable driver)
2. WIFI       board becomes an AP -> captive portal -> pick network
3. PAIR       board shows a code -> user enters it on the site while signed in
4. CONFIG     site sets repo, effect, intervals -> board picks them up
```

### 1. Flash from the browser

[ESP Web Tools](https://esphome.github.io/esp-web-tools/) is a web component that
speaks the ESP32 bootloader protocol over WebSerial. The site serves a manifest
plus the four images (bootloader, partitions, boot_app0, firmware) and the
browser does the rest.

Chrome and Edge only; Safari and Firefox have no WebSerial. That is a hard
limitation, not something to work around, so the page has to say so up front
rather than failing mysteriously.

Serving the images is already solved: `/api/firmware` does it for OTA. The
flasher needs the *unauthenticated* variant plus the three support images, which
never change between releases.

### 2. WiFi, on the device

On boot, if NVS has no credentials, the board starts a SoftAP called
`esp-git-setup` and runs a DNS server that answers every query with its own
address. Phones detect that as a captive portal and pop the page automatically.

The page lists networks from a scan, takes a password, writes both to NVS and
reboots. Standard, well-trodden, and it means the WiFi password never leaves the
device or gets typed into a website.

A long press on the boot button clears NVS and returns here, which is the escape
hatch when someone moves house or mistypes a password.

### 3. Pairing, without the user handling a token

The device must never hold a GitHub token. Pairing keeps that true:

```
board                        server                      user
  |-- POST /api/pair/start ---->|
  |    { deviceId }             |  mint 6-char code, store pending
  |<-- { code: "K7M2QX" } ------|
  | show code on screen         |
  |                             |<--- signs in with GitHub OAuth ---|
  |                             |<--- enters K7M2QX ----------------|
  |                             |  bind deviceId -> github account
  |-- GET /api/pair/poll ------>|
  |<-- { token: <device tok> } -|  device-specific, revocable alone
```

The GitHub OAuth token stays server-side. The board receives only a token scoped
to reading its own owner's numbers. Revoking one board does not touch the others.

Codes expire in ten minutes and are single use, otherwise they are a standing
invitation to bind someone else's board.

### 4. Config from the site

Repo, push effect, poll intervals and brightness live server-side against the
device id. The board already polls `/api/command`, so this needs no new
transport: settings arrive the same way commands do.

## What this forces: a database

Multi-tenant means persistent state that outlives a serverless instance:

- github account -> oauth token
- device id -> owner, device token, settings
- pending pairing codes

There is no way around this. The current in-memory store is fine for one device
on one deployment and cannot work for anyone else. This is the one place where
"add a database" is a real requirement rather than an optimisation.

Neon Postgres or Upstash Redis, both provisioned through the Vercel marketplace
with no separate signup. Postgres is the better fit: the relationships above
are relational and will grow.

## Order of work

1. **WiFi captive portal**: self-contained, no server changes, and the biggest
   single unlock. Removes two of the three compile-time values.
2. **Web flasher page**: static, serves images that already exist.
3. **Pairing + OAuth + settings**: needs the database, and is the only part that
   changes the server's shape.

1 and 2 are useful on their own: they make the firmware shareable even while
pairing is still manual.

## Deliberately not doing

- **Bluetooth provisioning.** Works, but needs an app. A captive portal needs a
  browser everyone already has.
- **Hardcoded fallback WiFi.** Tempting for demos, terrible in a public binary.
- **Shipping a token in the image.** Everyone would share one secret and a flash
  dump would hand it over.
