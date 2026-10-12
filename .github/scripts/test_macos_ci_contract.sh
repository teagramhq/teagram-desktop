#!/usr/bin/env bash
set -euo pipefail

mac_workflow="${MACOS_CI_CONTRACT_WORKFLOW:-.github/workflows/mac.yml}"
packaged_workflow=".github/workflows/mac_packaged.yml"
unit_workflow=".github/workflows/macos_unit_feedback.yml"
updater_workflow=".github/workflows/macos_updater_fixtures.yml"
updater_runner=".github/scripts/run_teagram_updater_fixtures.sh"

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

macos_group_for_event() {
  local pr_repo="$1"
  local pr_head="$2"
  local dispatch_pr_repo="$3"
  local dispatch_pr_branch="$4"
  local dispatch_branch="$5"
  local ref_name="$6"
  local repository_qualified_identity
  if [[ -n "$pr_head" ]]; then
    repository_qualified_identity="pr/${pr_repo:-teagramhq/teagram-desktop}/${pr_head}"
  elif [[ -n "$dispatch_pr_branch" ]]; then
    repository_qualified_identity="pr/${dispatch_pr_repo:-teagramhq/teagram-desktop}/${dispatch_pr_branch}"
  else
    repository_qualified_identity="ref/${dispatch_branch:-$ref_name}"
  fi
  printf 'MacOS.-%s\n' "$repository_qualified_identity"
}

assert_pr_dispatch_group() {
  local repository="$1"
  local branch="$2"
  local pr_group
  local dispatch_group
  pr_group="$(macos_group_for_event "$repository" "$branch" '' '' '' '105/merge')"
  dispatch_group="$(macos_group_for_event '' '' "$repository" "$branch" '' 'dev')"
  if [[ "$pr_group" != "$dispatch_group" ]]; then
    printf 'PR and matching dispatch runs for %s/%s must share a concurrency group.\n' "$repository" "$branch" >&2
    exit 1
  fi
}

assert_pr_base_isolation() {
  local branch="$1"
  local pr_group
  local base_group
  pr_group="$(macos_group_for_event 'teagramhq/teagram-desktop' "$branch" '' '' '' '105/merge')"
  base_group="$(macos_group_for_event '' '' '' '' "$branch" 'feature')"
  if [[ "$pr_group" == "$base_group" ]]; then
    printf 'A PR from %s must not share the base branch concurrency group.\n' "$branch" >&2
    exit 1
  fi
}

require_text "$mac_workflow" '  pull_request:'
require_text "$mac_workflow" '  schedule:'
require_text "$mac_workflow" 'concurrency:'
require_text "$mac_workflow" '  pull_request_repository:'
require_text "$mac_workflow" '  pull_request_branch:'
require_text "$mac_workflow" "group: \${{ github.workflow }}-\${{ github.event.pull_request.head.ref && format('pr/{0}/{1}', github.event.pull_request.head.repo.full_name || github.repository, github.event.pull_request.head.ref) || inputs.pull_request_branch && format('pr/{0}/{1}', inputs.pull_request_repository || github.repository, inputs.pull_request_branch) || format('ref/{0}', inputs.branch || github.ref_name) }}"
require_text "$mac_workflow" 'github.event.pull_request.head.repo.full_name'
require_text "$mac_workflow" 'cancel-in-progress: true'
assert_pr_dispatch_group 'teagramhq/teagram-desktop' 'feature/cache-refresh'
assert_pr_dispatch_group 'teagramhq/teagram-desktop' 'dev'
assert_pr_dispatch_group 'teagramhq/teagram-desktop' 'main'
fork_a_group="$(macos_group_for_event 'teagramhq/teagram-desktop' 'topic' '' '' '' '105/merge')"
fork_b_group="$(macos_group_for_event 'steward/tdesktop' 'topic' '' '' '' '105/merge')"
if [[ "$fork_a_group" == "$fork_b_group" ]]; then
  printf 'PRs from different forks with the same branch must use separate groups.\n' >&2
  exit 1
