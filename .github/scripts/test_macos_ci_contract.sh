#!/usr/bin/env bash
set -euo pipefail

mac_workflow="${MACOS_CI_CONTRACT_WORKFLOW:-.github/workflows/mac.yml}"
packaged_workflow=".github/workflows/mac_packaged.yml"
approval_workflow=".github/workflows/mac_pr_approval.yml"
unit_workflow=".github/workflows/macos_unit_feedback.yml"
updater_workflow=".github/workflows/macos_updater_fixtures.yml"
updater_runner=".github/scripts/run_teagram_updater_fixtures.sh"
ccache_profile_script=".github/scripts/mac_ccache_profile.sh"

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
  local event_name="${7:-}"
  local repository_qualified_identity
  if [[ -n "$pr_head" ]]; then
    repository_qualified_identity="pr/${pr_repo:-teagramhq/teagram-desktop}/${pr_head}"
  elif [[ -n "$dispatch_pr_branch" ]]; then
    repository_qualified_identity="pr/${dispatch_pr_repo:-teagramhq/teagram-desktop}/${dispatch_pr_branch}"
  elif [[ "$event_name" == 'schedule' ]]; then
    repository_qualified_identity="schedule/${ref_name}"
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
require_text "$mac_workflow" "group: \${{ github.workflow }}-\${{ github.event.pull_request.head.ref && format('pr/{0}/{1}', github.event.pull_request.head.repo.full_name || github.repository, github.event.pull_request.head.ref) || inputs.pull_request_branch && format('pr/{0}/{1}', inputs.pull_request_repository || github.repository, inputs.pull_request_branch) || github.event_name == 'schedule' && format('schedule/{0}', github.ref_name) || format('ref/{0}', inputs.branch || github.ref_name) }}"
require_text "$mac_workflow" 'github.event.pull_request.head.repo.full_name'
require_text "$mac_workflow" "cancel-in-progress: \${{ github.event_name != 'pull_request' || github.event.label.name == 'macos-build' }}"
pr_types="$(sed -n '/^  pull_request:$/,/^  [a-z_]*:$/p' "$mac_workflow" | sed -n 's/^    types: //p')"
if [[ "$pr_types" != '[labeled]' ]]; then
  printf 'MacOS PR runs must start only from the macos-build label, got: %s\n' "${pr_types:-every push}" >&2
  exit 1
fi
require_text "$mac_workflow" "if: \${{ !inputs.libiconv_only && (github.event_name != 'pull_request' || github.event.label.name == 'macos-build') }}"
reject_text "$mac_workflow" 'pull-requests: write'
require_text "$approval_workflow" '  pull_request_target:'
require_text "$approval_workflow" '    branches: [dev]'
require_text "$approval_workflow" '    types: [synchronize]'
require_text "$approval_workflow" '      actions: write'
require_text "$approval_workflow" '      pull-requests: write'
reject_text "$approval_workflow" 'head.repo.full_name == github.repository'
reject_text "$approval_workflow" 'actions/checkout'
reject_text "$approval_workflow" "contains(github.event.pull_request.labels"
approval_step="$(awk '
  /- name: Cancel superseded MacOS. builds and clear their approval./ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step && /^        run: \|$/ { in_run = 1; next }
  in_run { sub(/^          /, ""); print }
' "$approval_workflow")"
if [[ -z "$approval_step" ]]; then
  printf 'Could not extract the macOS approval step from %s.\n' "$approval_workflow" >&2
  exit 1
fi
approval_fixture="$(mktemp -d)"
trap 'rm -rf "$approval_fixture"' EXIT
cat > "$approval_fixture/gh" <<'FAKE_GH'
#!/usr/bin/env bash
if [[ "$*" == *'/actions/workflows/mac.yml/runs'* ]]; then
  cat "$FAKE_GH_RUNS"
  exit 0
fi
printf '%s\n' "$*" >> "$FAKE_GH_LOG"
if [[ "$*" =~ /actions/runs/([0-9]+)/jobs ]]; then
  cat "$FAKE_GH_JOBS_DIR/${BASH_REMATCH[1]}.json" 2>/dev/null || printf '%s' '{"jobs":[]}'
  exit 0
