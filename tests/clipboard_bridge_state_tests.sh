#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_bin="$(mktemp "${TMPDIR:-/tmp}/sc7-clipboard-state.XXXXXXXX")"
trap 'rm -f -- "$test_bin"' EXIT

cc -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    $(pkg-config --cflags wayland-client) \
    "$project_root/tests/clipboard_bridge_state_tests.c" \
    "$project_root/bridge/wlr-data-control-unstable-v1-protocol.c" \
    $(pkg-config --libs wayland-client) -lpthread -o "$test_bin"
ASAN_OPTIONS=detect_leaks=1 "$test_bin"
