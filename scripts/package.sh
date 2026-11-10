#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/dist/opt/cedar-native"

echo "===== PREPARANDO DISTRIBUICAO ====="

mkdir -p \
  "$DEST/v6.0" \
  "$DEST/beta54" \
  "$DEST/jellyfin-test"

install -m 755 \
  "$ROOT/src/jellyfin/cedar-jellyfin-selector.py" \
  "$DEST/v6.0/"

install -m 755 \
  "$ROOT/src/jellyfin/cedar-adapter-v6.0.8.py" \
  "$DEST/v6.0/"

install -m 755 \
  "$ROOT/src/legacy/cedar-adapter-v6.0.7.1.py" \
  "$DEST/v6.0/"

install -m 755 \
  "$ROOT/src/legacy/cedar-adapter-fmp4-v5.4.py" \
  "$DEST/beta54/"

install -m 755 \
  "$ROOT/src/legacy/cedar-adapter.py" \
  "$DEST/jellyfin-test/"

install -m 755 \
  "$ROOT/src/controller/cedar-process-control-v5.4.py" \
  "$DEST/beta54/"

install -m 755 \
  "$ROOT/src/feeder/cedar-feed-ass-v5.4.py" \
  "$DEST/beta54/"

install -m 755 \
  "$ROOT/src/jellyfin/ffmpeg-wrapper" \
  "$DEST/jellyfin-test/ffmpeg"

install -m 755 \
  "$ROOT/build/cedar-mux-ass" \
  "$DEST/beta54/cedar-mux-ass-v5.4"

echo
echo "===== ARQUIVOS DA DISTRIBUICAO ====="
find "$ROOT/dist" -type f | sort

echo
echo "===== INTEGRIDADE DO MUXER ====="
sha256sum "$DEST/beta54/cedar-mux-ass-v5.4"

echo
echo "Distribuicao preparada em: $ROOT/dist"
echo "Nenhum arquivo foi instalado no sistema."
