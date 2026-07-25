#!/usr/bin/env bash
# Build + run the headless MLS/address desktop peer.
#
# It dlopens the HOST (x86_64) build of this repo's wrapper (liblogoschat.so). The
# wrapper NEEDs liblogosdelivery.so (+ librln.so beside it) — the installed
# Basecamp delivery_module ships an x86_64 pair, which we point LD_LIBRARY_PATH at.
# If you have not built the host wrapper yet, see the note at the bottom.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)

# Host wrapper .so (built via: in the libchat workspace,
#   CARGO_TARGET_DIR=<dir> LOGOS_DELIVERY_LIB_DIR=<host delivery dir> \
#   LOGOS_DELIVERY_RELOCATABLE=1 cargo build --release -p liblogoschat-android )
LIB=${LIB:-/extra/tmp/libchat-mls-build/target-host/release/liblogoschat.so}
# Host delivery node + RLN (x86_64) — the installed Basecamp module ships them.
DELIVERY_DIR=${DELIVERY_DIR:-$HOME/.local/share/Logos/LogosBasecamp/modules/delivery_module}
WORKDIR=${WORKDIR:-/tmp/logoschat-peer}
# The account/keypackage directory. EMPTY here means "use the .so's baked-in
# default" (peer.c passes NULL for an empty value) — do NOT pass an empty string
# through to open_persistent, which treats "" as an invalid URL and fails with
# 'device bundle publish failed: directory: http: builder error' (repo #1).
REGISTRY=${REGISTRY:-}

[ -f "$LIB" ] || { echo "missing host wrapper: $LIB (build it first — see header)"; exit 1; }
[ -f "$DELIVERY_DIR/liblogosdelivery.so" ] || { echo "missing $DELIVERY_DIR/liblogosdelivery.so"; exit 1; }

echo "==> compiling peer"
cc -O2 -o "$HERE/peer" "$HERE/peer.c" -ldl

echo "==> running peer (lib=$LIB delivery=$DELIVERY_DIR work=$WORKDIR)"
export LD_LIBRARY_PATH="$DELIVERY_DIR:${LD_LIBRARY_PATH:-}"
exec "$HERE/peer" "$LIB" "$WORKDIR" "$REGISTRY"
