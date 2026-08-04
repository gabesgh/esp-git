# AGENTS.md

Instructions for a coding agent working in this repo. Written for any assistant
that reads `AGENTS.md`.

If you are a human: this doubles as the setup guide. Start at
**Setup, from a bare board** and work down.

---

## What this is

A GitHub activity display for cheap ESP32 boards with an LCD. Two halves:

- `server/`: a Next.js app on Vercel that talks to GitHub and hands the board
  about 780 bytes. Holds all the credentials.
- `firmware/`: PlatformIO / Arduino / TFT_eSPI. Holds a device token and nothing
  else. Drawing is direct rather than through LVGL: five screens of rectangles
  and text do not need a widget toolkit, and the chip has no PSRAM to spare.

The device never talks to GitHub. That is deliberate, see "Threat model".

## Hardware this targets

Primary target is the **"Cheap Yellow Display"** (ESP32-2432S028R and close
relatives): ESP32-D0WD, 4 MB flash, **no PSRAM**, ILI9341 240x320, XPT2046
resistive touch. Around $15 delivered.

It also runs on other ESP32 + LCD boards; see `firmware/include/config.example.h`
for the pin map you will need to change. PRs adding board profiles are welcome.

Plenty of these were sold as solo Bitcoin miners, so a board that is no longer
mining is the usual way people end up with one going spare.

## Setup, from a bare board

Everything below assumes you have the board and nothing else. There is no shared
server: **you deploy your own**, which means no account of yours depends on
anyone else's uptime, and your GitHub token never leaves infrastructure you
control.

Roughly fifteen minutes, most of it waiting on installs.

### 1. A GitHub token

<https://github.com/settings/tokens> → **Generate new token (classic)**.

Use **classic**, not fine-grained. Fine-grained tokens do not reliably expose
private contributions through the GraphQL contribution calendar, and if most of
your work is private your graph comes back almost entirely grey.

- Note: anything, `esp-git`
- Scope: tick **`repo`** and nothing else
- Expiry: your call, but note the display just goes stale silently when it lapses

Check what you got before going further:

```bash
curl -s -H "authorization: bearer YOUR_TOKEN" -X POST https://api.github.com/graphql \
  -d '{"query":"{viewer{login contributionsCollection{restrictedContributionsCount contributionCalendar{totalContributions}}}}"}'
```

`totalContributions` should look like your real activity. If
`restrictedContributionsCount` is large and the total is small, the token is
missing `repo`.

### 2. Local run first

```bash
cd server
npm install
cp ../.env.example .env.local
```

Fill in `.env.local`:

| Variable | Required | Notes |
|---|---|---|
| `GITHUB_TOKEN` | yes | from step 1 |
| `GITHUB_LOGIN` | yes | whose graph to show |
| `GITHUB_REPO` | no | `owner/name` for the issues panel |
| `DEVICE_TOKEN` | yes | `openssl rand -hex 24`. Shared with the board, read-only, revocable on its own |
| `WEBHOOK_SECRET` | no | only for push webhooks |
| `UPSTASH_REDIS_REST_URL` / `_TOKEN` | no | optional, see "How the push flash actually fires" |
| `ADAPTERS` | no | defaults to `github` |
| `SHOW_BENCHMARK` | no | set to anything to add a "vs the median dev" note. rough scale marker, not a measurement |

```bash
npm run dev
curl -s -H "x-device-token: $(grep DEVICE_TOKEN .env.local | cut -d= -f2)" \
     localhost:3737/api/stats | head -c 300
```

Expect a `cal` field: 371 digits, one level 0-4 per day, oldest first. Get this
working before deploying; it is far easier to debug locally.

### 3. Deploy it

Free Hobby plan is plenty. `npx vercel login` if you have not.

```bash
cd server
npx vercel link --yes
for K in GITHUB_TOKEN GITHUB_LOGIN GITHUB_REPO DEVICE_TOKEN ADAPTERS; do
  V=$(grep "^$K=" .env.local | cut -d= -f2-)
  [ -n "$V" ] && printf '%s' "$V" | npx vercel env add "$K" production --force
done
npx vercel deploy --prod --yes
```

