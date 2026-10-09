#!/usr/bin/env bash
set -euo pipefail

mac_workflow=".github/workflows/mac.yml"
packaged_workflow=".github/workflows/mac_packaged.yml"
unit_workflow=".github/workflows/macos_unit_feedback.yml"

require_text() {
  local file="$1"
  local expected="$2"
  if ! grep -Fq -- "$expected" "$file"; then
    printf '%s must contain: %s\n' "$file" "$expected" >&2
    exit 1
  fi
}

reject_text() {
  local file="$1"
  local unexpected="$2"
  if grep -Fq -- "$unexpected" "$file"; then
    printf '%s must not contain: %s\n' "$file" "$unexpected" >&2
    exit 1
  fi
}

require_text "$mac_workflow" '  pull_request:'
require_text "$mac_workflow" '  schedule:'
require_text "$mac_workflow" 'github.event.pull_request.number'
require_text "$mac_workflow" 'macOS-arm64-ccache-pr-'
require_text "$mac_workflow" 'macOS-arm64-ccache-dev-'
require_text "$mac_workflow" 'CCACHE_MAXSIZE: "5G"'
require_text "$mac_workflow" 'CCACHE_SLOPPINESS: "pch_defines,time_macros"'
require_text "$mac_workflow" 'CMAKE_C_COMPILER_LAUNCHER=ccache'
require_text "$mac_workflow" 'ccache_launcher.sh'
require_text "$mac_workflow" '-Xclang -fno-pch-timestamp'
require_text "$mac_workflow" '-G "Ninja Multi-Config"'
require_text "$mac_workflow" 'ccache --show-stats'
require_text "$mac_workflow" 'ccache hit rate'
require_text "$mac_workflow" 'ccache_hit_count.sh'
if ! awk '
  /^      - name: Report compiler cache hits\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step && /BUILD_OUTCOME: \$\{\{ steps\.build\.outcome \}\}/ { found = 1 }
  END { exit !found }
' "$mac_workflow"; then
  printf 'Report compiler cache hits must receive BUILD_OUTCOME from the build step.\n' >&2
  exit 1
fi
require_text "$mac_workflow" 'Telegram/build/prepare/prepare.py'
require_text "$mac_workflow" 'Telegram/build/prepare/mac.sh'
require_text "$mac_workflow" 'Telegram/build/qt_version.py'
require_text "$mac_workflow" "steps.cache-libs.outputs.cache-hit != 'true'"
require_text "$mac_workflow" 'name: Full chat info session regression.'
require_text "$mac_workflow" "if: github.event_name != 'pull_request'"
require_text "$mac_workflow" 'elapsed_seconds > 1800'
reject_text "$mac_workflow" 'CCACHE_DISABLE=1'

require_text "$packaged_workflow" '  workflow_dispatch:'
reject_text "$packaged_workflow" '  pull_request:'
reject_text "$packaged_workflow" '  schedule:'
require_text "$packaged_workflow" 'CCACHE_MAXSIZE: "5G"'
reject_text "$packaged_workflow" 'CCACHE_DISABLE=1 cmake --build'
reject_text "$packaged_workflow" 'name: Full chat info session regression.'

require_text "$unit_workflow" '  workflow_dispatch:'
reject_text "$unit_workflow" '  pull_request:'
require_text "$unit_workflow" 'Telegram/build/prepare/mac.sh'
require_text "$unit_workflow" "steps.cache-libs.outputs.cache-hit != 'true'"

require_text README.md '`MacOS.` workflow is the required pull request gate'

printf 'macOS CI contract tests passed.\n'
