#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
idf_dir=${DESK_IDF_PATH:-"${HOME}/esp/v5.5.5/esp-idf"}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/desk-console-tests.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT INT TERM

if [ ! -f "$idf_dir/components/json/cJSON/cJSON.c" ]; then
    echo "Missing cJSON source under ESP-IDF: $idf_dir" >&2
    exit 1
fi

cc \
    -std=c17 \
    -Wall \
    -Wextra \
    -Werror \
    -I"$project_dir/components/app_model/include" \
    -I"$project_dir/components/privacy/include" \
    -I"$project_dir/components/ble_protocol/include" \
    -I"$project_dir/components/storage/include" \
    -I"$project_dir/components/weather" \
    -I"$project_dir/components/market" \
    -I"$idf_dir/components/json/cJSON" \
    "$project_dir/tests/host/test_core.c" \
    "$project_dir/components/app_model/app_model.c" \
    "$project_dir/components/app_model/mock_data.c" \
    "$project_dir/components/privacy/privacy_state_machine.c" \
    "$project_dir/components/ble_protocol/ble_protocol.c" \
    "$project_dir/components/storage/log_policy.c" \
    "$project_dir/components/weather/qweather_parser.c" \
    "$project_dir/components/market/market_parser.c" \
    "$idf_dir/components/json/cJSON/cJSON.c" \
    -lm \
    -o "$test_dir/test_core"

"$test_dir/test_core"