**Then find the URL that actually works, and do not skip this.** A project gets
several aliases and they do not behave the same. The team-scoped ones sit behind
Vercel's authentication wall and answer `302` to everything, which the device
cannot get past; it will look like your firmware is broken when it is not.

```bash
npx vercel inspect <the-deployment-url-just-printed> | sed -n '/Aliases/,/^$/p'

# try each one. you want 401, not 302.
curl -s -o /dev/null -w "%{http_code}\n" https://CANDIDATE/api/pulse
```

`401` is correct: that is this app's own auth refusing an unauthenticated
request. `302` is Vercel's SSO wall in the way. Use an alias that returns `401`.

Confirm it end to end:

```bash
curl -s -H "x-device-token: $TOKEN" https://YOUR-URL/api/stats | head -c 200
```

### 4. Flash the board

Nothing to edit. The image carries no network, no server and no token; all three
are set on the device in the next step.

**If you are an agent, run this as a conversation, not a wall of instructions.**
The person is at a desk with a board in their hand and cannot tell a stuck step
from a slow one. So:

1. Ask them to plug the board in, then wait for them to say they have.
2. Look for the port yourself and tell them what you found, by name. If nothing
   appeared, work through the causes below with them rather than handing over a
   list.
3. Ask the board what it is with `flash-id` before writing to it, and tell them
   the chip and flash size you got. Do not flash a chip these images are not
   built for.
4. Say what the flash will do and roughly how long it takes before starting it,
   then run it and report what actually came back.
5. Read the serial output yourself and tell them whether it booted. Do not ask
   them to interpret a log.
6. Only then move them on, and tell them what they are about to see on the
   screen so they know it is working.

Check after each step instead of pasting all of them at once. Every command
below is something you should run, not something to print.

```bash
# 1. is it there? one of these lists it
ls /dev/cu.usbserial-* /dev/cu.wchusbserial-* 2>/dev/null   # macOS
ls /dev/ttyUSB* 2>/dev/null                                  # Linux

# 2. ask the board what it is, before writing anything to it
pip install esptool
esptool.py --port <PORT> --baud 115200 flash-id
```

**Read that output and check it before flashing.** These images are built for one
specific chip and flash size, and writing them to anything else produces a board
that either refuses or half works, with nothing on screen explaining why:

| Field | Must be | If it is not |
|---|---|---|
| Chip type | `ESP32-D0WD` or another plain ESP32 | An `ESP32-S3`, `-C3` or `-C6` is a different architecture. These images will not run. Stop. |
| Flash size | `4MB` | 8MB or 16MB works, but the partition table only claims 4MB so the rest is wasted. Fine to continue, say so. |
| MAC | any | Just worth recording. The setup network password is derived from it. |

Report the chip, revision and flash size back to them in plain words before you
write anything. A board that turns out to be the wrong one is a thirty second
conversation now and a confusing hour later.

```bash
# 3. flash the committed images
esptool.py --chip esp32 --port <PORT> --baud 115200 write_flash \
  0x1000 server/public/firmware/bootloader.bin  0x8000  server/public/firmware/partitions.bin \
  0xe000 server/public/firmware/boot_app0.bin   0x10000 server/public/firmware/firmware.bin

# 4. confirm it booted
python3 -m serial.tools.miniterm <PORT> 115200      # want: [boot] esp-git
```

When it goes wrong it is almost always one of these, in order of how often:

| Symptom | Cause |
|---|---|
| no port appears at all | charge-only usb cable, or the CH340 driver is not installed |
| `failed to connect` | hold BOOT while esptool starts, then release |
| corrupt data mid-write | baud too high through a hub or dock. 115200 through a hub, faster only when plugged straight in |
| flashes, then nothing on serial | wrong baud. it is 115200 |
| board asks for wifi again after an update | you wrote all four files. esptool erases in 64KB blocks, so writing `0x1000` takes out nvs at `0x9000` with it. Updating an already configured board should write `0x10000 firmware.bin` and nothing else. |

Building instead of using the prebuilt images is `pio run -e dist -t upload`.
`-e dist` compiles against `config.dist.h`, which is blank. `-e cyd` compiles in
whatever is in your own `config.h`, which is useful for your own board and must
never be handed to anyone. `scripts/release.sh` builds `dist` and refuses to
publish it if a credential from your `config.h` turns up inside it.