fi
case "$*" in
  *'/runs/11/cancel'*) printf 'gh: Cannot cancel a workflow run that is completed. (HTTP 409)\n' >&2; exit 1 ;;
  *'/runs/12/cancel'*) [[ "$FAKE_GH_CANCEL" == ok ]] && exit 0; printf 'gh: Resource not accessible by integration (HTTP 403)\n' >&2; exit 1 ;;
  *'/labels/macos-build'*) [[ "$FAKE_GH_LABEL" == present ]] && exit 0; printf 'gh: Label does not exist (HTTP 404)\n' >&2; exit 1 ;;
esac
exit 0
FAKE_GH
chmod +x "$approval_fixture/gh"
old_head_runs='{"id":11,"status":"in_progress","head_sha":"old1","head_repository":{"full_name":"fork/teagram-desktop"}},{"id":12,"status":"queued","head_sha":"old2","head_repository":{"full_name":"fork/teagram-desktop"}},{"id":13,"status":"in_progress","head_sha":"old1","head_repository":{"full_name":"other/teagram-desktop"}},{"id":14,"status":"completed","head_sha":"old1","head_repository":{"full_name":"fork/teagram-desktop"}}'
current_head_run='{"id":15,"status":"in_progress","head_sha":"new","head_repository":{"full_name":"fork/teagram-desktop"}}'
skipped_head_run='{"id":16,"status":"completed","head_sha":"new","head_repository":{"full_name":"fork/teagram-desktop"}}'
pending_head_run='{"id":17,"status":"queued","head_sha":"new","head_repository":{"full_name":"fork/teagram-desktop"}}'
printf '%s' '{"jobs":[{"name":"MacOS (non-store)","status":"in_progress","conclusion":null}]}' > "$approval_fixture/15.json"
printf '%s' '{"jobs":[{"name":"MacOS","status":"completed","conclusion":"skipped"},{"name":"Focused libiconv (macOS arm64)","status":"completed","conclusion":"skipped"}]}' > "$approval_fixture/16.json"
run_approval_step() {
  printf '{"workflow_runs":[%s]}' "$1" > "$approval_fixture/runs.json"
  : > "$approval_fixture/calls"
  PATH="$approval_fixture:$PATH" FAKE_GH_LOG="$approval_fixture/calls" FAKE_GH_RUNS="$approval_fixture/runs.json" \
    FAKE_GH_CANCEL="$2" FAKE_GH_LABEL="$3" FAKE_GH_JOBS_DIR="$approval_fixture" MACOS_APPROVAL_POLL_SECONDS=0 \
    GITHUB_REPOSITORY=teagramhq/teagram-desktop HEAD_REF=feature HEAD_REPOSITORY=fork/teagram-desktop HEAD_SHA=new PR_NUMBER=105 \
    bash -c "$approval_step" >/dev/null 2>&1
}
approval_calls() {
  grep -oE 'runs/[0-9]+/cancel|labels/macos-build' "$approval_fixture/calls" | tr '\n' ' '
}
if ! run_approval_step "$old_head_runs" ok present; then
  printf 'A 409 for a run that already finished must not fail the macOS approval step.\n' >&2
  exit 1
fi
if [[ "$(approval_calls)" != 'runs/11/cancel runs/12/cancel labels/macos-build ' ]]; then
  printf 'A push must cancel unfinished older-head runs from the PR head repository and clear the label, got: %s\n' "$(approval_calls)" >&2
  exit 1
fi
if ! run_approval_step "$old_head_runs,$current_head_run" ok present; then
  printf 'The macOS approval step failed with an approved current-head run.\n' >&2
  exit 1
fi
if [[ "$(approval_calls)" != 'runs/11/cancel runs/12/cancel ' ]]; then
  printf 'A current-head run approved before cleanup must keep running and keep its label, got: %s\n' "$(approval_calls)" >&2
  exit 1
fi
if ! run_approval_step "$old_head_runs,$skipped_head_run" ok present; then
  printf 'The macOS approval step failed with a skipped current-head run.\n' >&2
  exit 1
fi
if [[ "$(approval_calls)" != 'runs/11/cancel runs/12/cancel labels/macos-build ' ]]; then
  printf 'A current-head run whose MacOS job was skipped must not keep macos-build, got: %s\n' "$(approval_calls)" >&2
  exit 1
