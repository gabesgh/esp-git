#!/bin/bash
# Wire the pre-push hook up so the display reacts when you push.
#
#   ./scripts/install-hook.sh                       this repo only
#   ./scripts/install-hook.sh --global              every repo on this machine
#
# Worth knowing: hooks live in .git/hooks, which is not tracked and does not
# survive deleting .git. Re-clone or re-init and the display goes quiet with no
# error anywhere. Run this again if that happens.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$HOME/.config/esp-git"

read -r -p "server url [$( [ -r "$CFG/url" ] && sed 's|/api/pulse||' "$CFG/url" )]: " URL
read -r -p "device token: " TOKEN

mkdir -p "$CFG"
if [ -n "$URL" ]; then
    printf '%s/api/pulse' "${URL%/}" > "$CFG/url"
fi
if [ -n "$TOKEN" ]; then
    printf '%s' "$TOKEN" > "$CFG/token"
    chmod 600 "$CFG/token"
fi

[ -r "$CFG/url" ]   || { echo "no server url set"; exit 1; }
[ -r "$CFG/token" ] || { echo "no token set"; exit 1; }

if [ "$1" = "--global" ]; then
    mkdir -p "$HOME/.githooks"
    cp "$ROOT/hooks/pre-push" "$HOME/.githooks/pre-push"
    chmod +x "$HOME/.githooks/pre-push"
    git config --global core.hooksPath "$HOME/.githooks"
    echo "installed for every repo on this machine"
    echo "note: core.hooksPath replaces per-repo hooks rather than adding to them."
    echo "the hook chains to any it finds, so existing ones keep working."
else
    mkdir -p "$ROOT/.git/hooks"
    ln -sf ../../hooks/pre-push "$ROOT/.git/hooks/pre-push"
    chmod +x "$ROOT/hooks/pre-push"
    echo "installed for this repo"
fi

echo
echo "testing it..."
BEFORE=$(curl -s -m 10 -H "x-device-token: $(cat "$CFG/token")" \
         "$(sed 's|/api/pulse|/api/pulse|' "$CFG/url")" | sed -E 's/.*"seq":([0-9]+).*/\1/')
GITHUB_PULSE_HOOK_RAN= "$ROOT/hooks/pre-push" </dev/null || true
sleep 3
AFTER=$(curl -s -m 10 -H "x-device-token: $(cat "$CFG/token")" \
        "$(sed 's|/api/pulse|/api/pulse|' "$CFG/url")" | sed -E 's/.*"seq":([0-9]+).*/\1/')

if [ "$AFTER" -gt "$BEFORE" ] 2>/dev/null; then
    echo "  counter went $BEFORE -> $AFTER. the display will flash on your next push."
else
    echo "  counter did not move ($BEFORE -> $AFTER). check the url and token above."
    exit 1
fi
