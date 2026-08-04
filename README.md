# esp git

A GitHub contribution display for $15 ESP32 boards. Tap between five screens:
year heatmap, a total that winds up like an odometer, contributions per year, a
repo's open issues, and what the buttons do. It flashes green when a push lands.

Runs on cheap ESP32 boards with a screen on them. A lot of those were sold as
solo Bitcoin miners, so if you have one sitting in a drawer it already has
everything this needs: a display, wifi, and a usb socket.

## How it works

```
   ESP32 + LCD                    Vercel                      GitHub
   ┌──────────────┐   ~780 B     ┌─────────────┐   GraphQL   ┌──────────┐
   │ heatmap      │◄─────────────┤ /api/stats  ├────────────►│ calendar │
   │ odometer     │   every 30s  │ (cached)    │             │ issues   │
   │ per-year     │              │             │             └──────────┘
   │ repo issues  │              │             │
   │ controls     │              │ /api/pulse  │◄─── pre-push hook (optional)
   │              │              │ /api/command│◄─── remote control
   │ green flash  │◄─────────────┤ /api/firmware├──── over-the-air updates
   └──────────────┘              └─────────────┘
```

The device never talks to GitHub and never holds a GitHub token. Dumping an
ESP32's flash over USB takes about five minutes, so the design assumes anything
on the board is public. The board gets one read-only token that reads counters
and nothing else, and WiFi credentials live in NVS rather than the binary.

A rising contribution total is itself the push signal, so the green flash needs
no database, no webhook and no configuration.

The server compresses a year of contributions, 24 KB of GraphQL JSON, down to a
371 character digit string, one level per day. That matters on a chip with no
PSRAM.

## Getting started

**You run your own copy.** There is no shared service to sign up for, nothing of
yours sits on someone else's infrastructure, and your GitHub token never leaves a
deployment you control.

Full walkthrough in [AGENTS.md](AGENTS.md): from a bare board to a working
display, about fifteen minutes. It is written so you can hand the whole repo to a
coding agent and let it do the work, and it reads fine as a human guide too.

### Flashing the board

Prebuilt images are committed in
[`server/public/firmware/`](server/public/firmware). No compiler, no toolchain,
nothing to edit. They are built from a blank config, so they contain nobody's
wifi, nobody's server and nobody's token, and the release script refuses to
publish an image that does.

**Hand this section to a coding agent and it will do the whole thing.** Doing it
yourself:

**1. Plug the board into your computer** with a USB cable. It has to be a data
cable. Charge-only cables are common, look identical, and fail here in a way that
looks like a dead board.

**2. Find the port it appeared on.**

```bash
# macOS
ls /dev/cu.usbserial-* /dev/cu.wchusbserial-*

# Linux
ls /dev/ttyUSB*        # you may need: sudo usermod -aG dialout $USER, then log out and back in

# Windows, in PowerShell
[System.IO.Ports.SerialPort]::getportnames()
```

Nothing listed? Most of these boards use a CH340 usb chip, and macOS and Windows
often need its driver first. Search "CH340 driver", install, replug. On Linux it
is already in the kernel.

**3. Install the flasher and check what the board actually is.**

```bash
pip install esptool
esptool.py --port /dev/cu.usbserial-XXXX --baud 115200 flash-id
```

You want `Chip type: ESP32-D0WD` (or another plain ESP32) and `Detected flash
size: 4MB`. If it says **ESP32-S3**, **-C3** or **-C6**, stop: those are a
different architecture and these images will not run on them. A larger flash size
is fine, the extra just goes unused.

**4. Flash it.** Replace the port with yours.

```bash
esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX --baud 115200 write_flash \
  0x1000  server/public/firmware/bootloader.bin \
  0x8000  server/public/firmware/partitions.bin \
  0xe000  server/public/firmware/boot_app0.bin \
  0x10000 server/public/firmware/firmware.bin
```

It prints `Hash of data verified.` and `Hard resetting` when it worked. Takes
about a minute at this speed.

If it says it cannot connect, hold the **BOOT** button while it starts, then let
go. Some boards need that, most do not.

Those four offsets matter and are recorded in
[`server/public/firmware/manifest.json`](server/public/firmware/manifest.json) with a sha256 you can
check against. All four are only needed the first time: `firmware.bin` alone is
enough once a board is already running this, which is why over-the-air updates
send only that one. A board arriving from miner firmware has a different
partition layout, so the first flash has to replace the lot.

**5. Watch it come up**, optional but reassuring:

```bash
pip install pyserial
python3 -m serial.tools.miniterm /dev/cu.usbserial-XXXX 115200
```

You want `[boot] esp-git`. Ctrl-] to quit.

Building it yourself instead of using the prebuilt images is
`cd firmware && pio run -e dist -t upload`.

### Re-flashing a board that is already set up

`esptool` erases in 64KB blocks, so writing the bootloader at `0x1000` also
erases everything up to `0xFFFF`. The `nvs` partition sits at `0x9000`, inside
that block, which means **a four file flash forgets the wifi, server and token**
and drops the board back to setup.

That is right for a first flash and wrong for an update. To keep the settings,
write only the application:

```bash
esptool.py --chip esp32 --port <PORT> --baud 115200 write_flash \
  0x10000 server/public/firmware/firmware.bin
```

Nothing below `0x10000` changes, so nvs survives and the board comes straight
back up on your network. This is also exactly what an over the air update sends,
which is why those never lose your settings.

Use all four files when the board is new, has come from other firmware, or when
the partition table itself changed.

### The rest of it

1. A GitHub token, classic, `repo` scope only
2. `npm run dev` and check the numbers look right locally
3. `vercel deploy` to your own account, free tier
4. Flash, as above
5. Scan the QR on the screen and fill in the form: wifi, your server, your token

After step 5 the cable is optional forever.

### Changing anything later

The board serves its own settings page while it runs:

```
http://esp-git.local          or   its address on your network
```

Open it from anything on your network. Celebration (with a preview button that
plays it on the screen), brightness, which screen is showing, wifi, server url
and token. The display carries on working while you do.

Updates are **off by default**. The board checks hourly and tells you on that
page when a newer version exists, but it will not install anything until you
press the button or tick the box. Nobody should be able to change what your
board runs without you asking for it.

Holding **BOOT** for three seconds forgets the wifi and returns to first run
setup. That is the escape hatch for when the board cannot reach your network and
the settings page is therefore out of reach too.

## Other data sources

`Adapter` is the only interface that matters:

```ts
export interface Adapter {
  id: string
  label: string
  load(): Promise<Snapshot>
}
```

Return a `heatmap`, a `series`, some `stats`, or any combination, and the device
renders it. GitLab, Linear, your CI, your own metrics; none of that needs the
firmware to change.

Anything private goes in `server/lib/adapters/private/`, which is gitignored, so
forks never carry your data sources and you never have to edit a registry to keep
something out of the repo.

## Hardware

| | |
|---|---|
| Target | ESP32-2432S028R and relatives ("Cheap Yellow Display") |
| Chip | ESP32-D0WD, 4 MB flash, no PSRAM |
| Display | ILI9341 240×320 via TFT_eSPI |
| Touch | XPT2046 resistive. Tap right/left to change screen, drag down to dim |

Other boards need a new pin map in `firmware/include/config.example.h`. PRs
adding profiles are welcome.

## Licence

MIT.
