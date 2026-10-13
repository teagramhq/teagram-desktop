#!/usr/bin/env bash
set -euo pipefail

linux_workflow=".github/workflows/linux.yml"
linux_libraries_workflow=".github/workflows/linux_libraries.yml"
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
require_text "$linux_workflow" 'workflow_run:'
require_text "$linux_workflow" 'workflows: ["Linux libraries."]'
require_text "$linux_workflow" 'packages: read'
require_text "$linux_workflow" 'run: bash .github/scripts/linux_libraries_image.sh pull'
require_text "$linux_workflow" 'name: Report Linux Libraries duration.'
require_text "$linux_workflow" 'name: Report Actions cache total.'
require_text "$linux_workflow" 'under 9000000000 bytes (9 GB)'
reject_text "$linux_workflow" 'Linux-libs-'
reject_text "$linux_workflow" 'buildkit-cache-dance'

require_text "$linux_workflow" "hashFiles('Telegram/build/docker/centos_env/**')"
require_text "$linux_libraries_workflow" "hashFiles('Telegram/build/docker/centos_env/**')"
require_text "$linux_libraries_workflow" 'branches: [dev]'
require_text "$linux_libraries_workflow" '    paths:'
require_text "$linux_libraries_workflow" "      - '!Telegram/build/**'"
require_text "$linux_libraries_workflow" "      - 'Telegram/build/docker/centos_env/**'"
require_text "$linux_libraries_workflow" 'packages: write'
require_text "$linux_libraries_workflow" 'docker/build-push-action@v7'
require_text "$linux_libraries_workflow" 'push: true'
reject_text "$linux_libraries_workflow" '  pull_request:'

require_text "$cache_cleanup_workflow" '  pull_request_target:'
require_text "$cache_cleanup_workflow" '    types: [closed]'
require_text "$cache_cleanup_workflow" '  push:'
require_text "$cache_cleanup_workflow" 'branches: [dev]'
require_text "$cache_cleanup_workflow" '  actions: write'
require_text "$cache_cleanup_workflow" 'gh cache delete --all --succeed-on-no-caches --ref "$cache_ref" --repo "$GITHUB_REPOSITORY"'
require_text "$cache_cleanup_workflow" 'gh cache list --limit 1000 --key macOS-arm64-ccache-pr-'
require_text "$cache_cleanup_workflow" 'gh cache delete "$cache_id" --repo "$GITHUB_REPOSITORY"'
reject_text "$cache_cleanup_workflow" 'actions/checkout@'

printf 'CI cache contract tests passed.\n'
