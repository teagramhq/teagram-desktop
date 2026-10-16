#!/usr/bin/env bash
set -euo pipefail

linux_workflow=".github/workflows/linux.yml"
cache_cleanup_workflow=".github/workflows/pr_cache_cleanup.yml"
cache_threat_model=".github/ci-cache-threat-model.md"
workflow_lint_workflow=".github/workflows/workflow_lint.yml"
unit_feedback_workflow="${UNIT_FEEDBACK_WORKFLOW:-.github/workflows/unit_feedback.yml}"
macos_unit_feedback_workflow="${MACOS_UNIT_FEEDBACK_WORKFLOW:-.github/workflows/macos_unit_feedback.yml}"

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

# Linux. publishes the dependency image; nothing pulls centos_env:latest.
for retired_workflow in .github/workflows/docker.yml .github/workflows/linux_libraries.yml; do
  if [[ -e "$retired_workflow" ]]; then
    printf '%s must not exist\n' "$retired_workflow" >&2
    exit 1
  fi
done

# The contract tests read the threat model, so editing it must lint, and a PR
# push must lint once (pull_request), not again through an unfiltered push.
if [[ "$(grep -Fxc -- "      - '.github/ci-cache-threat-model.md'" "$workflow_lint_workflow")" != 2 ]]; then
  printf '%s\n' "$workflow_lint_workflow must list .github/ci-cache-threat-model.md under push and pull_request" >&2
  exit 1
fi
if [[ "$(awk 'previous == "  push:" { print; exit } { previous = $0 }' "$workflow_lint_workflow")" != '    branches: [dev]' ]]; then
  printf '%s\n' "$workflow_lint_workflow must run push lint on dev only" >&2
  exit 1
fi

