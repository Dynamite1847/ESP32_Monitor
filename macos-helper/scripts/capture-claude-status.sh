#!/usr/bin/env bash
set -euo pipefail

snapshot_dir="${HOME}/Library/Application Support/DeskConsoleHelper"
snapshot_path="${snapshot_dir}/claude-status.json"
temporary_path="${snapshot_path}.tmp.$$"
input="$(cat)"

mkdir -p "${snapshot_dir}"
trap 'rm -f "${temporary_path}"' EXIT
jq -c '{
  updatedAt: now,
  fiveHour: (.rate_limits.five_hour.used_percentage // null),
  sevenDay: (.rate_limits.seven_day.used_percentage // null)
}' <<<"${input}" >"${temporary_path}"
chmod 600 "${temporary_path}"
mv -f "${temporary_path}" "${snapshot_path}"
trap - EXIT

jq -r '
  if .rate_limits.five_hour.used_percentage != null then
    "Claude  5小时 \(.rate_limits.five_hour.used_percentage | floor)%  ·  7天 \(.rate_limits.seven_day.used_percentage // 0 | floor)%"
  else
    "Claude  用量等待服务端返回"
  end
' <<<"${input}"