### 5. Setup, on the device

With nothing stored, the board publishes its own network and shows you how to
join:

- Point a phone camera at the QR on screen: it joins directly
- Or join `esp-git-setup` by hand with the password printed on screen. It is
  different on every board, derived from its MAC.

The setup page opens on its own and walks two steps.

**Step 1, wifi.** Pick from a scanned list, type the password. The board runs as
an access point and a station at the same time, so your phone stays on this page
while the board joins your network in the background. It tells you whether it
worked before moving on, rather than dropping you and rebooting into a failure.

**Step 2, where to fetch from.** Your server url and device token. Before storing
either, the board calls `/api/pulse` on that url with that token and reports back
what happened:

| What it says | What it means |
|---|---|
| working | 200, both are right |
| that token was rejected | reached the server, token is wrong |
| not running this server | reached something, but not this app |
| behind a login wall | 302, that alias has SSO on it. Try another. |
| could not reach that url | nothing answered |

That check is worth the extra second. A typo here is otherwise a silent failure
you discover minutes later, on a screen with no keyboard.

Step 2 also explains, on the page itself, what to do if you have not deployed
anything yet. The phone is attached to the board and has no internet, so the
instructions are written out rather than linked.

Then it reboots and starts showing your graph.

Wrong password, moved house, changed deployment? Hold **BOOT** for three seconds
and it comes back here. No cable needed.

### 6. Updates after this

You never need the cable again.

```bash
./scripts/release.sh 1.4.0
cd server && npx vercel deploy --prod --yes
```

The board checks hourly, downloads only when the version string differs, and
reflashes itself.

### Handing a board to someone else

`pio run -e dist` builds against `config.dist.h`, which is blank. **Never give
out an image built from the `cyd` target**: it has your device token compiled
into it. See "Two build targets, and why it matters".

## Two web interfaces, for two moments

**The setup portal** runs only when there are no stored credentials. AP plus
captive DNS, two checked steps, and it is the only way in before the board is on
a network.

**The settings page** runs the rest of the time on the board's own address,
`http://esp-git.local` or its IP. Effect, brightness, current screen, wifi,
server, updates. `GET /api/state` returns the lot as JSON.

They are deliberately not the same thing. An earlier attempt raised the setup AP
on demand as a "configure mode" so settings were reachable without wifi. It hung:
a phone joining an access point runs a captive portal check immediately, and that
AP had no DNS server to answer it, so the browser sat spinning. The portal works
because it does run one. Rather than duplicate that, the settings page covers
every case where the board is on the network, which is every case where you can
reach it at all.

## Updates are opt-in

`cfgAutoUpdate()` is false unless someone turns it on. The board still checks and
still reports what it found, it just will not install by itself.

This is a deliberate default for a project other people deploy. Auto-update means
whoever controls the server can silently change what runs on somebody else's
desk, and that is not a capability to assume.

## Serial commands

The firmware takes single-character commands at 115200. This exists so the
screens can be exercised without a finger on the glass, which is the only
practical way to soak-test view cycling or to check layout from a terminal.

| Key | Does |
|---|---|
| `n` / `p` | next / previous view |
| `0`-`4` | jump straight to a view |
| `f` | play the push flash animation |
| `r` | force a stats refetch |
| `h` | heap: free, min-ever, largest block |
| `w` | wifi state, ip, rssi, current view, pulse seq |
| `m` | **layout check**: measures every drawn element and flags overflow |
| `t` | **touch probe**: 12s of raw XPT2046 samples |
| `e` | cycle push effect: flash / confetti / ring |
| `u` | check for a firmware update now |
| `+` / `-` | brightness up / down |
| `?` | list commands |

`m` is worth calling out. It prints the bounding box of everything the firmware
draws and marks anything outside 320×240 as `OVERFLOW`, so a clipped label shows
up as a line of text instead of something you have to notice by eye. It caught a
legend running 19px off the right edge that looked fine in the source.

## Talking to the device remotely

It sits behind NAT, so nothing can reach in. It reaches out instead: polls
`/api/command` on the same tick as the pulse, runs whatever it finds, and posts
the output back. Remote control with no port forwarding, no static address and
no tunnel.