fi
if run_approval_step "$pending_head_run" ok present || [[ "$(approval_calls)" == *labels/macos-build* ]]; then
  printf 'A current-head run that never creates its jobs must fail without touching macos-build.\n' >&2
  exit 1
fi
if ! run_approval_step "" ok absent; then
  printf 'A PR without macos-build must not fail the macOS approval step.\n' >&2
  exit 1
fi
if run_approval_step "$old_head_runs" forbidden present || [[ "$(approval_calls)" == *labels/macos-build* ]]; then
  printf 'A cancel failure other than 409 must fail the macOS approval step before the label is cleared.\n' >&2
  exit 1
fi
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
schedule_group="$(macos_group_for_event '' '' '' '' '' 'dev' 'schedule')"
dev_dispatch_group="$(macos_group_for_event '' '' '' '' '' 'dev' 'workflow_dispatch')"
if [[ "$schedule_group" == "$dev_dispatch_group" ]]; then
  printf 'A dev dispatch must not cancel the scheduled run that seeds the compiler cache.\n' >&2
  exit 1
fi
require_text "$mac_workflow" 'CCACHE_MAXSIZE: "5G"'
require_text "$mac_workflow" 'CCACHE_SLOPPINESS: "pch_defines,time_macros"'
require_text "$mac_workflow" 'bash "$REPO_NAME/.github/scripts/mac_ccache_profile.sh" "$GITHUB_EVENT_NAME" >> "$GITHUB_OUTPUT"'
reject_text "$mac_workflow" 'CMAKE_${lang}_FLAGS_DEBUG_INIT/s/ -g//'
pull_request_profile="$(bash "$ccache_profile_script" pull_request)"
scheduled_profile="$(bash "$ccache_profile_script" schedule)"
dispatch_profile="$(bash "$ccache_profile_script" workflow_dispatch)"
if [[ "$pull_request_profile" != "$scheduled_profile" ]]; then
  printf 'MacOS PR and scheduled builds must share one ccache profile.\n' >&2
  exit 1
fi
if [[ "$pull_request_profile" != $'name=pr-compatible\nprotected_path_integration=OFF' ]]; then
  printf 'MacOS PR and scheduled builds must use the PR-compatible OFF profile.\n' >&2
  exit 1
fi
if [[ "$dispatch_profile" != $'name=protected\nprotected_path_integration=ON' ]]; then
  printf 'Protected-path dispatches must use a separate ON ccache profile.\n' >&2
  exit 1
fi
cache_setup_step="$(awk '
  /^      - name: First set up\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step { print }
' "$mac_workflow")"
for cache_profile_setting in \
  'PROTECTED_PATH_INTEGRATION: ${{ steps.ccache-profile.outputs.protected_path_integration }}' \
  'protected_path_integration="$PROTECTED_PATH_INTEGRATION"' \
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
hash_expression="\${{ hashFiles('CCACHE_KEY.txt') }}"
profile_expression="\${{ steps.ccache-profile.outputs.name }}"
expected_ccache_restore_keys="macOS-arm64-ccache-dev-${profile_expression}-${hash_expression}-
macOS-arm64-ccache-dev-${hash_expression}-"
if [[ "$ccache_restore_keys" != "$expected_ccache_restore_keys" ]]; then
  printf 'MacOS pull requests must restore the shared dev compiler cache.\n' >&2
  exit 1
