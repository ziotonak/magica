#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
tmp=$(mktemp)
trap 'rm -f "$tmp"' exit
p=0; f=0

check() {
    printf "  %-35s " "$(basename "$1")"
    local s=0

    { ./magica "$1" "$tmp" && timeout "${TIMEOUT:-2}" "$tmp"; } &>/dev/null || s=1

    if [[ $s -eq $2 ]]; then
        echo "pass"; p=$((p+1))
    else
        echo "fail"; f=$((f+1))
    fi
}

echo "building..."; ./build.sh >/dev/null
echo "testing..."
shopt -s nullglob
for x in "${RUN_DIR:-tests/run}"/*.magi; do check "$x" 0; done
for x in "${ERR_DIR:-tests/error}"/*.magi; do check "$x" 1; done

echo "result: $p passed, $f failed"
[[ $f -eq 0 ]]