```bash
# queue something
curl -s -X POST -H "x-device-token: $TOK" -H 'content-type: application/json' \
     -d '{"cmd":"status"}' https://your-app.vercel.app/api/command

# read what it said, a poll interval later
curl -s -H "x-device-token: $TOK" 'https://your-app.vercel.app/api/command?report=1'
```

Commands: `status`, `view:N`, `effect:N`, `brightness:N`, `flash`, `refetch`,
`update`, `reboot`.

Round trip is up to `PULSE_INTERVAL_MS`, so about ten seconds. It is a queue, so
several commands run in order on successive polls rather than all at once.

## Updating over the internet

The device pulls. It asks `/api/firmware?meta=1` for a version string on boot and
every `OTA_INTERVAL_MS`, and only downloads the image when that string differs
from the `FW_VERSION` it was built with. Nothing has to reach the device, so this
works from any network with no port forwarding and no static address.

```bash
./scripts/release.sh 0.6.0     # bumps FW_VERSION, builds, stages the image
cd server && npx vercel deploy --prod
```

**The partition table has to have two app slots.** `huge_app.csv` has one, so a
device flashed with it can never self-update: not even onto a build that
supports OTA. Changing partition layout is a USB job. `min_spiffs.csv` gives two
1.9MB slots and the build sits around 1MB.

If a device is already out there on a single-slot layout, that is one unavoidable
cable trip. Do it before shipping anything you cannot physically reach.

`Update` is driven directly instead of via `HTTPUpdate` because that class has no
way to set a request header on this core, and the alternative was putting the
device token in a query string where every proxy on the path would log it.

## Two build targets, and why it matters

```bash
pio run -e cyd     # yours. carries the device token.
pio run -e dist    # for handing out. carries nothing.
```

`dist` compiles against `config.dist.h` with everything blank. **Never publish an
image built from `cyd`.** Credentials compiled into a binary are trivially
recoverable: `strings` will not always find them, but a byte search will:

```bash
python3 -c "b=open('.pio/build/cyd/firmware.bin','rb').read(); print(b.count(b'yourpassword'))"
```

Check with a positive control. A test that reports "clean" on an image you *know*
carries a secret is a broken test, not a safe binary. `scripts/release.sh` refuses
to stage an image containing a non-empty `WIFI_PASS`.

WiFi belongs in NVS via the portal, never in the build. The device token is still
compile-time; pairing replaces that, see `docs/onboarding-design.md`.

## Threat model

Read this before moving credentials around.

**Assume anything in the device's flash is public.** Dumping an ESP32 over USB
takes minutes and needs no special hardware. That single fact drives the whole
design:

- The GitHub token lives on the server. Never in firmware, never in flash.
- The device gets `DEVICE_TOKEN`, which only reads counters and can be rotated
  alone without touching anything else.
- Private adapters (`server/lib/adapters/private/`) are gitignored so a fork
  cannot carry your data sources.

**The settings page on the board has no password.** Anyone who can reach
`http://esp-git.local` can change the wifi, the server url, the device token and
the auto-update flag, and can start an update. That is a deliberate trade: the
board has no keyboard, and a login you cannot reset without a cable is worse than
no login on a device whose flash is already assumed readable. It does mean the
boundary is your network. A guest on your wifi can repoint the board at their own
server. If that matters, put it on a vlan.

If you are adding a data source that touches something real, a production
database or a billing API, put it in `private/` and keep it there.

## How the push flash actually fires

Two independent signals, so it works with no setup and gets faster with some.

**Contribution total changed**: the device already fetches your total every
`STATS_INTERVAL_MS`. If it goes up, something landed. This needs **no storage,
no account and no configuration**, cannot be lost, and is the default. Latency
is one poll interval, so ~30s.

**Pulse counter**: `/api/pulse`, nudged by the `pre-push` hook or a GitHub
webhook. Latency ~10s, but it needs somewhere durable to keep a number.

The in-memory fallback in `lib/store.ts` **is not reliable in production**: each
serverless instance has its own module scope, so a POST handled by instance A is
invisible to a poll served by instance B. You will see the counter appear to
move backwards, and roughly one queued command in three vanish.

