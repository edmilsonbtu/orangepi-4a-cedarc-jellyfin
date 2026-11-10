#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/muxer/cedar-mux-ass-v5.3-beta.c"
OUT="$ROOT/build/cedar-mux-ass"

mkdir -p "$ROOT/build"

echo "===== COMPILANDO CEDARC ====="

gcc -O2 -std=gnu11 \
  -I/usr/include \
  -I"$ROOT/src/muxer" \
  "$SRC" \
  -o "$OUT" \
  -lvdecoder \
  -lvencoder \
  -lvideoengine \
  -lMemAdapter \
  -lVE \
  -lass \
  -ldl \
  -lm \
  -lpthread

echo
echo "===== RESULTADO ====="
file "$OUT"
sha256sum "$OUT"
ldd "$OUT"
