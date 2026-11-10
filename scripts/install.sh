#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="$ROOT/dist/opt/cedar-native"

PREFIX="/opt/cedar-native-preview"
APPLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --apply)
            APPLY=1
            shift
            ;;
        --prefix)
            [[ $# -ge 2 ]] || {
                echo "ERRO: --prefix requer um caminho"
                exit 1
            }
            PREFIX="$2"
            shift 2
            ;;
        *)
            echo "ERRO: argumento desconhecido: $1"
            exit 1
            ;;
    esac
done

[[ "$PREFIX" = /* ]] || {
    echo "ERRO: prefixo deve ser absoluto"
    exit 1
}

PREFIX="$(realpath -m -- "$PREFIX")"

case "$PREFIX" in
    /opt/cedar-native-preview)
        ;;
    *)
        echo "ERRO: a preview somente pode ser instalada em /opt/cedar-native-preview"
        exit 1
        ;;
esac

case "$PREFIX" in
    ""|/|/opt|/opt/cedar-native|/opt/cedar-native/*)
        echo "ERRO: destino proibido para proteger a producao"
        exit 1
        ;;
esac

[[ -d "$SOURCE" ]] || {
    echo "ERRO: pacote ausente. Execute scripts/package.sh"
    exit 1
}

echo "===== INSTALADOR CEDARC PREVIEW ====="
echo "Origem: $SOURCE"
echo "Destino: $PREFIX"

if [[ "$APPLY" -eq 0 ]]; then
    echo
    echo "MODO SIMULACAO: nenhuma alteracao sera realizada"
    find "$SOURCE" -type f -printf '%P\n' | sort
    exit 0
fi

[[ "$EUID" -eq 0 ]] || {
    echo "ERRO: --apply requer root"
    exit 1
}

[[ ! -e "$PREFIX" && ! -L "$PREFIX" ]] || {
    echo "ERRO: destino ja existe. Instalacao cancelada."
    exit 1
}

if [[ -L /opt || -L "$PREFIX" ]]; then
    echo "ERRO: link simbolico no destino"
    exit 1
fi

if [[ "$(realpath -e /opt)" != "/opt" ]]; then
    echo "ERRO: /opt nao corresponde ao caminho esperado"
    exit 1
fi

mkdir -- "$PREFIX"
cp -a -- "$SOURCE/." "$PREFIX/"

echo
echo "===== INSTALACAO CONCLUIDA ====="
find "$PREFIX" -type f | sort

echo
echo "ATENCAO: Jellyfin nao foi alterado."
echo "Para executar a preview, configure CEDAR_ROOT e CEDAR_PYTHON."
