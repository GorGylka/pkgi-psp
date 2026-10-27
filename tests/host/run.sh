#!/bin/sh
set -eu
: "${PSPSDK_SOURCE:?Set PSPSDK_SOURCE to a checkout of pspdev/pspsdk}"
cd "$(dirname "$0")"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
export ASAN_OPTIONS=detect_leaks=0
cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Istubs -I../../include -I"$PSPSDK_SOURCE/src/net" -I"$PSPSDK_SOURCE/src/utility" \
  test_native.c ../../source/pkgi_native_http.c -o "$build/native"
"$build/native"
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror -fsyntax-only \
  -Istubs -I../../include ../../source/pkgi_download.c
cc -std=gnu11 -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -fsanitize=address,undefined -Istubs -I../../include test_cache.c -o "$build/cache"
"$build/cache"
