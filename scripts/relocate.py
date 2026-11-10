#!/usr/bin/env python3
import ast
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist/opt/cedar-native"

if not DIST.is_dir():
    sys.exit("ERRO: execute scripts/package.sh primeiro")

OLD = "/opt/cedar-native"
EXPR = 'os.environ.get("CEDAR_ROOT", "/opt/cedar-native")'

def relocate_python(path):
    source = path.read_text()

    # Substituir apenas literais de caminhos absolutos.
    tree = ast.parse(source)
    lines = source.splitlines(keepends=True)

    edits = []

    for node in ast.walk(tree):
        if not isinstance(node, ast.Constant):
            continue
        if not isinstance(node.value, str):
            continue
        if not node.value.startswith(OLD + "/"):
            continue

        if node.lineno != node.end_lineno:
            raise RuntimeError(
                f"Literal multilinha nao suportado: {path}"
            )

        original = lines[node.lineno - 1]
        start = node.col_offset
        end = node.end_col_offset

        replacement = (
            f'({EXPR} + {node.value[len(OLD):]!r})'
        )

        edits.append((node.lineno - 1, start, end, replacement))

    for lineno, start, end, replacement in sorted(
        edits, reverse=True
    ):
        line = lines[lineno]
        lines[lineno] = (
            line[:start] + replacement + line[end:]
        )

    result = "".join(lines)
    ast.parse(result, filename=str(path))
    path.write_text(result)

    if edits:
        print("RELOCADO:", path.relative_to(DIST))

def relocate_wrapper(path):
    source = path.read_text()

    old = "/opt/cedar-native/v6.0/cedar-jellyfin-selector.py"

    if old not in source:
        raise RuntimeError("Wrapper inesperado")

    source = source.replace(
        old,
        '${CEDAR_ROOT:-/opt/cedar-native}/v6.0/'
        'cedar-jellyfin-selector.py'
    )

    path.write_text(source)
    print("RELOCADO:", path.relative_to(DIST))

for path in sorted(DIST.rglob("*.py")):
    relocate_python(path)

relocate_wrapper(DIST / "jellyfin-test/ffmpeg")

print("Relocacao concluida.")
