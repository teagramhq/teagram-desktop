#!/usr/bin/env bash
set -euo pipefail

linux_workflow=".github/workflows/linux.yml"
mac_workflow=".github/workflows/mac.yml"
cache_cleanup_workflow=".github/workflows/pr_cache_cleanup.yml"

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

require_text "$linux_workflow" "matrix.defines || 'default'"
reject_text "$linux_workflow" "matrix.defines || 'null'"
require_text "$linux_workflow" "key: \${{ runner.OS }}-tdesktop-\${{ matrix.defines || 'default' }}-\${{ github.run_id }}"
require_text "$linux_workflow" "restore-keys: \${{ runner.OS }}-tdesktop-\${{ matrix.defines || 'default' }}-"
require_text "$linux_workflow" "DEPENDENCY_HASH: \${{ hashFiles('Telegram/build/docker/centos_env/**') }}"
require_text "$linux_workflow" 'ghcr.io/teagramhq/teagram-desktop-linux'
require_text "$linux_workflow" 'linux-libs-${{ env.DEPENDENCY_HASH }}'
require_text "$linux_workflow" 'packages: write'
require_text "$linux_workflow" 'packages: read'
require_text "$linux_workflow" 'Build dependencies when the image is missing.'
dependency_image_job="$(awk '
  /^  linux-dependency-image:$/ { in_job = 1 }
  /^  linux-build:$/ { in_job = 0 }
  in_job { print }
' "$linux_workflow")"
linux_build_job="$(awk '
  /^  linux-build:$/ { in_job = 1 }
  /^  linux:$/ { in_job = 0 }
  in_job { print }
' "$linux_workflow")"
if ! grep -Fq 'if: github.ref_name == github.event.repository.default_branch' <<< "$dependency_image_job" \
  || ! grep -Fq 'packages: write' <<< "$dependency_image_job" \
  || ! grep -Fq 'packages: read' <<< "$linux_build_job" \
  || grep -Fq 'packages: write' <<< "$linux_build_job"; then
  echo "Only the default-branch dependency image job may publish to GHCR." >&2
  exit 1
fi
reject_text "$linux_workflow" 'name: Libraries cache.'

require_text "$mac_workflow" "CCACHE_MAXSIZE: \${{ github.event_name == 'pull_request' && '300M' || '5G' }}"
require_text "$mac_workflow" 'macOS-arm64-ccache-${{ github.event_name'
require_text "$mac_workflow" "&& 'v2-pr' || 'dev'"
require_text "$mac_workflow" 'name: Enforce PR compiler cache budget.'
require_text "$mac_workflow" 'ccache --cleanup'

if grep -E '^[[:space:]]*(key|restore-keys):.*null' "$linux_workflow" "$mac_workflow"; then
  echo "Linux and macOS cache keys must not contain null." >&2
  exit 1
fi

require_text "$cache_cleanup_workflow" '  pull_request_target:'
require_text "$cache_cleanup_workflow" '    types: [closed]'
require_text "$cache_cleanup_workflow" '  actions: write'
require_text "$cache_cleanup_workflow" 'gh cache delete --all --succeed-on-no-caches --ref "$cache_ref" --repo "$GITHUB_REPOSITORY"'
require_text "$cache_cleanup_workflow" 'macOS-arm64-ccache-pr-'
require_text "$cache_cleanup_workflow" 'Linux-libs-'
reject_text "$cache_cleanup_workflow" 'actions/checkout@'

printf 'CI cache contract tests passed.\n'
