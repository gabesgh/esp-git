#!/bin/bash
# Build both images and stage them.
#
#   ./scripts/release.sh 1.0.3
#   ./scripts/release.sh 1.0.3 "what changed and why"
#
# A release is a commit. It bumps the version, builds both images, refuses if the
# published one carries a credential, commits and tags. Source and binary cannot
# drift because they land together.
#
# Two builds, on purpose:
#
#   firmware/bin/             dist. committed. this is what people download.
#   server/firmware-release/  cyd. not committed. what OTA serves your own board.
#
# They differ because a board set up through the portal keeps its token in NVS,
# while a board that was flashed with values compiled in has nothing in NVS to
# fall back to. Sending it a dist image would leave it with no token and no way
# to ask for one. Your board gets the build it expects, everyone else gets the
# blank one.
set -e

VERSION="$1"
[ -z "$VERSION" ] && { echo "usage: $0 <version>   e.g. $0 1.0.3"; exit 1; }

# A version that sorts backwards leaves every board convinced it is already
# current, and updates are pull-only so there is no way to correct them.
case "$VERSION" in
    1.0.*) ;;
    *) printf 'warning: %s is outside the 1.0.x line. ctrl-c to stop, enter to go on: ' "$VERSION"
       read -r _ ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
BIN="$ROOT/server/public/firmware"

sed -i '' "s/#define FW_VERSION \".*\"/#define FW_VERSION \"$VERSION\"/" \
    "$ROOT/firmware/src/main.cpp"

cd "$ROOT/firmware"
"$PIO" run -e dist

BOOT_APP0=$(find "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions" \
            -name boot_app0.bin 2>/dev/null | head -1)
[ -z "$BOOT_APP0" ] && { echo "cannot find boot_app0.bin"; exit 1; }

mkdir -p "$BIN"
cp .pio/build/dist/firmware.bin   "$BIN/firmware.bin"
cp .pio/build/dist/bootloader.bin "$BIN/bootloader.bin"
cp .pio/build/dist/partitions.bin "$BIN/partitions.bin"
cp "$BOOT_APP0"                   "$BIN/boot_app0.bin"

# ---------------------------------------------------------------------------
# The one way publishing binaries goes badly wrong is shipping one with somebody's
# credentials inside. Every string in config.h is compared against the image byte
# for byte and anything found stops the release.
#
# Byte search, not `strings`. strings once reported an image clean that provably
# was not, which is exactly the sort of check that gets trusted and should not be.
# ---------------------------------------------------------------------------
python3 - "$BIN/firmware.bin" "$ROOT/firmware/include/config.h" <<'PY'
import io, re, sys
img = open(sys.argv[1], 'rb').read()
try:
    cfg = io.open(sys.argv[2], encoding='utf-8').read()
except FileNotFoundError:
    cfg = ''

# Only the fields that are actually secret. BOOT_NAME and SETUP_AP_NAME are
# supposed to be in there, and flagging them trains you to ignore this check.
SECRET = ('WIFI_SSID', 'WIFI_PASS', 'DEVICE_TOKEN', 'PULSE_HOST')

bad = [k for k, v in re.findall(r'#define\s+(\w+)\s+"([^"]*)"', cfg)
       if k in SECRET and len(v) >= 6 and v.encode() in img]

if bad:
    print('REFUSING TO RELEASE: the published image contains %s from your config.h'
          % ', '.join(bad))
    print('That would hand it to everyone who downloads it.')
    sys.exit(1)
print('  secret scan: clean')
PY

SIZE=$(stat -f%z "$BIN/firmware.bin")
SHA=$(shasum -a 256 "$BIN/firmware.bin" | cut -d' ' -f1)

# The board this image is for. A device refuses an update whose manifest names a
# different profile, so this is what stops one deployment bricking a board it was
# never built for.
BOARD=$(sed -nE 's/.*BOARD_ID[[:space:]]+"(.+)".*/\1/p' "$ROOT/firmware/include/config.dist.h")
[ -z "$BOARD" ] && { echo "no BOARD_ID in config.dist.h"; exit 1; }

cat > "$BIN/manifest.json" <<EOF
{
  "version": "$VERSION",
  "board": "$BOARD",
  "size": $SIZE,
  "sha256": "$SHA",
  "flash": [
    { "offset": "0x1000",  "file": "bootloader.bin" },
    { "offset": "0x8000",  "file": "partitions.bin" },
    { "offset": "0xe000",  "file": "boot_app0.bin" },
    { "offset": "0x10000", "file": "firmware.bin" }
  ]
}
EOF

# ---------------------------------------------------------------------------
# One release, one commit, one tag. The images in firmware/bin are what people
# download, so a commit without them rebuilt is a commit whose source and binary
# disagree, and nobody can tell by looking. Doing it here means they cannot drift.
# ---------------------------------------------------------------------------
cd "$ROOT"
git add -A

N=$(( $(git rev-list --count HEAD 2>/dev/null || echo 0) + 1 ))
case "$N" in
    *1[123]) SUF=th ;;
    *1) SUF=st ;;
    *2) SUF=nd ;;
    *3) SUF=rd ;;
    *)  SUF=th ;;
esac

SUBJECT="${N}${SUF} commit: v$VERSION"
if [ -n "$2" ]; then
    printf '%s\n\n%s\n' "$SUBJECT" "$2" | git commit -q -F -
else
    git commit -q -m "$SUBJECT"
fi

git tag -f "v$VERSION" -m "v$VERSION" >/dev/null

echo
echo "  $VERSION released as commit $N"
echo "    server/public/firmware/   $(( SIZE / 1024 )) KB, flashed over usb and pulled over the air"
echo
echo "  git push origin main --tags   then   cd server && npx vercel deploy --prod"