fi
assert_pr_base_isolation 'dev'
assert_pr_base_isolation 'main'
dev_group="$(macos_group_for_event '' '' '' '' 'dev' 'main')"
main_group="$(macos_group_for_event '' '' '' '' 'main' 'dev')"
if [[ "$dev_group" == "$main_group" ]]; then
  printf 'MacOS dev and main runs must remain in separate concurrency groups.\n' >&2
  exit 1
fi
require_text "$mac_workflow" 'CCACHE_MAXSIZE: "5G"'
require_text "$mac_workflow" 'CCACHE_SLOPPINESS: "pch_defines,time_macros"'
cache_setup_step="$(awk '
  /^      - name: First set up\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step { print }
' "$mac_workflow")"
for cache_profile_setting in \
  'protected_path_integration=OFF' \
  'protected_path_integration=ON' \
  'TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=%s'; do
  if ! grep -Fq -- "$cache_profile_setting" <<< "$cache_setup_step"; then
    printf 'MacOS cache setup must include profile setting: %s\n' "$cache_profile_setting" >&2
    exit 1
  fi
done
ccache_restore_step="$(awk '
  /^      - name: Teagram compiler cache\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step { print }
' "$mac_workflow")"
ccache_restore_keys="$(awk '
  /^          restore-keys:/ { in_restore_keys = 1; next }
  in_restore_keys && /^          [^ ]/ { exit }
  in_restore_keys && /^            / { sub(/^            /, ""); print }
' <<< "$ccache_restore_step")"
if grep -Eq 'macOS-arm64-ccache-(pr|dev)-' <<< "$ccache_restore_keys"; then
  printf 'MacOS cache restore keys must not include cross-scope PR/dev fallbacks.\n' >&2
  exit 1
fi
expected_ccache_restore_key='macOS-arm64-ccache-${{ github.event_name == '\''pull_request'\'' && '\''pr'\'' || '\''dev'\'' }}-${{ hashFiles('\''CCACHE_KEY.txt'\'') }}-'
if [[ "$ccache_restore_keys" != "$expected_ccache_restore_key" ]]; then
  printf 'MacOS cache restores must stay in the same PR/dev scope as their saves.\n' >&2
  exit 1
fi
require_text "$mac_workflow" 'CMAKE_C_COMPILER_LAUNCHER=ccache'
require_text "$mac_workflow" 'ccache_launcher.sh'
require_text "$mac_workflow" '-Xclang -fno-pch-timestamp'
require_text "$mac_workflow" '-G "Ninja Multi-Config"'
require_text "$mac_workflow" 'ccache --show-stats'
require_text "$mac_workflow" 'ccache hit rate'
require_text "$mac_workflow" 'ccache_hit_count.sh'
require_text "$mac_workflow" 'ccache_save_decision.sh'
require_text "$mac_workflow" 'A restored compiler cache produced no cache hits.'
require_text "$mac_workflow" '::warning::A restored compiler cache produced no cache hits.'
require_text "$mac_workflow" '::warning::ccache did not record any compiler invocations for the Teagram build.'
reject_text "$mac_workflow" '::error::A restored compiler cache produced no cache hits.'
reject_text "$mac_workflow" '::error::ccache did not record any compiler invocations for the Teagram build.'
require_text "$mac_workflow" '::warning::Warm-cache MacOS PR build exceeded the 30-minute target ('
reject_text "$mac_workflow" '::error::Warm-cache (ccache and libraries) MacOS PR build exceeded the 30-minute limit.'
reject_text "$mac_workflow" "steps.ccache-stats.outcome == 'success'"
require_text "$mac_workflow" "steps.ccache-stats.outputs.save == 'true'"
reject_text "$mac_workflow" 'A warm MacOS PR run must have an exact Libraries cache hit.'
require_text "$mac_workflow" "github.ref == 'refs/heads/dev' || github.event_name == 'pull_request'"
reject_text "$mac_workflow" 'startsWith(steps.cache-ccache.outputs.cache-matched-key'
ccache_stats_step="$(awk '
  /^      - name: Report compiler cache hits\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step { print }
