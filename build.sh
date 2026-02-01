#!/usr/bin/env bash
set -euo pipefail

cc="${CC:-cc}"
mode="${BUILD_TYPE:-debug}"
llvm_config="${LLVM_CONFIG:-}"
search_dirs=("${HOME}/.local/bin")

if [[ -z "$llvm_config" ]]; then
    for dir in "${search_dirs[@]}"; do
        [[ -x "$dir/llvm-config" ]] && llvm_config="$dir/llvm-config" && break
    done
fi
llvm_config="${llvm_config:-$(command -v llvm-config || true)}"

[[ -z "$llvm_config" ]] && { echo "error: llvm-config not found"; exit 1; }

[[ "${mode,,}" == "release" ]] && flags="-O3 -DNDEBUG" || flags="-O0 -g"

echo "building ($mode) with $llvm_config"

$cc magica.c -o magica -std=c99 -pedantic-errors -Wall -Wextra $flags -lstdc++ -lm \
    $($llvm_config --cflags --ldflags --libs core target analysis native --system-libs)

echo "build complete"