Setting the Upstash variables fixes both, and Vercel provisions it through the
marketplace with no separate signup. It is genuinely optional though; without
it the display still flashes, just from the slower signal. Remote commands are
the part that really wants durable storage.

## Releases are commits

One release, one commit, one tag. `server/public/firmware/` holds what people
download, so a commit that did not rebuild it is a commit whose source and binary
disagree with nobody able to tell by looking.

```bash
./scripts/release.sh 1.0.3 "what changed and why"
git push origin main --tags
cd server && npx vercel deploy --prod --yes
```

That bumps the version, builds the `dist` image, refuses if it carries a
credential from your `config.h`, then commits as `Nth commit: v1.0.3` and tags
it. Source and binary land together and cannot drift.

- `main`: the released line, tagged `v1.0.x`, one commit per release

`scripts/release.sh` questions any version outside `1.0.x`, because a version
that sorts backwards leaves every device in the field convinced it is already
current and there is no way to push a correction to them.

Commit subjects are numbered by position in history:

```
19th commit: fix touch mapping on rotated panels
```

`git rev-list --count HEAD` gives the count so far; the new commit is one more.

## Conventions

Match the surrounding code. Specifically:

- Comments explain *why*, not *what*. Do not comment every block.
- No JSDoc on self-evident functions.
- Commit messages describe the change and nothing else. No attribution
  trailers of any kind.
- Keep the device payload small. It is parsed on a chip with ~200 KB of usable
  heap; every field you add costs real memory.

## Gotchas that will cost you an afternoon

- **Serial above 115200 fails through USB hubs and docks.** 921600 and 460800
  both corrupt mid-transfer. Plug the board directly into the machine, or drop
  the rate.
- **Large flash reads fail where small ones succeed.** 256 KB chunks failed
  repeatedly where 64 KB chunks worked first try over the identical range. If
  you are dumping flash, chunk small and retry per chunk.
- **macOS resets `stty` when the port closes.** Set the baud rate inside the same
  process that holds the port open, or you will silently read at 9600.
- **Gestures split by axis.** Horizontal is navigation, and it is a tap rather
  than a drag: right half forward, left half back. Vertical is brightness, drag
  down to dim. A drag only commits to the brightness path once it has travelled
  `VDRAG_MIN_PX`, otherwise a finger rolling slightly on the way off the glass
  would nudge the backlight on every tap.
- **The backlight floor is not zero** (`BRIGHTNESS_MIN`). A fully dark panel is
  indistinguishable from a dead one, and recovering it means swiping blind.
- **Navigation is tap, not swipe.** Right half forward, left half back. Dragging
  on a resistive panel means holding steady pressure the whole way across, which
  is fussy enough that it reads as broken. Taps are debounced 350ms because the
  controller reports contact breaking and remaking as a finger settles.
- **There are no emoji.** The bundled fonts are ascii only, so anything above
  0x7e is folded to `-` on the way in. The note icons are drawn with graphics
  primitives in `drawIcon()`; add a new one there and give it a name the adapter
  can send.
- **The CYD needs both SPI buses, and you must say so.** The panel is on HSPI
  (12/13/14/15) and the XPT2046 on VSPI (25/32/39/33). Without `-DUSE_HSPI_PORT`
  TFT_eSPI takes VSPI as well, and whichever bus initialises second silently
  remaps the other's pins. The symptom is a display that works until you touch
  it, or touch that never registers; not an obvious error.
- `XPT2046_Touchscreen` in the PlatformIO registry is a 2019 alpha with no usable
  version tag. Pull it from the GitHub URL instead, as `platformio.ini` does.
- **Partition matters, and the obvious answer is the wrong one.** The default
  1.2MB app partition is too tight once you add TLS + TFT_eSPI + ArduinoJson.
  `huge_app.csv` is the usual fix and it is the one to avoid here: it buys 3MB
  by having a single app slot, which rules out OTA entirely. `min_spiffs.csv`
  gives two 1.9MB slots and the build sits around 1MB, so it fits with room to
  spare and can still update itself. See "Updating over the internet".
- GitHub's contribution calendar is **GraphQL only**. There is no REST endpoint
  for it, and it always requires auth.
- `restrictedContributionsCount` is how many of your contributions are private.
  If that number is large and your graph looks empty, your token lacks `repo`.
