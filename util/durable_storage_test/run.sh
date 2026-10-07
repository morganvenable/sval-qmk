#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -Wno-clobbered -O2 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$root/keyboards/svalboard/storage" \
    "$root/util/durable_storage_test/test_store.c" \
    "$root/keyboards/svalboard/storage/store.c" \
    "$root/keyboards/svalboard/storage/legacy.c" -o "$out/test_store"
"$out/test_store"
