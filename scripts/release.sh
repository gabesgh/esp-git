#!/bin/bash
# Build the distributable image and stage it.
#
#   ./scripts/release.sh 1.0.3
#   ./scripts/release.sh 1.0.3 "what changed and why"
#
# A release is a commit. It bumps the version, builds the dist image, refuses if
# that image carries a credential, commits and tags. Source and binary cannot
# drift because they land together.
#
# Only the `dist` target is built, into server/public/firmware/. That is what
# people download over usb and what every board pulls over the air, and it is
# built against config.dist.h so it carries nobody's wifi, server or token.
#
# One case this does not cover: a board you flashed yourself from `-e cyd` has
# its credentials compiled in rather than stored in NVS, so handing it a dist
# image leaves it with no token and no way to ask for one. If you have such a
# board, reflash it through the setup portal once so its settings live in NVS,
# and it can follow the same releases as everyone else.
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

# not `sed -i`: the in-place flag takes an argument on BSD and does not on GNU,
# so any spelling of it breaks on one of the two platforms.
python3 - "$ROOT/firmware/src/main.cpp" "$VERSION" <<'PY'
import io, re, sys
p, ver = sys.argv[1], sys.argv[2]
s = io.open(p, encoding="utf-8").read()
s, n = re.subn(r'#define FW_VERSION "[^"]*"', '#define FW_VERSION "%s"' % ver, s)
if n != 1:
    sys.exit("expected one FW_VERSION define in %s, found %d" % (p, n))
io.open(p, "w", encoding="utf-8").write(s)
PY

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

# `stat` and the sha tool are both spelled differently on macOS and linux.
SIZE=$(wc -c < "$BIN/firmware.bin" | tr -d ' ')
if command -v sha256sum >/dev/null 2>&1; then
    SHA=$(sha256sum "$BIN/firmware.bin" | cut -d' ' -f1)
else
    SHA=$(shasum -a 256 "$BIN/firmware.bin" | cut -d' ' -f1)
fi

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
# One release, one commit, one tag. The images in server/public/firmware are what
# people download, so a commit without them rebuilt is a commit whose source and
# binary disagree, and nobody can tell by looking. Doing it here means they cannot
# drift.
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

# Wrap the body. Passing it through verbatim gives you one enormous line that
# git will not fold, so it reads fine in a terminal that soft wraps and badly
# everywhere else.
if [ -n "$2" ]; then
    { printf '%s\n\n' "$SUBJECT"
      printf '%s' "$2" | python3 -c "
import sys, textwrap
for para in sys.stdin.read().split('\n\n'):
    para = ' '.join(para.split())
    if para:
        print(textwrap.fill(para, 78))
        print()
"
    } | git commit -q -F -
else
    git commit -q -m "$SUBJECT"
fi

git tag -f "v$VERSION" -m "v$VERSION" >/dev/null

echo
echo "  $VERSION released as commit $N"
echo "    server/public/firmware/   $(( SIZE / 1024 )) KB, flashed over usb and pulled over the air"
echo
echo "  git push origin main --tags   then   cd server && npx vercel deploy --prod"
