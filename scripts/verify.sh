#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "===== SISTEMA ====="
uname -a
cat /etc/os-release | head -6

echo
echo "===== DRIVER CEDARC ====="
if [[ -c /dev/cedar_dev ]]; then
    ls -l /dev/cedar_dev
else
    echo "ERRO: /dev/cedar_dev ausente"
    exit 1
fi

echo
echo "===== HEADERS ====="
for h in \
    memoryAdapter.h \
    veAdapter.h \
    veInterface.h \
    sc_interface.h \
    vdecoder.h \
    vencoder.h
do
    test -f "/usr/include/$h" || {
        echo "ERRO: header ausente: $h"
        exit 1
    }
    echo "OK: $h"
done

echo
echo "===== BIBLIOTECAS ====="
for lib in \
    libvdecoder.so \
    libvencoder.so \
    libvideoengine.so \
    libMemAdapter.so \
    libVE.so
do
    if [[ -x /usr/sbin/ldconfig ]]; then
        LDCONFIG=/usr/sbin/ldconfig
    elif command -v ldconfig >/dev/null 2>&1; then
        LDCONFIG="$(command -v ldconfig)"
    else
        echo "ERRO: ldconfig indisponivel"
        exit 1
    fi

    if ! "$LDCONFIG" -p | grep -Fq "$lib"; then
        echo "ERRO: biblioteca ausente: $lib"
        exit 1
    fi

    echo "OK: $lib"
done

echo
echo "===== PYTHON / PYAV ====="
/opt/cedar-native/venv/bin/python - <<'PY'
import sys
import av
print("Python:", sys.version.split()[0])
print("PyAV:", av.__version__)
PY

echo
echo "===== INTEGRIDADE ====="
(
    cd "$ROOT"
    sha256sum -c SHA256SUMS
)

echo
echo "VERIFICACAO CONCLUIDA"