require_text "$linux_workflow" "matrix.defines || 'default'"
reject_text "$linux_workflow" "matrix.defines || 'null'"
require_text "$linux_workflow" "key: \${{ runner.OS }}-tdesktop-\${{ matrix.defines || 'default' }}-\${{ github.run_id }}"
require_text "$linux_workflow" "restore-keys: \${{ runner.OS }}-tdesktop-\${{ matrix.defines || 'default' }}-"
# A workflow_run build checks out its source run's commit but reports against
# the default branch head, and cannot write the dev cache. The post-merge build
# runs on the push, checks out GITHUB_SHA and verifies it before building.
reject_text "$linux_workflow" 'workflow_run'
reject_text "$linux_workflow" 'head_sha'
reject_text "$linux_workflow" '          ref: '
require_text "$linux_workflow" 'packages: read'
linux_triggers="$(awk '
  /^on:$/ { in_on = 1; next }
  in_on && /^[^ #]/ { exit }
  in_on && /^  [a-z_]+:/ { sub(/^  /, ""); sub(/:.*/, ""); print }
' "$linux_workflow" | paste -sd ' ' -)"
if [[ "$linux_triggers" != 'pull_request push schedule workflow_dispatch' ]]; then
  printf '%s must trigger on pull_request, push, schedule and workflow_dispatch only, got: %s\n' "$linux_workflow" "$linux_triggers" >&2
  exit 1
fi
if [[ "$(awk 'previous == "  push:" { print; exit } { previous = $0 }' "$linux_workflow")" != '    branches: [dev]' ]]; then
  printf '%s\n' "$linux_workflow must run its push build on dev only" >&2
  exit 1
fi
require_text "$linux_workflow" "      - 'Telegram/build/docker/centos_env/**'"
require_text "$linux_workflow" "cancel-in-progress: \${{ github.ref != 'refs/heads/dev' }}"
verify_line="$(grep -nFx '        run: bash .github/scripts/verify_built_sha.sh "$GITHUB_SHA"' "$linux_workflow" | cut -d: -f1)"
build_line="$(grep -nFx '      - name: Telegram Desktop build.' "$linux_workflow" | cut -d: -f1)"
if [[ -z "$verify_line" || -z "$build_line" ]] || (( verify_line > build_line )); then
  printf '%s must verify the checked-out commit is GITHUB_SHA before the build\n' "$linux_workflow" >&2
  exit 1
fi

# Only a trusted dev run writes the Linux compiler cache.
for step_name in 'Trim Telegram Desktop cache.' 'Save Telegram Desktop cache.'; do
  step_guard="$(awk -v step="      - name: ${step_name}" '
    $0 == step { in_step = 1; next }
    in_step && /^      - / { exit }
    in_step && /^        if: >$/ { in_if = 1; next }
    in_if && /^          / { sub(/^ +/, ""); printf "%s ", $0; next }
    in_if { exit }
  ' "$linux_workflow")"
  expected_guard="env.ONLY_CACHE == 'false' && github.event_name != 'pull_request' && github.ref == 'refs/heads/dev' && fromJSON(steps.ccache-stats.outputs.misses) > 300 "
  if [[ "$step_guard" != "$expected_guard" ]]; then
    printf '%s step %s must be gated with: %s\n' "$linux_workflow" "$step_name" "$expected_guard" >&2
    exit 1
  fi
done
if [[ "$(grep -cF 'actions/cache/save@' "$linux_workflow")" != 1 ]]; then
  printf '%s must save the Linux compiler cache from one gated step\n' "$linux_workflow" >&2
  exit 1
fi
reject_text "$linux_workflow" 'actions/cache@'
reject_text "$linux_workflow" 'cache-to:'

# The dev push publishes this commit's dependency image before its build pulls
# it. Only that job may write packages.
linux_job() {
  awk -v job="  $1:" '
    $0 == job { in_job = 1; print; next }
    in_job && /^  [^ ]/ { exit }
    in_job { print }
  ' "$linux_workflow"
}
libraries_job="$(linux_job linux-libraries)"
build_job="$(linux_job linux-build)"
if ! grep -Fqx "    if: github.event_name == 'push' && github.ref == 'refs/heads/dev' && github.repository == 'teagramhq/teagram-desktop'" <<< "$libraries_job"; then
  printf '%s linux-libraries must run only for dev pushes in this repository\n' "$linux_workflow" >&2
  exit 1
fi
for expected in 'packages: write' 'docker/build-push-action@v7' 'push: true' "hashFiles('Telegram/build/docker/centos_env/**')"; do
  if ! grep -Fq -- "$expected" <<< "$libraries_job"; then
    printf '%s linux-libraries must contain: %s\n' "$linux_workflow" "$expected" >&2
    exit 1
  fi
done
if [[ "$(grep -cF 'packages: write' "$linux_workflow")" != 1 || "$(grep -cF 'push: true' "$linux_workflow")" != 1 ]]; then
  printf '%s must publish and write packages only from linux-libraries\n' "$linux_workflow" >&2
  exit 1
fi
if ! grep -Fqx '      - linux-libraries' <<< "$build_job" \
    || ! grep -Fq "(needs.linux-libraries.result == 'success' || needs.linux-libraries.result == 'skipped')" <<< "$build_job"; then
  printf '%s linux-build must wait for linux-libraries and stop when it fails\n' "$linux_workflow" >&2
  exit 1
fi
require_text "$linux_workflow" 'Trust boundary and token scope: see .github/ci-cache-threat-model.md.'
require_text "$linux_workflow" 'run: bash .github/scripts/linux_libraries_image.sh pull'
require_text "$linux_workflow" 'name: Report Linux Libraries duration.'
require_text "$linux_workflow" 'name: Report Actions cache total.'
require_text "$linux_workflow" 'under 9000000000 bytes (9 GB)'
reject_text "$linux_workflow" 'Linux-libs-'
reject_text "$linux_workflow" 'buildkit-cache-dance'

require_text "$linux_workflow" "hashFiles('Telegram/build/docker/centos_env/**')"

require_text "$cache_cleanup_workflow" '  pull_request_target:'
require_text "$cache_cleanup_workflow" '    types: [closed]'
reject_text "$cache_cleanup_workflow" '  push:'
require_text "$cache_cleanup_workflow" 'branches: [dev]'
require_text "$cache_cleanup_workflow" '  workflow_run:'
require_text "$cache_cleanup_workflow" 'workflows: ["MacOS.", "MacOS Packaged.", "Linux."]'
require_text "$cache_cleanup_workflow" 'Trust boundary and token scope: see .github/ci-cache-threat-model.md.'
require_text "$cache_cleanup_workflow" '      actions: write'
require_text "$cache_cleanup_workflow" '[[ ! "$PR_NUMBER" =~ ^[0-9]+$ ]]'
require_text "$cache_cleanup_workflow" 'gh cache delete --all --succeed-on-no-caches --ref "$cache_ref" --repo "$GITHUB_REPOSITORY"'
reject_text "$cache_cleanup_workflow" 'macOS-arm64-ccache-pr-'
require_text "$cache_cleanup_workflow" 'gh cache delete "$cache_id" --repo "$GITHUB_REPOSITORY"'
require_text "$cache_cleanup_workflow" 'SOURCE_BRANCH: ${{ github.event.workflow_run.head_branch }}'
require_text "$cache_cleanup_workflow" 'SOURCE_REPOSITORY: ${{ github.event.workflow_run.head_repository.full_name }}'
require_text "$cache_cleanup_workflow" '[[ "$SOURCE_BRANCH" != "dev" || "$SOURCE_REPOSITORY" != "$GITHUB_REPOSITORY" ]]'
require_text "$cache_cleanup_workflow" "cache_prefix='macOS-arm64-ccache-dev-'"
require_text "$cache_cleanup_workflow" '--ref refs/heads/dev'
require_text "$cache_cleanup_workflow" 'pr-compatible'
require_text "$cache_cleanup_workflow" "if: github.event_name == 'workflow_run' && github.event.workflow_run.name != 'Linux.'"
require_text "$cache_cleanup_workflow" "if: github.event_name == 'workflow_run' && github.event.workflow_run.name == 'Linux.'"
require_text "$cache_cleanup_workflow" "cache_prefix='Linux-tdesktop-default-'"
reject_text "$cache_cleanup_workflow" 'actions/checkout@'
# Feedback lanes run on dev against a selected feature ref, so they carry the
# dev cache scope. The cache service, not skipped steps, keeps them read-only.
for feedback_workflow in "$unit_feedback_workflow" "$macos_unit_feedback_workflow"; do
  if ! grep -Eq '^cache-mode: read$' "$feedback_workflow"; then
    printf '%s must set workflow-level cache-mode: read\n' "$feedback_workflow" >&2
    exit 1
  fi
  if grep -E '^[[:space:]]*cache-mode:' "$feedback_workflow" \
      | grep -Evq '^[[:space:]]*cache-mode:[[:space:]]*(read|none)[[:space:]]*$'; then
    printf '%s must not grant a job cache write access.\n' "$feedback_workflow" >&2
    exit 1
  fi
  reject_text "$feedback_workflow" 'actions/cache@'
  reject_text "$feedback_workflow" 'actions/cache/save@'
  reject_text "$feedback_workflow" 'cache-to:'
  reject_text "$feedback_workflow" 'type=gha'
done

require_text "$unit_feedback_workflow" 'packages: read'
reject_text "$unit_feedback_workflow" 'packages: write'
reject_text "$unit_feedback_workflow" 'push: true'
require_text "$unit_feedback_workflow" "hashFiles('Telegram/build/docker/centos_env/**')"
require_text "$unit_feedback_workflow" 'LINUX_LIBRARIES_IMAGE=ghcr.io/teagramhq/teagram-desktop-linux-libraries:${DEPENDENCY_HASH}'
require_text "$linux_workflow" 'LINUX_LIBRARIES_IMAGE=ghcr.io/teagramhq/teagram-desktop-linux-libraries:${DEPENDENCY_HASH}'
require_text "$unit_feedback_workflow" 'run: bash "$RUNNER_TEMP/linux_libraries_image.sh" pull "$LINUX_LIBRARIES_IMAGE" "$IMAGE_TAG" "$GITHUB_OUTPUT"'
unit_libraries_guard="$(awk '
  /^      - name: Libraries\.$/ { in_step = 1; next }
  in_step && /^      - / { exit }
  in_step && /^        if: / { sub(/^        if: /, ""); print }
' "$unit_feedback_workflow")"
if [[ "$unit_libraries_guard" != "steps.pull-libraries.outputs.cache-hit != 'true'" ]]; then
  printf '%s must build Libraries only when the GHCR image is missing.\n' "$unit_feedback_workflow" >&2
  exit 1
fi

reject_text "$macos_unit_feedback_workflow" 'brew upgrade'
reject_text "$macos_unit_feedback_workflow" 'brew update'
reject_text "$macos_unit_feedback_workflow" '|| true'
reject_text "$macos_unit_feedback_workflow" 'sed -i'
require_text "$macos_unit_feedback_workflow" 'HOMEBREW_NO_AUTO_UPDATE=1 brew install "${missing[@]}"'

require_text "$cache_threat_model" 'Pull request workflows cannot invoke its write'
require_text "$cache_threat_model" '## Trusted refs and Linux dependency image publisher'
require_text "$cache_threat_model" 'numeric PR number, validated'
require_text "$cache_threat_model" 'actions: write'
require_text "$cache_threat_model" 'PR-controlled workflow runs fail the source validation'
require_text "$cache_threat_model" '## Post-merge Linux build'
require_text "$cache_threat_model" 'never from `workflow_run`'
require_text "$cache_threat_model" '## Manual feedback lanes'
require_text "$cache_threat_model" 'cache-mode: read'

bash .github/scripts/test_prune_macos_dev_ccache.sh
bash .github/scripts/test_prune_linux_dev_ccache.sh

printf 'CI cache contract tests passed.\n'
