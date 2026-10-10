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
require_text "$mac_workflow" 'concurrency:'
require_text "$mac_workflow" "group: \${{ github.workflow }}-\${{ github.event.pull_request.number && format('pr-{0}', github.event.pull_request.number) || format('ref-{0}', inputs.branch || github.ref_name) }}"
require_text "$mac_workflow" 'cancel-in-progress: true'
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
require_text "$mac_workflow" 'A restored compiler cache produced no cache hits.'
reject_text "$mac_workflow" 'A warm MacOS PR run must have an exact Libraries cache hit.'
require_text "$mac_workflow" 'build_dir="$repo/out"'
require_text "$mac_workflow" 'cache_file="$build_dir/CMakeCache.txt"'
require_text "$mac_workflow" 'compile_commands="$build_dir/compile_commands.json"'
require_text "$mac_workflow" 'Debug is missing from CMAKE_CONFIGURATION_TYPES'
require_text "$mac_workflow" 'ninja -C "$build_dir" -f build-Debug.ninja -t commands Telegram'
reject_text "$mac_workflow" 'out/Debug/CMakeCache.txt'
reject_text "$mac_workflow" 'out/Debug/compile_commands.json'
reject_text "$mac_workflow" 'ninja -C "$repo/out/Debug"'
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
require_text "$packaged_workflow" 'concurrency:'
require_text "$packaged_workflow" 'group: ${{ github.workflow }}-${{ inputs.branch || github.event.pull_request.head.ref || github.ref_name }}'
require_text "$packaged_workflow" 'cancel-in-progress: true'
require_text "$packaged_workflow" 'CCACHE_MAXSIZE: "5G"'
reject_text "$packaged_workflow" 'CCACHE_DISABLE=1 cmake --build'
reject_text "$packaged_workflow" 'name: Full chat info session regression.'

require_text "$unit_workflow" '  workflow_dispatch:'
reject_text "$unit_workflow" '  pull_request:'
require_text "$unit_workflow" 'concurrency:'
require_text "$unit_workflow" 'group: ${{ github.workflow }}-${{ inputs.branch || github.event.pull_request.head.ref || inputs.ref || github.ref_name }}'
require_text "$unit_workflow" 'cancel-in-progress: true'
require_text "$unit_workflow" 'Telegram/build/prepare/mac.sh'
require_text "$unit_workflow" "steps.cache-libs.outputs.cache-hit != 'true'"

require_text README.md '`MacOS.` workflow is the required pull request gate'

printf 'macOS CI contract tests passed.\n'
