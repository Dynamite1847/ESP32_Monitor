#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/desk-console-tests.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT INT TERM

cc \
    -std=c17 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$project_dir/components/app_model/include" \
    -I"$project_dir/components/privacy/include" \
    -I"$project_dir/components/ble_protocol/include" \
    "$project_dir/tests/host/test_core.c" \
    "$project_dir/components/app_model/app_model.c" \
    "$project_dir/components/app_model/mock_data.c" \
    "$project_dir/components/privacy/privacy_state_machine.c" \
    "$project_dir/components/ble_protocol/ble_protocol.c" \
    -o "$test_dir/test_core"

"$test_dir/test_core"