' "$mac_workflow")"
misses_output_line="$(grep -nF 'echo "misses=$misses" >> "$GITHUB_OUTPUT"' <<< "$ccache_stats_step" | cut -d: -f1)"
save_decision_line="$(grep -nF 'ccache_save_decision.sh' <<< "$ccache_stats_step" | cut -d: -f1)"
zero_hit_guard_line="$(grep -nF 'A restored compiler cache produced no cache hits.' <<< "$ccache_stats_step" | cut -d: -f1)"
if [[ -z "$misses_output_line" || -z "$save_decision_line" || -z "$zero_hit_guard_line" ]] \
  || ((misses_output_line >= zero_hit_guard_line || save_decision_line >= zero_hit_guard_line)); then
  printf 'Compiler cache outputs must be written before the zero-hit warning is reported.\n' >&2
  exit 1
fi
if grep -Eq '::error::|exit 1' <<< "$ccache_stats_step"; then
  printf 'Compiler cache diagnostics must not fail a passing source build.\n' >&2
  exit 1
fi
save_cache_step="$(awk '
  /^      - name: Save Teagram compiler cache\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step { print }
' "$mac_workflow")"
if grep -Eq 'cache-(matched-key|hit)' <<< "$save_cache_step"; then
  printf 'A restored PR compiler cache with new misses must remain eligible for saving.\n' >&2
  exit 1
fi
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
require_text "$mac_workflow" 'name: Upload deferred switch mutation evidence.'
require_text "$mac_workflow" 'Deferred switch mutation evidence: mutation=remove-dispatch'
require_text "$mac_workflow" 'Deferred switch mutation evidence: mutation=duplicate-dispatch'
require_text "$mac_workflow" 'elapsed_seconds > 1800'
reject_text "$mac_workflow" 'CCACHE_DISABLE=1'