fi
require_text "$mac_workflow" 'key: macOS-arm64-ccache-dev-${{ steps.ccache-profile.outputs.name }}-${{ hashFiles('\''CCACHE_KEY.txt'\'') }}-${{ github.run_id }}-${{ github.run_attempt }}'
require_text "$mac_workflow" "github.event_name == 'schedule'"
reject_text "$mac_workflow" 'macOS-arm64-ccache-pr-'
reject_text "$mac_workflow" 'source-format'
reject_text "$mac_workflow" 'clang-format'
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
require_text "$mac_workflow" "github.ref == 'refs/heads/dev'"
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
if ! grep -Fq "github.ref == 'refs/heads/dev'" <<< "$save_cache_step" \
  || grep -Fq "github.event_name == 'pull_request'" <<< "$save_cache_step"; then
  printf 'Only dev runs may save the shared macOS compiler cache.\n' >&2
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
require_text "$mac_workflow" 'app_startup_timeout = 300'
require_text "$mac_workflow" 'app_runtime_timeout = 180'
require_text "$mac_workflow" 'Debugger cleanup signal failed:'
require_text "$mac_workflow" 'signal_pid(app_pid, "Teagram", cleanup_signal)'
reject_text "$mac_workflow" 'os.killpg(process.pid, signal.SIGTERM)'
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
reject_text "$packaged_workflow" 'queue: max'
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
require_text "$updater_workflow" 'ref: ${{ github.sha }}'
reject_text "$updater_workflow" 'ref: ${{ github.event.pull_request.head.sha }}'
reject_text "$updater_workflow" 'github.event.pull_request.head.repo.full_name'
require_text "$updater_workflow" 'fetch-depth: 2'
require_text "$updater_workflow" 'test "$checked_out_sha" = "$EXPECTED_MERGE_SHA"'
reject_text "$updater_workflow" 'EXPECTED_BASE_SHA'
reject_text "$updater_workflow" 'github.event.pull_request.base.sha'
require_text "$updater_workflow" 'test "${parents[1]}" = "$EXPECTED_HEAD_SHA"'
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
  TeagramUpdateInstallLocationAllowsSharedWritableParent \
  TeagramUpdateReplacementSwapsInSharedWritableParent \
  TeagramUpdateInstallLocationAllowsGroupWritableApplications \
  TeagramUpdateInstallerExercisesAuthenticatedCandidateAndFallback \
  TeagramUpdateInstallerRemovesInterruptedExtraction \
  TeagramUpdateInstallerClearsUncommittedCandidateOnRestart \
  TeagramUpdateStartupKeepsOlderPreviousBundle \
  TeagramUpdateInstallerKeepsCustomIcon \
  TeagramUpdateReplacementRetainsPreviousThroughStartup \
  TeagramUpdateReplacementRestoresMissingTarget \
  TeagramUpdateDownloadFailsWithoutMutation \
  TeagramUpdateRejectsUntrustedOrIneligiblePackage \
  TeagramUpdateInstallerRejectsUnwritableTargetWithoutMutation \
  TeagramUpdateInstallerRejectsUnsupportedTargetWithoutMutation \
  TeagramUpdateWorkerShutdownCleansCandidateArchive \
  TeagramUpdateManagerInstallsProducerBundle; do
  require_text "$updater_runner" "$fixture"
  if ! grep -Fq "TEST_CASE($fixture)" \
    Telegram/SourceFiles/tests/unit/teagram_update_download_tests.cpp \
    Telegram/SourceFiles/tests/unit/teagram_update_verification_tests.cpp; then
    printf 'Updater fixture runner names a missing test case: %s\n' "$fixture" >&2
    exit 1
  fi
done
fixture_selection="$(sed -n '/^fixtures=(/,/^)/p' "$updater_runner")"
fixture_count="$(grep -c '^  [[:alnum:]_]*$' <<< "$fixture_selection")"
if [[ "$fixture_count" != 19 ]]; then
  printf 'Updater fixture runner must select exactly 19 registered cases, got %s.\n' \
    "$fixture_count" >&2
  exit 1
fi
worker_shutdown_count="$(grep -Fxc \
  '  TeagramUpdateWorkerShutdownCleansCandidateArchive' \
  <<< "$fixture_selection")"
if [[ "$worker_shutdown_count" != 1 ]]; then
  printf 'Updater fixture runner must select worker shutdown exactly once.\n' >&2
  exit 1
fi
require_text "$updater_runner" '[unit_binary, "--case", fixture]'
require_text "$updater_runner" 'fixture_timeout_seconds=300'
require_text "$updater_runner" 'timeout=int(timeout_seconds)'
require_text "$updater_runner" 'subprocess.TimeoutExpired'
require_text "$updater_runner" 'tail -c "$max_log_bytes"'

require_text README.md '`MacOS.` workflow is the required pull request gate'

printf 'macOS CI contract tests passed.\n'