require_text "$packaged_workflow" '  workflow_dispatch:'
require_text "$packaged_workflow" '  push:'
require_text "$packaged_workflow" 'branches: [dev, main]'
reject_text "$packaged_workflow" '  pull_request:'
reject_text "$packaged_workflow" '  schedule:'
require_text "$packaged_workflow" 'concurrency:'
require_text "$packaged_workflow" 'group: ${{ github.workflow }}-${{ github.event_name == '\''push'\'' && '\''release'\'' || inputs.branch || github.ref_name }}'
require_text "$packaged_workflow" 'cancel-in-progress: false'
require_text "$packaged_workflow" 'queue: max'
require_text "$packaged_workflow" 'openssl pkey -pubin -in "Telegram/build/teagram_update_public_key.pem"'
reject_text "$packaged_workflow" '"$REPO_NAME/Telegram/build/teagram_update_public_key.pem"'
public_key_path="$(sed -n 's/.*openssl pkey -pubin -in "\([^"]*\)".*/\1/p' "$packaged_workflow" | sed -n '1p')"
if [[ -z "$public_key_path" ]] || ! openssl pkey -pubin -in "$public_key_path" -outform DER | shasum -a 256 >/dev/null; then
  printf 'The packaged workflow public key path must work from the cloned repository directory.\n' >&2
  exit 1
fi
require_text "$packaged_workflow" "if: \${{ github.event_name == 'push' && github.repository == 'teagramhq/teagram-desktop' && (github.ref == 'refs/heads/dev' || github.ref == 'refs/heads/main') }}"
require_text "$packaged_workflow" 'environment:'
require_text "$packaged_workflow" '      name: release'
require_text "$packaged_workflow" 'CCACHE_MAXSIZE: "5G"'
require_text "$packaged_workflow" 'Teagram-macOS-arm64-QA'
reject_text "$packaged_workflow" 'CCACHE_DISABLE=1 cmake --build'
reject_text "$packaged_workflow" 'name: Full chat info session regression.'

require_text "$unit_workflow" '  workflow_dispatch:'
reject_text "$unit_workflow" '  pull_request:'
require_text "$unit_workflow" 'concurrency:'
require_text "$unit_workflow" 'group: ${{ github.workflow }}-${{ inputs.branch || github.event.pull_request.head.ref || inputs.ref || github.ref_name }}'
require_text "$unit_workflow" 'cancel-in-progress: true'
require_text "$unit_workflow" 'Telegram/build/prepare/mac.sh'
require_text "$unit_workflow" "steps.cache-libs.outputs.cache-hit != 'true'"

require_text "$updater_workflow" '  pull_request:'
require_text "$updater_workflow" "      - 'Telegram/SourceFiles/core/teagram_update_*'"
require_text "$updater_workflow" "      - 'Telegram/SourceFiles/core/mac_protected_path_access.h'"
require_text "$updater_workflow" "      - 'Telegram/SourceFiles/core/mac_protected_path_runtime.h'"
require_text "$updater_workflow" "      - 'Telegram/SourceFiles/tests/unit/mac_protected_path_runtime_stub.cpp'"
require_text "$updater_workflow" "      - 'Telegram/SourceFiles/tests/unit/mac_file_bookmark_stub.cpp'"
require_text "$updater_workflow" 'ref: ${{ github.event.pull_request.head.sha }}'
require_text "$updater_workflow" 'test "$checked_out_sha" = "$EXPECTED_HEAD_SHA"'
require_text "$updater_workflow" 'runs-on: macos-latest'
require_text "$updater_workflow" 'canonical_tmpdir="$(cd "$TMPDIR" && pwd -P)"'
require_text "$updater_workflow" 'TMPDIR=%s\n'
require_text "$updater_workflow" '>> "$GITHUB_ENV"'
require_text "$updater_workflow" 'timeout-minutes: 180'
require_text "$updater_workflow" 'cancel-in-progress: true'
require_text "$updater_workflow" 'run_teagram_updater_fixtures.sh'
require_text "$updater_workflow" 'actions/upload-artifact@v7'
require_text "$updater_workflow" 'retention-days: 14'
reject_text "$updater_workflow" 'secrets.'
reject_text "$updater_workflow" 'TEAGRAM_UPDATE_PRIVATE_KEY'
require_text "$updater_workflow" 'cmake --build "$REPO_NAME/out"'
require_text "$updater_workflow" '"$REPO_NAME/out/Debug/test_unit"'
reject_text "$updater_workflow" '"$REPO_NAME/Telegram/out'

for fixture in \
  TeagramUpdateArchiveWritesStayConfined \
  TeagramUpdateInstallFailurePreservesApp \
  TeagramUpdateMarkerRejectsHardLinkWithoutTruncatingCanary \
  TeagramUpdateInstallerExercisesAuthenticatedCandidateAndFallback \
  TeagramUpdateInstallerRemovesInterruptedExtraction \
  TeagramUpdateInstallerClearsUncommittedCandidateOnRestart \
  TeagramUpdateReplacementRetainsPreviousThroughStartup \
  TeagramUpdateReplacementRestoresMissingTarget \
  TeagramUpdateDownloadFailsWithoutMutation \
  TeagramUpdateRejectsUntrustedOrIneligiblePackage \
  TeagramUpdateInstallerRejectsUnwritableTargetWithoutMutation \
  TeagramUpdateInstallerRejectsUnsupportedTargetWithoutMutation; do
  require_text "$updater_runner" "$fixture"
  if ! grep -Fq "TEST_CASE($fixture)" \
    Telegram/SourceFiles/tests/unit/teagram_update_download_tests.cpp \
    Telegram/SourceFiles/tests/unit/teagram_update_verification_tests.cpp; then
    printf 'Updater fixture runner names a missing test case: %s\n' "$fixture" >&2
    exit 1
  fi
done
require_text "$updater_runner" '"$unit_binary" --case "$fixture"'
require_text "$updater_runner" 'tail -c "$max_log_bytes"'

require_text README.md '`MacOS.` workflow is the required pull request gate'

printf 'macOS CI contract tests passed.\n'
