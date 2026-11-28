#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=workflow_contract.sh
source "$scripts_dir/workflow_contract.sh"

mac_workflow="${MACOS_CI_CONTRACT_WORKFLOW:-.github/workflows/mac.yml}"
packaged_workflow=".github/workflows/mac_packaged.yml"
approval_workflow=".github/workflows/mac_pr_approval.yml"
unit_workflow=".github/workflows/macos_unit_feedback.yml"
updater_workflow=".github/workflows/macos_updater_fixtures.yml"
updater_runner=".github/scripts/run_teagram_updater_fixtures.sh"
macos_libraries_image_script=".github/scripts/macos_libraries_image.sh"
ccache_profile_script=".github/scripts/mac_ccache_profile.sh"

temp_dir="$(mktemp -d)"
trap 'rm -rf "$temp_dir"' EXIT

grep_file_pattern_status() {
  local status
  if grep -Eq -- "$1" "$2"; then
    return 0
  else
    status=$?
  fi
  if (( status == 1 )); then
    return 1
  fi
  return 2
}

grep_status_fixture="$temp_dir/grep-status.txt"
printf 'allowed\n' > "$grep_status_fixture"
grep_file_pattern_status '^allowed$' "$grep_status_fixture" \
  || contract_fail 'Contract grep must detect a matching line.'
if grep_file_pattern_status '^missing$' "$grep_status_fixture"; then
  contract_fail 'Contract grep must distinguish a missing pattern.'
else
  grep_status=$?
  if (( grep_status != 1 )); then
    contract_fail 'Contract grep must return 1 for a missing pattern.'
  fi
fi
if grep_file_pattern_status '^allowed$' "$temp_dir/missing-file" 2>/dev/null; then
  contract_fail 'Contract grep must report unreadable files as errors.'
else
  grep_status=$?
  if (( grep_status != 2 )); then
    contract_fail 'Contract grep must distinguish read errors from no match.'
  fi
fi

PYTHONDONTWRITEBYTECODE=1 python3 "$scripts_dir/test_macos_timeout_capture.py"
PYTHONDONTWRITEBYTECODE=1 python3 "$scripts_dir/test_macos_process_activation.py"

require_contract_tests_in_lint
for trigger in push pull_request; do
  paths="$(workflow_field .github/workflows/workflow_lint.yml ".on.$trigger.paths[]")"
  for script_path in \
    .github/scripts/test_macos_launch_smoke.py \
    .github/scripts/macos_timeout_capture.py \
    .github/scripts/test_macos_timeout_capture.py \
    .github/scripts/macos_process_activation.py \
    .github/scripts/test_macos_process_activation.py \
    .github/scripts/extract_dwarf_subprogram_address.py \
    .github/scripts/symbolicate_macos_minidump.py; do
    grep -Fxq -- "$script_path" <<< "$paths" \
      || contract_fail "Workflow lint must run when $script_path changes ($trigger)."
  done
done

# Mirrors the MacOS. concurrency group expression, which is pinned below.
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
  elif [[ "$event_name" == 'schedule' || "$event_name" == 'push' ]]; then
    repository_qualified_identity="seed/${ref_name}"
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


# MacOS. triggers and concurrency.
require_triggers "$mac_workflow" 'pull_request,push,schedule,workflow_dispatch'
# Each dev merge that can change a compile seeds the shared compiler cache.
require_workflow_field "$mac_workflow" '.on.push.branches | join(",")' 'dev'
mac_push_paths="$(workflow_field "$mac_workflow" '.on.push.paths[]')"
for seed_path in 'Telegram/**' 'cmake' 'cmake/**' 'CMakeLists.txt' \
    '.github/workflows/mac.yml' '.github/scripts/ccache_*.sh' '.github/scripts/mac_ccache_profile.sh'; do
  grep -Fxq -- "$seed_path" <<< "$mac_push_paths" \
    || contract_fail "$mac_workflow on.push.paths must list $seed_path"
done
for input in branch pull_request_repository pull_request_branch issue; do
  require_workflow_field "$mac_workflow" ".on.workflow_dispatch.inputs.$input.type" 'string'
done
require_workflow_field "$mac_workflow" '.on.workflow_dispatch.inputs.issue.required' 'false'
require_workflow_field "$mac_workflow" '.on.workflow_dispatch.inputs.issue.default' ''
require_workflow_field "$mac_workflow" '.run-name' \
  "\${{ inputs.issue && format('{0} [{1}]', github.workflow, inputs.issue) || '' }}"
require_workflow_field "$mac_workflow" '.concurrency.group' \
  "\${{ github.workflow }}-\${{ github.event.pull_request.head.ref && format('pr/{0}/{1}', github.event.pull_request.head.repo.full_name || github.repository, github.event.pull_request.head.ref) || inputs.pull_request_branch && format('pr/{0}/{1}', inputs.pull_request_repository || github.repository, inputs.pull_request_branch) || (github.event_name == 'schedule' || github.event_name == 'push') && format('seed/{0}', github.ref_name) || format('ref/{0}', inputs.branch || github.ref_name) }}"
assert_pr_dispatch_group 'teagramhq/teagram-desktop' 'feature/cache-refresh'
assert_pr_dispatch_group 'teagramhq/teagram-desktop' 'dev'
assert_pr_dispatch_group 'teagramhq/teagram-desktop' 'main'
fork_a_group="$(macos_group_for_event 'teagramhq/teagram-desktop' 'topic' '' '' '' '105/merge')"
fork_b_group="$(macos_group_for_event 'steward/tdesktop' 'topic' '' '' '' '105/merge')"
if [[ "$fork_a_group" == "$fork_b_group" ]]; then
  contract_fail 'PRs from different forks with the same branch must use separate groups.'
fi
assert_pr_base_isolation 'dev'
assert_pr_base_isolation 'main'
dev_group="$(macos_group_for_event '' '' '' '' 'dev' 'main')"
main_group="$(macos_group_for_event '' '' '' '' 'main' 'dev')"
if [[ "$dev_group" == "$main_group" ]]; then
  contract_fail 'MacOS dev and main runs must remain in separate concurrency groups.'
fi
schedule_group="$(macos_group_for_event '' '' '' '' '' 'dev' 'schedule')"
dev_dispatch_group="$(macos_group_for_event '' '' '' '' '' 'dev' 'workflow_dispatch')"
if [[ "$schedule_group" == "$dev_dispatch_group" ]]; then
  contract_fail 'A dev dispatch must not share the seed group.'
fi
push_group="$(macos_group_for_event '' '' '' '' '' 'dev' 'push')"
if [[ "$push_group" != "$schedule_group" ]]; then
  contract_fail 'Dev push and nightly seeds must share one concurrency group.'
fi

require_permission_map "$mac_workflow" 'workflow: inherit
check-macos-deps-artifact: contents=read,packages=read
build-macos-deps-artifact: contents=read,packages=write
ops-prune-macos-libraries: actions=write,contents=read
build-macos: contents=read,packages=read
test-libiconv: contents=read'

require_job_field "$mac_workflow" check-macos-deps-artifact '.["runs-on"]' 'ubuntu-latest'
expect_contains "$mac_workflow check-macos-deps-artifact if" \
  "$(job_field "$mac_workflow" check-macos-deps-artifact '.if')" \
  "github.event_name == 'push' || github.event_name == 'schedule'" \
  "github.ref == 'refs/heads/dev'" \
  "github.repository == 'teagramhq/teagram-desktop'"
require_step_field "$mac_workflow" check-macos-deps-artifact 'Set macOS dependency image tag.' \
  '.env.DEPENDENCY_HASH' \
  "\${{ hashFiles(format('{0}/Telegram/build/prepare/prepare.py', env.REPO_NAME), format('{0}/Telegram/build/prepare/mac.sh', env.REPO_NAME), format('{0}/Telegram/build/qt_version.py', env.REPO_NAME)) }}"
require_step_script "$mac_workflow" check-macos-deps-artifact 'Set macOS dependency image tag.' \
  "$macos_libraries_image_script\" set-image"
require_step_field "$mac_workflow" check-macos-deps-artifact 'Set up ORAS.' '.uses' \
  'oras-project/setup-oras@v2'
require_step_field "$mac_workflow" check-macos-deps-artifact 'Set up ORAS.' '.with.version' '1.3.4'
require_step_field "$mac_workflow" check-macos-deps-artifact 'Check for an existing dependency artifact.' '.run' \
  "bash \"\$REPO_NAME/$macos_libraries_image_script\" inspect \"\$MACOS_LIBRARIES_IMAGE\" \"\$GITHUB_OUTPUT\""
require_job_field "$mac_workflow" check-macos-deps-artifact '.outputs["cache-hit"]' "\${{ steps.image-cache.outputs.cache-hit }}"

require_job_field "$mac_workflow" build-macos-deps-artifact '.needs' 'check-macos-deps-artifact'
require_job_field "$mac_workflow" build-macos-deps-artifact '.["runs-on"]' 'macos-latest'
expect_contains "$mac_workflow build-macos-deps-artifact if" \
  "$(job_field "$mac_workflow" build-macos-deps-artifact '.if')" \
  '!cancelled()' \
  "github.event_name == 'push' || github.event_name == 'schedule'" \
  "github.ref == 'refs/heads/dev'" \
  "github.repository == 'teagramhq/teagram-desktop'" \
  "needs.check-macos-deps-artifact.result == 'success'" \
  "needs.check-macos-deps-artifact.outputs.cache-hit != 'true'"
require_step_field "$mac_workflow" build-macos-deps-artifact 'Set up ORAS.' '.uses' \
  'oras-project/setup-oras@v2'
require_step_field "$mac_workflow" build-macos-deps-artifact 'Set up ORAS.' '.with.version' '1.3.4'
require_step_field "$mac_workflow" build-macos-deps-artifact 'Set up dependency build tools.' '.if' \
  "steps.legacy-cache-libs.outputs.cache-hit != 'true'"
require_step_script "$mac_workflow" build-macos-deps-artifact 'Set up dependency build tools.' \
  'bash "$REPO_NAME/.github/scripts/install_macos_build_tools.sh" "$tools" libraries' \
  'echo "$tools/bin" >> "$GITHUB_PATH"'
dependency_tools_step_index=-1
libraries_setup_step_index=-1
step_index=0
while IFS= read -r step_name; do
  case "$step_name" in
    'Set up dependency build tools.') dependency_tools_step_index="$step_index" ;;
    'Libraries.') libraries_setup_step_index="$step_index" ;;
  esac
  ((step_index += 1))
done < <(JOB=build-macos-deps-artifact yq -r '.jobs[strenv(JOB)].steps[].name' "$mac_workflow")
if ((dependency_tools_step_index < 0 || dependency_tools_step_index >= libraries_setup_step_index)); then
  contract_fail "$mac_workflow must install dependency build tools before the cold Libraries build."
fi
require_step_field "$mac_workflow" build-macos-deps-artifact 'Restore legacy macOS libraries cache.' '.uses' \
  'actions/cache/restore@v6'
expect_equal "$mac_workflow build-macos-deps-artifact legacy cache restore if" \
  "$(step_field "$mac_workflow" build-macos-deps-artifact 'Restore legacy macOS libraries cache.' '.if')" 'null'
require_step_field "$mac_workflow" build-macos-deps-artifact 'Restore legacy macOS libraries cache.' '.with.key' \
  "macOS-libs-\${{ hashFiles(format('{0}/Telegram/build/prepare/prepare.py', env.REPO_NAME), format('{0}/Telegram/build/prepare/mac.sh', env.REPO_NAME), format('{0}/Telegram/build/qt_version.py', env.REPO_NAME)) }}"
expect_equal "$mac_workflow legacy cache key" \
  "$(step_field "$mac_workflow" build-macos-deps-artifact 'Restore legacy macOS libraries cache.' '.with.key')" \
  "$(step_field "$mac_workflow" build-macos 'Restore legacy macOS libraries cache.' '.with.key')"
require_step_field "$mac_workflow" build-macos-deps-artifact 'Libraries.' '.if' \
  "steps.legacy-cache-libs.outputs.cache-hit != 'true'"
require_step_field "$mac_workflow" build-macos-deps-artifact 'Publish macOS dependency artifact.' '.if' \
  "steps.libraries.outcome == 'success' || steps.legacy-cache-libs.outputs.cache-hit == 'true'"
require_step_script "$mac_workflow" build-macos-deps-artifact 'Publish macOS dependency artifact.' \
  "$macos_libraries_image_script\" push"
require_step_field "$mac_workflow" build-macos-deps-artifact 'Publish macOS dependency artifact.' '.run' \
  "bash \"\$REPO_NAME/$macos_libraries_image_script\" push \"\$MACOS_LIBRARIES_IMAGE\" \"\$GITHUB_WORKSPACE\""

require_job_field "$mac_workflow" ops-prune-macos-libraries '.["runs-on"]' 'ubuntu-latest'
require_job_field "$mac_workflow" ops-prune-macos-libraries '.needs | join(",")' \
  'check-macos-deps-artifact,build-macos-deps-artifact'
expect_contains "$mac_workflow ops-prune-macos-libraries if" \
  "$(job_field "$mac_workflow" ops-prune-macos-libraries '.if')" \
  '!cancelled()' \
  "needs.check-macos-deps-artifact.result == 'success'" \
  "needs.check-macos-deps-artifact.outputs.cache-hit == 'true'" \
  "needs.build-macos-deps-artifact.result == 'success'"
require_step_field "$mac_workflow" ops-prune-macos-libraries 'Prune legacy macOS library caches.' '.run' \
  "bash $macos_libraries_image_script prune-legacy-cache"
require_job_field "$mac_workflow" build-macos '.needs | join(",")' \
  'check-macos-deps-artifact,build-macos-deps-artifact,ops-prune-macos-libraries'
expect_contains "$mac_workflow build-macos if" "$(job_field "$mac_workflow" build-macos '.if')" \
  '!cancelled()' \
  "needs.check-macos-deps-artifact.result == 'success'" \
  "needs.check-macos-deps-artifact.result == 'skipped'" \
  "needs.build-macos-deps-artifact.result == 'success'" \
  "needs.build-macos-deps-artifact.result == 'skipped'" \
  "github.event.label.name == 'macos-build'" \
  "needs.ops-prune-macos-libraries.result == 'success'" \
  "needs.ops-prune-macos-libraries.result == 'skipped'"
expect_lacks "$mac_workflow macos if" "$(job_field "$mac_workflow" build-macos '.if')" 'always()'
require_job_field "$mac_workflow" build-macos '.strategy.fail-fast' 'false'
expect_equal "$mac_workflow in-job run cancellation steps" \
  "$(yq -r '[.jobs[] | .steps[]? | select(.name == "Cancel PR workflow run after job failure")] | length' "$mac_workflow")" '0'
require_step_field "$mac_workflow" build-macos 'Set macOS dependency image tag.' \
  '.env.DEPENDENCY_HASH' \
  "\${{ hashFiles(format('{0}/Telegram/build/prepare/prepare.py', env.REPO_NAME), format('{0}/Telegram/build/prepare/mac.sh', env.REPO_NAME), format('{0}/Telegram/build/qt_version.py', env.REPO_NAME)) }}"
require_step_field "$mac_workflow" build-macos 'Clone.' '.with.submodules' 'false'
merge_dev_step='Merge current dev for dispatched branches.'
require_step_field "$mac_workflow" build-macos "$merge_dev_step" '.if' \
  "\${{ github.event_name == 'workflow_dispatch' && github.ref_name != 'dev' && !inputs.libiconv_only }}"
require_step_script "$mac_workflow" build-macos "$merge_dev_step" \
  'git -C "$REPO_NAME" fetch --no-tags --quiet origin refs/heads/dev:refs/remotes/origin/dev' \
  'base_sha="$(git -C "$REPO_NAME" rev-parse origin/dev)"' \
  'base_sha=not_fetched' 'tested_sha=not_merged' \
  'write_merge_state()' 'write_merge_state' \
  'head_sha=%s\nbase_sha=%s\ntested_sha=%s\n' \
  'HEAD_SHA=%s\nBASE_SHA=%s\nTESTED_SHA=%s\n' \
  'git -C "$REPO_NAME" -c user.name='\''github-actions[bot]'\'' -c user.email='\''41898282+github-actions[bot]@users.noreply.github.com'\'' merge --no-edit --no-ff "$base_sha"' \
  'git -C "$REPO_NAME" diff --name-only --diff-filter=U' \
  'conflicts with dev at $base_sha; update the branch from dev' \
  'tested_sha="$(git -C "$REPO_NAME" rev-parse HEAD)"'
merge_dev_run="$(step_field "$mac_workflow" build-macos "$merge_dev_step" '.run')"
if ! awk '
  /tested_sha=not_merged/ { unmerged = NR }
  /base_sha="\$\(git -C "\$REPO_NAME" rev-parse origin\/dev\)"/ { fetched_base = NR }
  /^write_merge_state$/ && fetched_base && !merge { emitted_state = NR }
  /merge --no-edit --no-ff "\$base_sha"/ { merge = NR }
  /tested_sha="\$\(git -C "\$REPO_NAME" rev-parse HEAD\)"/ { tested = NR }
  /^write_merge_state$/ && merge { final_state = NR }
  END {
    exit !(unmerged && fetched_base && emitted_state && merge && tested && final_state \
      && unmerged < fetched_base && fetched_base < emitted_state && emitted_state < merge \
      && merge < tested && tested < final_state)
  }
' <<< "$merge_dev_run"; then
  contract_fail "$mac_workflow must emit the fetched dev SHA and not_merged status before merging, then record tested_sha only after a successful merge."
fi
submodules_step='Update submodules.'
require_step_field "$mac_workflow" build-macos "$submodules_step" '.run' \
  'git -C "$REPO_NAME" submodule update --init --recursive'
clone_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Clone.") | .key')"
merge_dev_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Merge current dev for dispatched branches.") | .key')"
submodules_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Update submodules.") | .key')"
dependency_hash_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Set macOS dependency image tag.") | .key')"
if ((clone_step_index >= merge_dev_step_index || merge_dev_step_index >= submodules_step_index \
    || submodules_step_index >= dependency_hash_step_index)); then
  contract_fail "$mac_workflow must merge dev after Clone and update submodules before hashing dependencies."
fi
require_step_field "$mac_workflow" build-macos 'Initialize workflow evidence.' '.if' \
  "\${{ always() && github.event_name != 'pull_request' }}"
require_step_script "$mac_workflow" build-macos 'Initialize workflow evidence.' \
  'head_sha=%s' 'base_sha=%s' 'tested_sha=%s' '$HEAD_SHA' '$BASE_SHA' '$TESTED_SHA'
require_step_script "$mac_workflow" build-macos 'Initialize workflow evidence.' \
  'HEAD_SHA=%s' 'BASE_SHA=%s' 'TESTED_SHA=%s' '$GITHUB_ENV'
require_job_field "$mac_workflow" build-macos '.env.HEAD_SHA' "\${{ github.sha }}"
require_job_field "$mac_workflow" build-macos '.env.BASE_SHA' 'not_merged'
require_job_field "$mac_workflow" build-macos '.env.TESTED_SHA' \
  "\${{ github.event_name == 'workflow_dispatch' && github.ref_name != 'dev' && !inputs.libiconv_only && 'not_merged' || github.sha }}"
for metadata_field in HEAD_SHA BASE_SHA TESTED_SHA; do
  expect_equal "$mac_workflow Summarize on-demand validation $metadata_field source" \
    "$(step_field "$mac_workflow" build-macos 'Summarize on-demand validation.' ".env.$metadata_field")" 'null'
done
require_step_script "$mac_workflow" build-macos 'Summarize on-demand validation.' \
  'head_sha' 'base_sha' 'tested_sha' '$HEAD_SHA' '$BASE_SHA' '$TESTED_SHA'
require_step_field "$mac_workflow" build-macos 'Summarize on-demand validation.' \
  '.env.OBSERVER_OUTCOME' '${{ steps.observer-self-tests.outcome }}'
require_step_script "$mac_workflow" build-macos 'Summarize on-demand validation.' \
  'Observer self-tests:' '$OBSERVER_OUTCOME'
require_step_script "$mac_workflow" build-macos 'Verify Mac App Store variant is unsupported.' '$TESTED_SHA'
require_step_script "$mac_workflow" build-macos 'Launch app smoke test.' '$TESTED_SHA'
require_step_script "$mac_workflow" build-macos 'Run protected profile and IPC integration tests.' '$TESTED_SHA'
require_step_script "$mac_workflow" build-macos 'Full chat info session regression.' '$TESTED_SHA'
media_permissions_step='Grant test app camera and microphone access.'
require_step_field "$mac_workflow" build-macos "$media_permissions_step" '.if' \
  "env.ONLY_CACHE == 'false'"
require_step_script "$mac_workflow" build-macos "$media_permissions_step" \
  'io.teagram.desktop' \
  'System Integrity Protection status: disabled.' \
  '/Library/Application Support/com.apple.TCC/TCC.db' \
  '$HOME/Library/Application Support/com.apple.TCC/TCC.db' \
  'kTCCServiceMicrophone' 'kTCCServiceCamera' \
  'INSERT OR REPLACE INTO access VALUES' \
  'sudo sqlite3' 'sqlite3' 'SELECT auth_value FROM access' \
  'Verified TCC grant:'
step_names=()
while IFS= read -r step_name; do
  step_names+=("$step_name")
done < <(yq -r '.jobs["build-macos"].steps[].name' "$mac_workflow")
find_step_index() {
  local wanted="$1"
  for index in "${!step_names[@]}"; do
    if [[ "${step_names[$index]}" == "$wanted" ]]; then
      printf '%s\n' "$index"
      return 0
    fi
  done
  return 1
}
if ! media_permissions_index="$(find_step_index "$media_permissions_step")"; then
  contract_fail "$mac_workflow is missing $media_permissions_step."
fi
if ! launch_smoke_index="$(find_step_index 'Launch app smoke test.')"; then
  contract_fail "$mac_workflow is missing the launch smoke test."
fi
if ! profile_ipc_index="$(find_step_index 'Run protected profile and IPC integration tests.')"; then
  contract_fail "$mac_workflow is missing profile and IPC integration tests."
fi
if ! lifecycle_index="$(find_step_index 'Full chat info session regression.')"; then
  contract_fail "$mac_workflow is missing the full chat info session regression."
fi
if (( media_permissions_index >= launch_smoke_index \
    || media_permissions_index >= profile_ipc_index \
    || media_permissions_index >= lifecycle_index )); then
  contract_fail "$mac_workflow must seed TCC before any app launch or lifecycle test."
fi
require_step_script "$mac_workflow" build-macos 'Log in to GitHub Container Registry.' \
  'oras login ghcr.io' '--password-stdin'
require_step_field "$mac_workflow" build-macos 'Restore macOS libraries artifact.' '.run' \
  "bash \"\$REPO_NAME/$macos_libraries_image_script\" pull \"\$MACOS_LIBRARIES_IMAGE\" \"\$GITHUB_WORKSPACE\" \"\$GITHUB_OUTPUT\""
require_step_field "$mac_workflow" build-macos 'Restore legacy macOS libraries cache.' '.uses' \
  'actions/cache/restore@v6'
require_step_field "$mac_workflow" build-macos 'Restore legacy macOS libraries cache.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true'"
require_step_field "$mac_workflow" build-macos 'Libraries.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true' && steps.legacy-cache-libs.outputs.cache-hit != 'true'"

# MacOS. builds a PR once its approved head is labeled macos-build; a later
# push cancels that build and clears the label from mac_pr_approval.yml.
require_workflow_field "$mac_workflow" '(.on.pull_request.types // []) | join(",")' 'labeled'
# Seeds queue instead of cancelling, so a nightly run that passed its dev-tip
# check saves before a later push seed runs; dispatches and approved PR builds
# replace their older run.
require_workflow_field "$mac_workflow" '.concurrency["cancel-in-progress"]' \
  "\${{ github.event_name == 'workflow_dispatch' || github.event.label.name == 'macos-build' }}"
require_triggers "$approval_workflow" 'pull_request_target'
require_workflow_field "$approval_workflow" '.on.pull_request_target.branches | join(",")' 'dev'
require_workflow_field "$approval_workflow" '.on.pull_request_target.types | join(",")' 'synchronize'
require_workflow_field "$approval_workflow" '.concurrency.group' \
  '${{ github.workflow }}-${{ github.event.pull_request.number }}'
require_permission_map "$approval_workflow" 'workflow: 
ops-clear-macos-approval: actions=write,pull-requests=write'
expect_equal "$approval_workflow checkout steps" \
  "$(yq -r '.jobs[].steps[] | select(.uses // "" | test("^actions/checkout@")) | .name' "$approval_workflow")" ''
reject_value "$approval_workflow" \
  'head.repo.full_name == github.repository' \
  'contains(github.event.pull_request.labels'
mkdir -p "$temp_dir/approval"
approval_step="$(step_field "$approval_workflow" ops-clear-macos-approval \
  'Cancel superseded MacOS. builds and clear their approval.' '.run')"
cat > "$temp_dir/approval/gh" <<'FAKE_GH'
#!/usr/bin/env bash
if [[ "$*" == *'/actions/workflows/mac.yml/runs'* ]]; then
  cat "$FAKE_GH_RUNS"
  exit 0
fi
if [[ "$*" == *'/pulls/105'* ]]; then
  # Each read of the live PR head takes the next line; the last one stays.
  live_head="$(head -n 1 "$FAKE_GH_LIVE_HEADS")"
  if [[ "$live_head" == fail ]]; then
    printf 'gh: Server Error (HTTP 502)\n' >&2
    exit 1
  fi
  printf '%s\n' "$live_head"
  if (( $(wc -l < "$FAKE_GH_LIVE_HEADS") > 1 )); then
    sed -i.bak 1d "$FAKE_GH_LIVE_HEADS"
  fi
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
chmod +x "$temp_dir/approval/gh"
old_head_runs='{"id":11,"status":"in_progress","head_sha":"old1","head_repository":{"full_name":"fork/teagram-desktop"}},{"id":12,"status":"queued","head_sha":"old2","head_repository":{"full_name":"fork/teagram-desktop"}},{"id":13,"status":"in_progress","head_sha":"old1","head_repository":{"full_name":"other/teagram-desktop"}},{"id":14,"status":"completed","head_sha":"old1","head_repository":{"full_name":"fork/teagram-desktop"}}'
current_head_run='{"id":15,"status":"in_progress","head_sha":"new","head_repository":{"full_name":"fork/teagram-desktop"}}'
skipped_head_run='{"id":16,"status":"completed","head_sha":"new","head_repository":{"full_name":"fork/teagram-desktop"}}'
pending_head_run='{"id":17,"status":"queued","head_sha":"new","head_repository":{"full_name":"fork/teagram-desktop"}}'
printf '%s' '{"jobs":[{"name":"MacOS (non-store)","status":"in_progress","conclusion":null}]}' > "$temp_dir/approval/15.json"
printf '%s' '{"jobs":[{"name":"MacOS","status":"completed","conclusion":"skipped"},{"name":"test: libiconv (macos-arm64)","status":"completed","conclusion":"skipped"}]}' > "$temp_dir/approval/16.json"
run_approval_step() {
  printf '{"workflow_runs":[%s]}' "$1" > "$temp_dir/approval/runs.json"
  printf '%s\n' ${4:-new} > "$temp_dir/approval/live_heads"
  : > "$temp_dir/approval/calls"
  PATH="$temp_dir/approval:$PATH" FAKE_GH_LOG="$temp_dir/approval/calls" FAKE_GH_RUNS="$temp_dir/approval/runs.json" \
    FAKE_GH_LIVE_HEADS="$temp_dir/approval/live_heads" \
    FAKE_GH_CANCEL="$2" FAKE_GH_LABEL="$3" FAKE_GH_JOBS_DIR="$temp_dir/approval" MACOS_APPROVAL_POLL_SECONDS=0 \
    GITHUB_REPOSITORY=teagramhq/teagram-desktop HEAD_REF=feature HEAD_REPOSITORY=fork/teagram-desktop HEAD_SHA=new PR_NUMBER=105 \
    bash -c "$approval_step" >/dev/null 2>&1
}
approval_calls() {
  grep -oE 'runs/[0-9]+/cancel|labels/macos-build' "$temp_dir/approval/calls" | tr '\n' ' '
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
# Pushes B ("new") then C ("newer"): B's cleanup starts after C was approved and built.
newer_head_run='{"id":18,"status":"in_progress","head_sha":"newer","head_repository":{"full_name":"fork/teagram-desktop"}}'
printf '%s' '{"jobs":[{"name":"MacOS (non-store)","status":"in_progress","conclusion":null}]}' > "$temp_dir/approval/18.json"
if ! run_approval_step "$old_head_runs,$newer_head_run" ok present newer; then
  printf 'A cleanup for a superseded push must exit cleanly.\n' >&2
  exit 1
fi
if [[ -n "$(approval_calls)" ]]; then
  printf 'A cleanup for a superseded push must not cancel the newer approved build or clear its label, got: %s\n' "$(approval_calls)" >&2
  exit 1
fi
if ! run_approval_step "$old_head_runs" ok present "new newer"; then
  printf 'A cleanup overtaken by a newer push must exit cleanly.\n' >&2
  exit 1
fi
if [[ "$(approval_calls)" != 'runs/11/cancel runs/12/cancel ' ]]; then
  printf 'A cleanup overtaken by a newer push before clearing must leave macos-build for that push, got: %s\n' "$(approval_calls)" >&2
  exit 1
fi
if run_approval_step "$old_head_runs" ok present fail; then
  printf 'A failed live PR head lookup must fail the macOS approval step.\n' >&2
  exit 1
fi
if [[ -n "$(approval_calls)" ]]; then
  printf 'A failed live PR head lookup must not cancel builds or clear approval, got: %s\n' "$(approval_calls)" >&2
  exit 1
fi
if run_approval_step "$old_head_runs" ok present "new fail" || [[ "$(approval_calls)" == *labels/macos-build* ]]; then
  printf 'A failed live PR head lookup before clearing must fail without clearing macos-build.\n' >&2
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

# MacOS. compiler cache: profile selection, keys, and who may save it.
require_job_field "$mac_workflow" build-macos '.env.CCACHE_MAXSIZE' '5G'
require_job_field "$mac_workflow" build-macos '.env.CCACHE_SLOPPINESS' 'pch_defines,time_macros'
require_job_field "$mac_workflow" build-macos '.env.CCACHE_COMPILERCHECK' 'content'
require_step_field "$mac_workflow" build-macos 'Select compiler cache profile.' '.run' \
  'bash "$REPO_NAME/.github/scripts/mac_ccache_profile.sh" "$GITHUB_EVENT_NAME" "$EVENT_LABEL" >> "$GITHUB_OUTPUT"'
require_step_field "$mac_workflow" build-macos 'Select compiler cache profile.' '.env.EVENT_LABEL' \
  "\${{ github.event.label.name == 'macos-build' && 'macos-build' || '' }}"
require_step_field "$mac_workflow" build-macos 'Teagram build.' '.env.PROTECTED_PATH_INTEGRATION' \
  '${{ steps.ccache-profile.outputs.protected_path_integration }}'
pull_request_profile="$(bash "$ccache_profile_script" pull_request)"
approved_pull_request_profile="$(bash "$ccache_profile_script" pull_request macos-build)"
scheduled_profile="$(bash "$ccache_profile_script" schedule)"
push_profile="$(bash "$ccache_profile_script" push)"
dispatch_profile="$(bash "$ccache_profile_script" workflow_dispatch)"
if [[ "$pull_request_profile" != "$scheduled_profile" || "$pull_request_profile" != "$push_profile" ]]; then
  contract_fail 'MacOS PR, scheduled and dev push builds must share one ccache profile.'
fi
if [[ "$pull_request_profile" != $'name=pr-compatible\nprotected_path_integration=OFF' ]]; then
  contract_fail 'Unapproved MacOS PR and scheduled builds must use the PR-compatible OFF profile.'
fi
if [[ "$approved_pull_request_profile" != $'name=protected\nprotected_path_integration=ON' ]]; then
  contract_fail 'Approved MacOS PR builds must use the protected ON ccache profile.'
fi
other_label_profile="$(bash "$ccache_profile_script" pull_request unrelated-label)"
if [[ "$other_label_profile" != "$pull_request_profile" ]]; then
  contract_fail 'Only the approved macos-build PR label may enable the protected profile.'
fi
if [[ "$dispatch_profile" != $'name=pr-compatible\nprotected_path_integration=ON' ]]; then
  contract_fail 'Protected-path dispatches must share the cache profile while keeping the ON integration define.'
fi
font_startup_profile_enabled() {
  [[ "$1" == *'protected_path_integration=ON'* ]]
}
if ! font_startup_profile_enabled "$approved_pull_request_profile" \
  || ! font_startup_profile_enabled "$dispatch_profile" \
  || font_startup_profile_enabled "$pull_request_profile" \
  || font_startup_profile_enabled "$push_profile" \
  || font_startup_profile_enabled "$scheduled_profile"; then
  contract_fail 'Font startup must run only when the build enables protected-path integration (approved PR and workflow_dispatch).'
fi
require_step_script "$mac_workflow" build-macos 'First set up.' \
  "TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=OFF"
if awk '
  /target_compile_definitions\(Telegram/ { in_target_definitions = 1 }
  in_target_definitions && /TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST/ { found = 1 }
  in_target_definitions && /^[[:space:]]*\)/ { in_target_definitions = 0 }
  END { exit !found }
' Telegram/CMakeLists.txt; then
  contract_fail 'The protected-path define must not change every Telegram compile command.'
fi
protected_path_sources="$(sed -n '/# Scope this opt-in define/,/endif()/p' Telegram/CMakeLists.txt)"
grep -Fq 'APPEND PROPERTY COMPILE_DEFINITIONS' <<< "$protected_path_sources" \
  || contract_fail 'The protected-path define must use source-level compile definitions.'
grep -Fq 'TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST' <<< "$protected_path_sources" \
  || contract_fail 'The protected-path define must be enabled for its source consumers.'
if grep -Fq 'TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST' \
    Telegram/SourceFiles/data/data_session.h; then
  contract_fail 'The source-scoped protected-path define must not change Session declarations.'
fi
for source in \
    '"${src_loc}/main.cpp"' \
    '"${src_loc}/data/data_session.cpp"' \
    '"${src_loc}/core/mac_protected_path_runtime.mm"' \
    '"${src_loc}/tests/account_lifecycle_regression.cpp"'; do
  grep -Fq -- "$source" <<< "$protected_path_sources" \
    || contract_fail "The protected-path define must stay scoped to $source."
done
grep -Fq 'TDESKTOP_MAC_EMOJI_CACHE_FOLDER_REGRESSION' \
  Telegram/SourceFiles/core/application.cpp \
  || contract_fail 'The protected-path profile fixture must exercise emojiCacheFolder in the app.'
grep -Fq 'Mac emoji cache folder regression: rejected=%d' \
  Telegram/SourceFiles/core/application.cpp \
  || contract_fail 'The profile fixture must report the emoji cache path decision.'
grep -Fq 'TDESKTOP_MAC_PROFILE_TEST_WAIT_FOR_APP_READY' \
  Telegram/SourceFiles/core/application.cpp \
  || contract_fail 'The profile fixture must publish startup readiness only after the app is ready.'
grep -Fq 'Mac profile app ready: startup=complete' \
  Telegram/SourceFiles/core/application.cpp \
  || contract_fail 'The profile fixture must expose a startup completion marker.'
for profile_marker in \
  TDESKTOP_MAC_EMOJI_CACHE_FOLDER_REGRESSION=1 \
  TDESKTOP_MAC_PROFILE_TEST_WAIT_FOR_APP_READY=1; do
  grep -Fq "$profile_marker" Telegram/build/test_mac_profile_guard.sh \
    || contract_fail "The profile IPC harness must set $profile_marker."
done
grep -Fq 'APP_READY' Telegram/build/test_mac_profile_guard.sh \
  || contract_fail 'The profile IPC harness must wait for the app-ready marker before sending quit.'
hash_expression="\${{ hashFiles('CCACHE_KEY.txt') }}"
profile_expression="\${{ steps.ccache-profile.outputs.name }}"
require_step_field "$mac_workflow" build-macos 'Teagram compiler cache.' '.uses | sub("@.*", "")' \
  'actions/cache/restore'
require_step_field "$mac_workflow" build-macos 'Teagram compiler cache.' '.with.key' \
  "macOS-arm64-ccache-dev-${profile_expression}-${hash_expression}-\${{ github.run_id }}-\${{ github.run_attempt }}"
require_step_field "$mac_workflow" build-macos 'Teagram compiler cache.' '.with["restore-keys"]' \
  "macOS-arm64-ccache-dev-${profile_expression}-${hash_expression}-
macOS-arm64-ccache-dev-pr-compatible-${hash_expression}-
macOS-arm64-ccache-dev-${hash_expression}-
macOS-arm64-ccache-dev-${profile_expression}-"
# Only a dev push or scheduled seed whose commit is still the dev tip saves the
# shared compiler cache. The separate trusted job publishes Libraries to GHCR.
seed_condition="always() && !cancelled() && steps.ccache-stats.outputs.save == 'true' && (github.event_name == 'schedule' || github.event_name == 'push') && github.ref == 'refs/heads/dev'"
expect_equal "$mac_workflow job build-macos step 'Check compiler cache seed is current.' if" \
  "$(contract_squash "$(step_field "$mac_workflow" build-macos 'Check compiler cache seed is current.' '.if')")" \
  "$seed_condition"
require_step_field "$mac_workflow" build-macos 'Check compiler cache seed is current.' '.id' 'seed-current'
require_step_script "$mac_workflow" build-macos 'Check compiler cache seed is current.' \
  'bash "$REPO_NAME/.github/scripts/ccache_seed_current.sh" "$REPO_NAME" "$GITHUB_SHA" FETCH_HEAD'
expect_equal "$mac_workflow job build-macos step 'Save Teagram compiler cache.' if" \
  "$(contract_squash "$(step_field "$mac_workflow" build-macos 'Save Teagram compiler cache.' '.if')")" \
  "$seed_condition && steps.seed-current.outputs.current == 'true'"
seed_check_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Check compiler cache seed is current.") | .key')"
save_cache_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Save Teagram compiler cache.") | .key')"
((seed_check_index < save_cache_index)) \
  || contract_fail 'The seed currency check must run before the compiler cache save.'
require_step_field "$mac_workflow" build-macos 'Save Teagram compiler cache.' '.with.key' \
  '${{ steps.cache-ccache.outputs.cache-primary-key }}'
expect_equal "$mac_workflow cache writers" "$(cache_writers "$mac_workflow")" \
  'build-macos / Save Teagram compiler cache.'
reject_value "$mac_workflow" \
  'macOS-arm64-ccache-pr-' \
  'startsWith(steps.cache-ccache.outputs.cache-matched-key' \
  'CCACHE_DISABLE=1' \
  'CMAKE_${lang}_FLAGS_DEBUG_INIT/s/ -g//' \
  'source-format' \
  'clang-format'

require_step_script "$mac_workflow" build-macos 'Teagram build.' \
  '[ "$GITHUB_EVENT_NAME" = "workflow_dispatch" ]' \
  '[ "$GITHUB_EVENT_NAME" = "pull_request" ]; then' \
  '-D TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=ON' \
  'CMAKE_C_COMPILER_LAUNCHER=ccache' \
  'ccache_launcher.sh' \
  '-Xclang -fno-pch-timestamp' \
  '-G "Ninja Multi-Config"'

# Compiler cache diagnostics decide the save and warn; they never fail a build.
stats_step='Report compiler cache hits.'
require_step_field "$mac_workflow" build-macos "$stats_step" '.env.BUILD_OUTCOME' '${{ steps.build.outcome }}'
require_step_script "$mac_workflow" build-macos "$stats_step" \
  'ccache --show-stats' \
  'ccache_hit_count.sh' \
  'ccache_save_decision.sh' \
  '::warning::A restored compiler cache produced no cache hits.' \
  '::warning::ccache did not record any compiler invocations for the Teagram build.'
reject_step_script "$mac_workflow" build-macos "$stats_step" '::error::' 'exit 1'
ccache_stats_script="$(step_script "$mac_workflow" build-macos "$stats_step")"
# Bash decides what is a comment: a command hidden after `#` never counts as
# present, while `$#`, `${#name}`, quoted text and heredoc lines do.
stats_body="$(step_field "$mac_workflow" build-macos "$stats_step" '.run')"
for hiding_prefix in 'true # ' 'true;# ' '(true)# ' "echo \$'can\\'t' # "; do
  hidden_body="$(HIDE="$hiding_prefix" awk '
    /ccache_save_decision\.sh/ { match($0, /^[[:space:]]*/); $0 = substr($0, 1, RLENGTH) ENVIRON["HIDE"] substr($0, RLENGTH + 1) }
    { print }
  ' <<< "$stats_body")"
  hidden_script="$(strip_shell_comments <<< "$hidden_body")" \
    || contract_fail "The stats step rewritten with '$hiding_prefix' must still parse."
  if [[ "$(contract_compact "$hidden_script")" == *ccache_save_decision.sh* ]]; then
    contract_fail "A run body must not count a command hidden after: $hiding_prefix"
  fi
done
for kept_case in \
  'echo "$#" ccache_save_decision.sh' \
  'echo "${#args[@]}" ccache_save_decision.sh' \
  "echo '# ccache_save_decision.sh'" \
  $'cat <<EOF\n# ccache_save_decision.sh\nEOF'; do
  kept_script="$(strip_shell_comments <<< "$kept_case")" \
    || contract_fail "Comment stripping must parse: $kept_case"
  if [[ "$kept_script" != *ccache_save_decision.sh* ]]; then
    contract_fail "A run body must keep text that is not a comment: $kept_case"
  fi
done

# A body that closes the parsing wrapper early is refused before anything in it
# can run.
escape_canary="$temp_dir/wrapper-escape"
if printf '%s\n' ':' '}' "touch '$escape_canary'" '__contract_tail() {' ':' \
    | strip_shell_comments > /dev/null 2>&1; then
  contract_fail 'A run body that closes the parsing wrapper must fail to parse.'
fi
[[ ! -e "$escape_canary" ]] || contract_fail 'Parsing a run body must never execute it.'

# Cache outputs are written before the zero-hit warning can end the step.
compact_stats="$(contract_compact "$ccache_stats_script")"
script_offset() {
  local needle
  needle="$(contract_compact "$1")"
  [[ "$compact_stats" == *"$needle"* ]] || { printf '%s\n' -1; return; }
  local before="${compact_stats%%"$needle"*}"
  printf '%s\n' "${#before}"
}
misses_output_offset="$(script_offset 'echo "misses=$misses" >> "$GITHUB_OUTPUT"')"
save_decision_offset="$(script_offset 'ccache_save_decision.sh')"
zero_hit_guard_offset="$(script_offset '::warning::A restored compiler cache produced no cache hits.')"
if ((misses_output_offset < 0 || save_decision_offset < 0 || zero_hit_guard_offset < 0 \
    || misses_output_offset >= zero_hit_guard_offset || save_decision_offset >= zero_hit_guard_offset)); then
  contract_fail 'Compiler cache outputs must be written before the zero-hit warning is reported.'
fi
require_step_script "$mac_workflow" build-macos 'Report MacOS job wall time.' \
  '((elapsed_seconds > 1800))' \
  '::warning::Warm-cache MacOS PR build exceeded the 30-minute target ('
reject_step_script "$mac_workflow" build-macos 'Report MacOS job wall time.' '::error::' 'exit 1'
reject_value "$mac_workflow" 'A warm MacOS PR run must have an exact Libraries cache hit.'
require_step_field "$mac_workflow" build-macos 'Restore macOS libraries artifact.' '.run' \
  "bash \"\$REPO_NAME/$macos_libraries_image_script\" pull \"\$MACOS_LIBRARIES_IMAGE\" \"\$GITHUB_WORKSPACE\" \"\$GITHUB_OUTPUT\""
require_step_field "$mac_workflow" build-macos 'Libraries.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true' && steps.legacy-cache-libs.outputs.cache-hit != 'true'"

# Bound each post-build test or replay independently so one hang does not hold
# a macOS runner until the job timeout.
ui_watchdog_step='Verify UI stall watchdog stack capture.'
require_step_field "$mac_workflow" build-macos "$ui_watchdog_step" '.id' 'ui-stall-watchdog'
require_step_field "$mac_workflow" build-macos "$ui_watchdog_step" '.timeout-minutes' '1'
require_step_script "$mac_workflow" build-macos "$ui_watchdog_step" \
  'macos-regression-diagnostics/ui-stall-watchdog' \
  'exec > >(tee "$diagnostics/harness.log") 2>&1'

observer_self_tests_step='Run isolation observer self-tests (informational).'
require_step_field "$mac_workflow" build-macos "$observer_self_tests_step" '.id' 'observer-self-tests'
require_step_field "$mac_workflow" build-macos "$observer_self_tests_step" '.timeout-minutes' '2'
require_step_script "$mac_workflow" build-macos "$observer_self_tests_step" \
  'macos-regression-diagnostics/observer-self-tests' \
  'exec > >(tee "$diagnostics/harness.log") 2>&1'

font_startup_step='Verify font startup regression.'
require_step_field "$mac_workflow" build-macos "$font_startup_step" '.id' 'font-startup'
require_step_field "$mac_workflow" build-macos "$font_startup_step" '.timeout-minutes' '5'
require_step_field "$mac_workflow" build-macos "$font_startup_step" '.if' \
  "\${{ !cancelled() && steps.ccache-profile.outputs.protected_path_integration == 'ON' && steps.build.outcome == 'success' }}"
require_step_script "$mac_workflow" build-macos "$font_startup_step" \
  'macos-regression-diagnostics/font-startup' \
  'bash "$REPO_NAME/Telegram/build/test_mac_profile_guard.sh"' \
  '"$executable" font-startup' \
  'TDESKTOP_MAC_FONT_STARTUP_ALREADY_RUN=1' \
  'tee "$evidence/font-startup-regression.log" "$diagnostics/harness.log"'

font_startup_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.id == "font-startup") | .key')"
build_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.id == "build") | .key')"
cache_watchdog_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.id == "cache-watchdog") | .key')"
launch_smoke_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.id == "launch-smoke") | .key')"
if ((font_startup_step_index <= build_step_index \
    || font_startup_step_index >= cache_watchdog_step_index \
    || font_startup_step_index >= launch_smoke_step_index)); then
  contract_fail "$font_startup_step must run after build and before the cache watchdog or first app launch."
fi

cache_watchdog_step='Verify cache path watchdog regression.'
require_step_field "$mac_workflow" build-macos "$cache_watchdog_step" '.id' 'cache-watchdog'
require_step_field "$mac_workflow" build-macos "$cache_watchdog_step" '.timeout-minutes' '5'
require_step_field "$mac_workflow" build-macos "$cache_watchdog_step" '.if' \
  "\${{ !cancelled() && github.event_name == 'pull_request' && steps.build.outcome == 'success' && steps.font-startup.outcome == 'success' }}"
require_step_script "$mac_workflow" build-macos "$cache_watchdog_step" \
  'rm -rf "$diagnostics"' \
  'state="$diagnostics/state"' \
  'trap cleanup_cache_watchdog_state EXIT' \
  'if [ "$status" -eq 0 ]; then rm -rf "$state"; fi' \
  'TDESKTOP_MAC_PROFILE_EVIDENCE_LOG="$diagnostics/authenticated-app.log"' \
  'tee "$evidence/cache-watchdog-regression.log" "$diagnostics/harness.log"'

launch_smoke_step='Launch app smoke test.'
require_step_field "$mac_workflow" build-macos "$launch_smoke_step" '.timeout-minutes' '2'
require_step_script "$mac_workflow" build-macos "$launch_smoke_step" \
  'macos-regression-diagnostics/launch-smoke' \
  'state="$RUNNER_TEMP/teagram-launch-smoke"' 'rm -rf "$state"' \
  'HOME="$state/home" TDESKTOP_MAC_PROFILE_TEST_HOME="$state/home" arch -arm64 "$executable"' \
  'launch-smoke.stdout.log' 'launch-smoke.stderr.log' \
  'exec > >(tee "$diagnostics/harness.log") 2>&1'
reject_step_script "$mac_workflow" build-macos "$launch_smoke_step" \
  'mktemp -d "$RUNNER_TEMP/teagram-launch-smoke.'

account_database_profile_step='Resolve Teagram account-database profile.'
require_step_field "$mac_workflow" build-macos "$account_database_profile_step" \
  '.id' 'account-database-profile'
require_step_script "$mac_workflow" build-macos "$account_database_profile_step" \
  'pwd.getpwuid(os.getuid()).pw_dir' \
  'profile_root=%s/Library/Application Support/Teagram'

profile_step='Run protected profile and IPC integration tests.'
require_step_field "$mac_workflow" build-macos "$profile_step" '.timeout-minutes' '30'
require_step_script "$mac_workflow" build-macos "$profile_step" \
  'state="$diagnostics/state"' \
  'if [ "$status" -eq 0 ]; then rm -rf "$state"; fi' \
  'exec > >(tee "$diagnostics/harness.log") 2>&1'
profile_step_script="$(step_field "$mac_workflow" build-macos "$profile_step" '.run')"
for deadline_clause in \
  'step_started_at="$(date +%s)"' \
  'step_timeout_seconds=1800' \
  'step_finish_headroom_seconds=60' \
  'step_deadline_epoch="$((step_started_at + step_timeout_seconds - step_finish_headroom_seconds))"' \
  'TDESKTOP_MAC_LIFECYCLE_STEP_DEADLINE_EPOCH="$step_deadline_epoch"'; do
  grep -Fq "$deadline_clause" <<< "$profile_step_script" \
    || contract_fail "$profile_step must pass a bounded lifecycle deadline: $deadline_clause"
done
step_started_at_index="$(awk 'index($0, "step_started_at=\"$(date +%s)\"") { print NR; exit }' <<< "$profile_step_script")"
diagnostics_setup_index="$(awk 'index($0, "diagnostics=\"$RUNNER_TEMP/macos-regression-diagnostics/profile-ipc\"") { print NR; exit }' <<< "$profile_step_script")"
[[ "$step_started_at_index" =~ ^[1-9][0-9]*$ \
  && "$diagnostics_setup_index" =~ ^[1-9][0-9]*$ ]] \
  && ((step_started_at_index < diagnostics_setup_index)) \
  || contract_fail "$profile_step must start its deadline before diagnostics setup."
if grep -Fq 'cp -R "$state"' <<< "$profile_step_script"; then
  contract_fail "$profile_step must retain state in the uploaded diagnostics directory without copying it."
fi
require_step_script "$mac_workflow" build-macos "$profile_step" \
  'if [ "$tee_status" -ne 0 ]; then' \
  'Could not retain profile/IPC integration logs for variant=$BUILD_VARIANT.'

case_runner='Telegram/build/run_mac_lifecycle_cases.sh'
case_timeout_limit_seconds="$(awk '
  /^CASE_TIMEOUT_LIMIT_SECONDS=/ {
    sub(/^[^=]*=/, "")
    print
  }
' "$case_runner")"
cleanup_grace_seconds="$(awk '
  /^CASE_CLEANUP_GRACE_SECONDS=/ {
    sub(/^[^=]*=/, "")
    print
  }
' "$case_runner")"
runner_cleanup_calls="$(awk '
  /process\.wait\(timeout=cleanup_grace_seconds\)/ { count++ }
  END { print count + 0 }
' "$case_runner")"
profile_step_timeout_minutes="$(step_field "$mac_workflow" build-macos "$profile_step" '.timeout-minutes')"
profile_step_timeout_seconds="$(awk -F= '/^step_timeout_seconds=/ { print $2; exit }' <<< "$profile_step_script")"
profile_step_finish_headroom_seconds="$(awk -F= '/^step_finish_headroom_seconds=/ { print $2; exit }' <<< "$profile_step_script")"
if ! grep -Fq \
  'CASE_TIMEOUT_SECONDS="${TDESKTOP_MAC_LIFECYCLE_CASE_TIMEOUT_SECONDS:-$CASE_TIMEOUT_LIMIT_SECONDS}"' \
  "$case_runner"; then
  contract_fail "$case_runner must default to the checked lifecycle case timeout limit."
fi
[[ "$case_timeout_limit_seconds" =~ ^[1-9][0-9]*$ \
  && "$cleanup_grace_seconds" =~ ^[1-9][0-9]*$ \
  && "$runner_cleanup_calls" == 2 \
  && "$profile_step_timeout_minutes" =~ ^[1-9][0-9]*$ \
  && "$profile_step_timeout_seconds" =~ ^[1-9][0-9]*$ \
  && "$profile_step_finish_headroom_seconds" =~ ^[1-9][0-9]*$ ]] \
  || contract_fail 'The protected profile case, deadline, and cleanup timeout budget must be explicit and positive.'
grep -Fq 'if (( CASE_RUN_TIMEOUT_SECONDS > CASE_TIMEOUT_LIMIT_SECONDS )); then' "$case_runner" \
  || contract_fail 'Lifecycle cases must share the existing 10-minute timeout cap.'
cleanup_headroom_seconds="$((2 * cleanup_grace_seconds))"
case_timeout_budget_seconds="$((case_timeout_limit_seconds + cleanup_headroom_seconds))"
profile_step_deadline_budget_seconds="$((profile_step_timeout_seconds - profile_step_finish_headroom_seconds))"
((case_timeout_limit_seconds == 600 \
    && case_timeout_budget_seconds < profile_step_deadline_budget_seconds \
    && profile_step_timeout_seconds == profile_step_timeout_minutes * 60 \
    && profile_step_timeout_seconds == 1800 \
    && profile_step_finish_headroom_seconds > cleanup_headroom_seconds)) \
  || contract_fail "Lifecycle deadline and cleanup headroom must fit inside the profile/IPC step limit (${profile_step_timeout_minutes}m)."
for runner_clause in \
  'CASE_CLEANUP_HEADROOM_SECONDS=$((2 * CASE_CLEANUP_GRACE_SECONDS))' \
  'if (( CASE_RUN_TIMEOUT_SECONDS > CASE_TIMEOUT_LIMIT_SECONDS )); then' \
  'STEP_DEADLINE_EPOCH="${TDESKTOP_MAC_LIFECYCLE_STEP_DEADLINE_EPOCH:-}"' \
  'remaining_case_seconds=$((STEP_DEADLINE_EPOCH - now_epoch - CASE_CLEANUP_HEADROOM_SECONDS))' \
  'if (( remaining_case_seconds < CASE_RUN_TIMEOUT_SECONDS )); then' \
  'CASE_RUN_TIMEOUT_SECONDS="$remaining_case_seconds"' \
  'timeout_seconds=%s' \
  '"$CASE_LOG" "$CASE_RUN_TIMEOUT_SECONDS" "$CASE_CLEANUP_GRACE_SECONDS"' \
  'process.wait(timeout=cleanup_grace_seconds)'; do
  grep -Fq "$runner_clause" "$case_runner" \
    || contract_fail "$case_runner must bound each case against the step deadline: $runner_clause"
done
expect_equal "$mac_workflow lifecycle timeout override" \
  "$(workflow_field "$mac_workflow" '.env.TDESKTOP_MAC_LIFECYCLE_CASE_TIMEOUT_SECONDS')" 'null'
require_job_field "$mac_workflow" build-macos '.env.TDESKTOP_MAC_LIFECYCLE_CASE_TIMEOUT_SECONDS' 'null'
require_step_field "$mac_workflow" build-macos "$profile_step" \
  '.env.TDESKTOP_MAC_LIFECYCLE_CASE_TIMEOUT_SECONDS' 'null'

if ! build_step_index="$(find_step_index 'Teagram build.')"; then
  contract_fail "$mac_workflow is missing Teagram build."
fi
for step_index in "${!step_names[@]}"; do
  ((step_index > build_step_index)) || continue
  step_name="${step_names[$step_index]}"
  [[ "$step_name" =~ (regression|watchdog|smoke|self-tests|integration) ]] || continue
  if [[ "$step_name" == 'Report skipped '* || "$step_name" == 'Upload '* ]]; then
    continue
  fi
  timeout="$(step_field "$mac_workflow" build-macos "$step_name" '.timeout-minutes')"
  [[ "$timeout" =~ ^[1-9][0-9]*$ ]] \
    || contract_fail "$step_name must have a positive timeout-minutes value."
done

regression_diagnostics_step='Upload macOS regression diagnostics.'
require_step_field "$mac_workflow" build-macos "$regression_diagnostics_step" '.uses' \
  'actions/upload-artifact@v7'
require_step_field "$mac_workflow" build-macos "$regression_diagnostics_step" \
  '.with.name' 'macos-regression-diagnostics-${{ github.run_id }}'
require_step_field "$mac_workflow" build-macos "$regression_diagnostics_step" \
  '.with["if-no-files-found"]' 'error'
require_step_field "$mac_workflow" build-macos "$regression_diagnostics_step" \
  '.with["retention-days"]' '14'
regression_artifact_condition="$(step_field "$mac_workflow" build-macos \
  "$regression_diagnostics_step" '.if')"
regression_artifact_condition_compact="$(contract_compact "$regression_artifact_condition")"
regression_artifact_paths="$(step_field "$mac_workflow" build-macos \
  "$regression_diagnostics_step" '.with.path')"
grep -Fq 'always()' <<< "$regression_artifact_condition_compact" \
  || contract_fail "$regression_diagnostics_step must run after a regression step fails or times out."
for regression_step_id in \
  ui-stall-watchdog \
  observer-self-tests \
  font-startup \
  cache-watchdog \
  launch-smoke \
  profile-ipc \
  lifecycle-regression; do
  condition_clause="steps.$regression_step_id.outcome!='success'&&steps.$regression_step_id.outcome!='skipped'"
  grep -Fq "$condition_clause" <<< "$regression_artifact_condition_compact" \
    || contract_fail "$regression_diagnostics_step must upload for $regression_step_id failures and timeouts."
done
for regression_artifact_path in \
  '${{ runner.temp }}/macos-regression-diagnostics/' \
  '${{ runner.temp }}/tdesktop-chat-participants/diagnostics/' \
  '${{ env.REPO_NAME }}/out/Debug/evidence/' \
  '${{ steps.account-database-profile.outputs.profile_root }}/log_start*.txt' \
  '${{ steps.account-database-profile.outputs.profile_root }}/log.txt' \
  '${{ steps.account-database-profile.outputs.profile_root }}/DebugLogs/'; do
  grep -Fq "$regression_artifact_path" <<< "$regression_artifact_paths" \
    || contract_fail "$regression_diagnostics_step must upload $regression_artifact_path."
done

require_step_script "$mac_workflow" build-macos \
  'Full chat info session regression.' \
  'authorization_timeout_report(' \
  'report.command_output' \
  'report.log_output' \
  'report.console_output' \
  'application_profile="$regression_home/Library/Application Support/Teagram"' \
  'HOME="$regression_home"' \
  'TDESKTOP_MAC_PROFILE_TEST_HOME="$regression_home"' \
  '"$application_profile/$relative_log"' \
  'cp "$application_profile/$relative_log" "$application_logs/$relative_log"'

# Derive the profile roots from the workflow, then make sure every profile used
# for smoke/lifecycle or account-database fallback is an artifact input.
smoke_script="$(step_field "$mac_workflow" build-macos "$launch_smoke_step" '.run')"
smoke_state_suffix="$(sed -n 's|^[[:space:]]*state="\$RUNNER_TEMP/\([^"]*\)"$|\1|p' <<< "$smoke_script")"
lifecycle_script="$(step_field "$mac_workflow" build-macos \
  'Full chat info session regression.' '.run')"
regression_root_suffix="$(sed -n 's|^[[:space:]]*regression_root="\$RUNNER_TEMP/\([^"]*\)"$|\1|p' <<< "$lifecycle_script")"
regression_home_suffix="$(sed -n 's|^[[:space:]]*regression_home="\$regression_root/\([^"]*\)"$|\1|p' <<< "$lifecycle_script")"
application_profile_suffix="$(sed -n 's|^[[:space:]]*application_profile="\$regression_home/\([^"]*\)"$|\1|p' <<< "$lifecycle_script")"
for extracted_value in "$smoke_state_suffix" "$regression_root_suffix" \
    "$regression_home_suffix" "$application_profile_suffix"; do
  [[ -n "$extracted_value" ]] \
    || contract_fail 'The workflow profile mappings must remain extractable for retention coverage.'
done
profile_suffix='Library/Application Support/Teagram'
smoke_profile_suffix="$smoke_state_suffix/home/$profile_suffix"
lifecycle_profile_suffix="$regression_root_suffix/$regression_home_suffix/$application_profile_suffix"
runner_temp_path_expression='${{ runner.temp }}/'
for profile_suffix_to_upload in "$smoke_profile_suffix" "$lifecycle_profile_suffix"; do
  for profile_log_path in log_start\*.txt log.txt DebugLogs/; do
    regression_artifact_path="$runner_temp_path_expression$profile_suffix_to_upload/$profile_log_path"
    grep -Fq "$regression_artifact_path" <<< "$regression_artifact_paths" \
      || contract_fail "$regression_diagnostics_step must upload workflow-derived profile path $regression_artifact_path."
  done
done

# Fixture test using the workflow-derived profiles: simulate app failure and
# timeout, and verify the corresponding artifact paths still contain the logs.
python3 - "$temp_dir/profile-log-retention" \
  "$temp_dir/$smoke_state_suffix/home" \
  "$temp_dir/$regression_root_suffix/$regression_home_suffix" \
  "$temp_dir/account-database-home" <<'PY'
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(sys.argv[1])
profile_homes = [
    ("smoke", Path(sys.argv[2]), True),
    ("lifecycle", Path(sys.argv[3]), True),
    ("account-fallback", Path(sys.argv[4]), False),
]
app = root / "fake_teagram.py"
app.parent.mkdir(parents=True, exist_ok=True)
app.write_text(
    "import os, sys, time\n"
    "from pathlib import Path\n"
    "home = Path(os.environ.get('TDESKTOP_MAC_PROFILE_TEST_HOME', os.environ['ACCOUNT_DATABASE_HOME']))\n"
    "profile = home / 'Library/Application Support/Teagram'\n"
    "(profile / 'DebugLogs').mkdir(parents=True, exist_ok=True)\n"
    "(profile / 'log_start0.txt').write_text(sys.argv[1] + ' ' + sys.argv[2] + ' startup log\\n')\n"
    "if sys.argv[2] == 'failure': sys.exit(23)\n"
    "(profile / 'log.txt').write_text(sys.argv[1] + ' ' + sys.argv[2] + ' app log\\n')\n"
    "(profile / 'DebugLogs/log_20261112.txt').write_text(sys.argv[1] + ' ' + sys.argv[2] + ' debug log\\n')\n"
    "time.sleep(60)\n",
    encoding="utf-8",
)

for source, home, use_override in profile_homes:
    for mode in ("failure", "timeout"):
        profile = home / "Library/Application Support/Teagram"
        expected = {
            profile / "log_start0.txt": f"{source} {mode} startup log\n",
        }
        if mode == "timeout":
            expected.update({
                profile / "log.txt": f"{source} {mode} app log\n",
                profile / "DebugLogs/log_20261112.txt": f"{source} {mode} debug log\n",
            })
        env = os.environ.copy()
        env["HOME"] = str(home)
        env["ACCOUNT_DATABASE_HOME"] = str(home if not use_override else root / "account-database-home")
        if use_override:
            env["TDESKTOP_MAC_PROFILE_TEST_HOME"] = str(home)
        else:
            env.pop("TDESKTOP_MAC_PROFILE_TEST_HOME", None)
        if mode == "failure":
            result = subprocess.run(
                [sys.executable, str(app), source, mode], env=env, check=False
            )
            if result.returncode != 23:
                raise SystemExit(f"The simulated {source} failure returned the wrong status.")
        else:
            process = subprocess.Popen(
                [sys.executable, str(app), source, mode], env=env
            )
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not all(path.is_file() for path in expected):
                if process.poll() is not None:
                    raise SystemExit(f"The simulated {source} timeout exited early.")
                time.sleep(0.01)
            if not all(path.is_file() for path in expected):
                process.kill()
                process.wait()
                raise SystemExit(f"The simulated {source} app did not write its logs.")
            try:
                process.communicate(timeout=0.25)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
            else:
                raise SystemExit(f"The simulated {source} app did not time out.")
        for path, contents in expected.items():
            if not path.is_file() or path.read_text(encoding="utf-8") != contents:
                raise SystemExit(f"The {source} {mode} artifact log was not retained: {path}")

print("macOS startup and profile logs retained after failure and timeout: PASS")
PY

# The full chat info regression reads the Ninja Multi-Config tree under out/.
regression_step='Full chat info session regression.'
dwarf_address_helper='.github/scripts/extract_dwarf_subprogram_address.py'
dwarf_info_fixture="$temp_dir/dwarf-info-summary.txt"
cat > "$dwarf_info_fixture" <<'DWARF'
0x00000000: DW_TAG_compile_unit
              DW_AT_name ("OtherRegression.cpp")
              DW_AT_low_pc (0x0000000100001000)
0x00000032: DW_TAG_subprogram
              DW_AT_name ("RunAccountLifecycleRegression")
              DW_AT_low_pc (0x0000000100001100)
0x00000064: DW_TAG_compile_unit
              DW_AT_name ("Telegram/SourceFiles/tests/account_lifecycle_regression.cpp")
              DW_AT_low_pc (0x0000000100002000)
0x00000096: DW_TAG_subprogram
              DW_AT_name ("RunAccountLifecycleRegression")
              DW_AT_low_pc (0x0000000100002100)
0x000000c8: DW_TAG_subprogram
              DW_AT_name ("OtherRegression")
              DW_AT_low_pc (0x0000000100003100)
DWARF
if ! dwarf_probe_address="$(python3 "$dwarf_address_helper" \
    RunAccountLifecycleRegression account_lifecycle_regression.cpp \
    "$dwarf_info_fixture" 2>"$temp_dir/dwarf-parser.err")"; then
  cat "$temp_dir/dwarf-parser.err" >&2
  contract_fail "$dwarf_address_helper must find the requested subprogram address."
fi
expect_equal 'DWARF probe skips the compile-unit low_pc' \
  "$dwarf_probe_address" '0x0000000100002100'
require_step_field "$mac_workflow" build-macos "$regression_step" '.if' \
  "\${{ !cancelled() && steps.build.outcome == 'success' && (github.event_name == 'workflow_dispatch' || (github.event_name == 'pull_request' && github.event.label.name == 'macos-build')) }}"
regression_timeout_minutes="$(step_field "$mac_workflow" build-macos \
  "$regression_step" '.timeout-minutes')"
[[ "$regression_timeout_minutes" == 30 ]] \
  || contract_fail 'Full chat info session timeout must cover dSYM generation and replay.'
require_step_script "$mac_workflow" build-macos "$regression_step" \
  'exec > >(tee "$diagnostics/harness.log") 2>&1' \
  'RUNNER_EXTERNAL_ACTIVATION_HOME="$HOME"' \
  'TDESKTOP_AUTH_LIFECYCLE_REGRESSION_RUNNER_ACTIVATION_REQUIRED=1' \
  'TDESKTOP_AUTH_LIFECYCLE_REGRESSION_RUNNER_ACTIVATION_REQUEST="$diagnostics/runner-external-activation.request"' \
  'build_dir="$repo/out"' \
  'cache_file="$build_dir/CMakeCache.txt"' \
  'compile_commands="$build_dir/compile_commands.json"' \
  'full_chat_info_session_regression=FAIL' \
  'ninja -C "$build_dir" -f build-Debug.ninja -t commands Telegram' \
  '--find=RunAccountLifecycleRegression' \
  '--show-parents' \
  'DW_AT_name' \
  'DW_AT_stmt_list' \
  'DW_AT_low_pc' \
  'python3 "$repo/.github/scripts/extract_dwarf_subprogram_address.py" "$dwarf_probe_name" account_lifecycle_regression.cpp "$diagnostics/dwarf-info-summary.txt"' \
  '--lookup="$probe_address"' \
  'grep -Fq "$dwarf_lookup" "$diagnostics/dwarf-line-summary.txt"' \
  'Deferred switch mutation evidence: mutation=remove-dispatch' \
  'Deferred switch mutation evidence: mutation=duplicate-dispatch'
reject_step_script "$mac_workflow" build-macos "$regression_step" \
  '--debug-info --arch=arm64' \
  '--debug-line --arch=arm64' \
  'head -n 1'
# The request decisions are implemented and unit-tested in the helper module;
# the workflow only connects that state machine to LLDB and the app marker.
require_step_python "$mac_workflow" build-macos "$regression_step" \
  "activation_handshake = ActivationHandshake(Path(os.path.dirname(stderr_path)) / 'runner-external-activation.status', runtime_seconds=app_runtime_timeout)" \
  'activation_attempt = activation_handshake.begin(activation_request)' \
  'marker_error = activation_handshake.complete(activation_attempt, activation_succeeded)' \
  'app_startup_timeout = 300' \
  'app_runtime_timeout = 180' \
  "signal_pid(app_pid, 'Teagram', cleanup_signal)"
require_step_python "$mac_workflow" build-macos "$regression_step" \
  "activation_helper = os.path.join(scripts_dir, 'macos_process_activation.py')" \
  'activation_environment = os.environ.copy()' \
  "activation_environment['HOME'] = os.environ['RUNNER_EXTERNAL_ACTIVATION_HOME']" \
  'activation_result = run_with_runtime_deadline([sys.executable, activation_helper, str(requested_pid)], runtime_deadline=activation_handshake.runtime_deadline, timeout=220, capture_output=True, text=True, check=False, env=activation_environment)'
require_step_script "$mac_workflow" build-macos "$regression_step" \
  'while process.poll() is None:' \
  'activation_handshake.begin(' \
  'if not activation_handshake.all_requests_passed:'
require_step_script "$mac_workflow" build-macos "$regression_step" \
  'except subprocess.TimeoutExpired as error:' \
  'timeout_phase = "runtime"' \
  'break'
require_step_script "$mac_workflow" build-macos "$regression_step" \
  'process handle SIGUSR2 -p true -s false -n false'
require_step_script "$mac_workflow" build-macos "$regression_step" \
  'if not activation_handshake.all_requests_passed:' \
  'HARNESS ERROR: runner did not activate and verify ' \
  'Teagram as frontmost before the lifecycle regression.'
require_step_script "$mac_workflow" build-macos "$regression_step" \
  'TDESKTOP_AUTH_LIFECYCLE_REGRESSION_RUNNER_ACTIVATION_MARKER="$diagnostics/runner-external-activation.status"'
grep -Fq 'request.write(requestContents)' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The lifecycle app must publish its PID for runner activation.'
grep -Fq 'WaitForRegressionCondition(' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The lifecycle app must wait for the runner activation marker.'
grep -Fq 'WaitForRunnerExternalActivationForRegression()' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The lifecycle app must wait for runner activation before focus checks.'
grep -Fq 'activateSettingsWindow(&stockController->window())' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The stock message-link matrix must refresh runner activation.'
grep -Fq 'activateSettingsWindow(&pinnedController->window())' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The pinned message-link matrix must refresh runner activation.'
grep -Fq 'RunnerExternalActivationMarkerRegressionPasses()' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The lifecycle activation marker must keep its self-test.'
grep -Fq '"TDESKTOP_AUTH_LIFECYCLE_REGRESSION_RUNNER_ACTIVATION_REQUIRED"' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The lifecycle activation self-test must read the opt-in from the environment.'
regression_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Full chat info session regression.") | .key')"
summary_step_index="$(job_field "$mac_workflow" build-macos \
  '.steps | to_entries[] | select(.value.name == "Summarize on-demand validation.") | .key')"
((summary_step_index > regression_step_index)) \
  || contract_fail 'On-demand validation summary must run after the lifecycle regression outcome exists.'
profile_step='Run protected profile and IPC integration tests.'
require_step_field "$mac_workflow" build-macos "$profile_step" '.if' \
  "\${{ !cancelled() && steps.ccache-profile.outputs.protected_path_integration == 'ON' && steps.build.outcome == 'success' }}"
require_step_script "$mac_workflow" build-macos "$profile_step" \
  'RUNNER_EXTERNAL_ACTIVATION_HOME="$HOME"' \
  'test_mac_profile_guard.sh'
profile_harness='Telegram/build/test_mac_profile_guard.sh'
for activation_contract in \
  'TDESKTOP_AUTH_LIFECYCLE_REGRESSION_RUNNER_ACTIVATION_REQUIRED=1' \
  'TDESKTOP_AUTH_LIFECYCLE_REGRESSION_RUNNER_ACTIVATION_MARKER=' \
  'TDESKTOP_AUTH_LIFECYCLE_REGRESSION_RUNNER_ACTIVATION_REQUEST=' \
  'macos_process_activation.py' \
  '--watch'; do
  grep -Fq -- "$activation_contract" "$profile_harness" \
    || contract_fail "$profile_harness must externally activate its authenticated lifecycle launch."
done
awk '
  /if ! : > "\$activation_stop"/ { in_stop_failure = 1 }
  in_stop_failure && /kill -TERM "\$activation_pid"/ { watcher_stopped = 1 }
  in_stop_failure && /wait "\$activation_pid"/ && watcher_stopped {
    watcher_reaped = 1
  }
  in_stop_failure && /activation stop file could not be written: %s/ {
    path_reported = 1
  }
  in_stop_failure && /return 1/ { exit }
  END { exit !(watcher_stopped && watcher_reaped && path_reported) }
' "$profile_harness" \
  || contract_fail "$profile_harness must stop and reap the activation watcher when its stop-file write fails."
grep -Fq 'test_profile_runner_serves_multiple_requests_until_stop' \
  .github/scripts/test_macos_process_activation.py \
  || contract_fail 'Runner activation tests must cover multiple profile requests.'
minidump_symbolizer='.github/scripts/symbolicate_macos_minidump.py'
python3 "$minidump_symbolizer" --self-test
for symbolizer_contract in \
	symbolicate_font_startup_crash \
	"$minidump_symbolizer" \
	'"$dump" "$APP"'; do
	grep -Fq "$symbolizer_contract" "$profile_harness" \
		|| contract_fail "$profile_harness must symbolicate font startup minidumps."
done
if grep -Fq 'dsymutil "$APP"' "$profile_harness"; then
	contract_fail "$profile_harness must not generate a dSYM in the timed startup fixture."
fi
settings_link_evidence='Settings deep link regression passed: paid and story routes'
grep -Fq "$settings_link_evidence" \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'The Settings deep link regression must report its current success evidence.'
grep -Fq "\"$settings_link_evidence" "$profile_harness" \
  || contract_fail "$profile_harness must check the Settings deep link regression success evidence."
grep -Fq 'First messages\.getDialogs handler latency regression:' "$profile_harness" \
  || contract_fail "$profile_harness must assert seeded first-getDialogs latency output."
grep -Fq 'requests=1 parsed=1 chats=2 dialogs=1 messages=73 documents=73' \
  "$profile_harness" \
  || contract_fail "$profile_harness must require a parsed two-chat, 73-document getDialogs response."
grep -Fq 'minimized_restore_budget_ns=50000000' "$profile_harness" \
  || contract_fail "$profile_harness must retain the minimized restore budget in the report."
awk '
  /^local watchdog_evidence$/ { copy = 1 }
  copy && /^if ! grep -F -q/ { exit }
  copy { print }
' "$profile_harness" > "$temp_dir/cache-watchdog-check.sh"
[[ -s "$temp_dir/cache-watchdog-check.sh" ]] \
  || contract_fail 'The cache watchdog evidence checks were not found.'
printf '%s\n' \
  'Cache watchdog regression: seeded_directories=125 seeded_files=169 cache_accesses=292 duration_ns=29999999 stalls=0.' \
  > "$temp_dir/cache-watchdog.log"
: > "$temp_dir/cache-profile.log"
check_cache_watchdog_evidence() (
  local AUTH_LOG="$1"
  local PROFILE_LOG="$temp_dir/cache-profile.log"
  source "$temp_dir/cache-watchdog-check.sh"
)
check_cache_watchdog_evidence "$temp_dir/cache-watchdog.log" >/dev/null 2>&1 \
  || contract_fail 'The 292-access watchdog evidence must pass below 30 ms.'
sed 's/duration_ns=29999999/duration_ns=30000000/' \
  "$temp_dir/cache-watchdog.log" > "$temp_dir/cache-watchdog-over-budget.log"
if check_cache_watchdog_evidence "$temp_dir/cache-watchdog-over-budget.log" >/dev/null 2>&1; then
  contract_fail 'The 292-access watchdog budget must remain a gate.'
fi
sed 's/stalls=0/stalls=1/' \
  "$temp_dir/cache-watchdog.log" > "$temp_dir/cache-watchdog-stall.log"
if check_cache_watchdog_evidence "$temp_dir/cache-watchdog-stall.log" >/dev/null 2>&1; then
  contract_fail 'The 292-access watchdog stall check must remain a gate.'
fi
awk '
  /^local untraced_wall_overruns$/ { copy = 1 }
  copy && /^if ! grep -F -q/ { exit }
  copy { print }
' "$profile_harness" > "$temp_dir/cache-latency-check.sh"
[[ -s "$temp_dir/cache-latency-check.sh" ]] \
  || contract_fail 'The cache latency evidence checks were not found.'
awk '
  /^work_value_within_budget\(\) \{$/ { copy = 1 }
  /^main_thread_cpu_work_within_budget\(\) \{$/ { copy = 1 }
  copy { print }
  copy && /^}$/ { copy = 0 }
' "$profile_harness" > "$temp_dir/main-thread-budget-gates.sh"
[[ -s "$temp_dir/main-thread-budget-gates.sh" ]] \
  || contract_fail 'The profile work budget predicates were not found.'
source "$temp_dir/main-thread-budget-gates.sh"
awk '
  /^main_thread_work_estimate_within_budget\(\) \{$/ { copy = 1 }
  copy { print }
  copy && /^}$/ { exit }
' "$profile_harness" > "$temp_dir/main-thread-work-gate.sh"
[[ -s "$temp_dir/main-thread-work-gate.sh" ]] \
  || contract_fail 'The blocked-time main-thread work budget predicate was not found.'
source "$temp_dir/main-thread-work-gate.sh"
awk '
  /^capture_stuck_profile_process\(\) \{$/ { copy = 1 }
  copy { print }
  copy && /^}$/ { exit }
' "$profile_harness" > "$temp_dir/profile-hang-sample.sh"
[[ -s "$temp_dir/profile-hang-sample.sh" ]] \
  || contract_fail 'A stuck profile process must retain a diagnostic sample.'
# shellcheck disable=SC1091
source "$temp_dir/profile-hang-sample.sh"
# shellcheck disable=SC2329
sample() {
  printf '%s\n' "$@" > "$temp_dir/profile-hang-sample-args"
  return 1
}
capture_stuck_profile_process 123 "$temp_dir/first-instance.sample.txt"
[[ "$(cat "$temp_dir/profile-hang-sample-args")" == \
  "$(printf '%s\n' 123 5 -file "$temp_dir/first-instance.sample.txt")" ]] \
  || contract_fail 'A stuck profile process must sample the exact child PID.'
unset -f sample
if ! main_thread_work_estimate_within_budget \
  0 100000000 RRRRR 50000000 0,25000000,50000000,75000000,100000000; then
  contract_fail 'Runnable samples 25 ms apart must cover the window.'
fi
if ! main_thread_work_estimate_within_budget \
  0 104000000 RRRRR 50000000 0,26000000,52000000,78000000,104000000; then
  contract_fail 'An uncovered 26 ms sample gap must use main-thread CPU work.'
fi
if main_thread_work_estimate_within_budget \
  0 100000000 RRRRW 25000000 0,25000000,50000000,75000000,100000000; then
  contract_fail 'A waiting end sample must charge the whole preceding gap.'
fi
if ! main_thread_work_estimate_within_budget \
  0 100000000 RRRRW 25000001 0,25000000,50000000,75000000,100000000; then
  contract_fail 'A waiting end sample must charge exactly its preceding gap.'
fi
if main_thread_work_estimate_within_budget \
  0 100000000 RRURR 50000000 0,25000000,50000000,75000000,100000000; then
  contract_fail 'An uninterruptible sample must charge both adjacent gaps.'
fi
if ! main_thread_work_estimate_within_budget \
  0 100000000 RRURR 50000001 0,25000000,50000000,75000000,100000000; then
  contract_fail 'Adjacent blocked gaps must be charged exactly once.'
fi
if main_thread_work_estimate_within_budget \
  0 100000000 RRRR 25000000 10000000,35000000,60000000,85000000; then
  contract_fail 'Unobserved window edges must count as blocked time.'
fi
if ! main_thread_work_estimate_within_budget \
  0 100000000 RRRR 25000001 10000000,35000000,60000000,85000000; then
  contract_fail 'Unobserved window edges must be charged exactly once.'
fi
if ! main_thread_work_estimate_within_budget \
  0 100000000 RRRR 50000000 26000000,50000000,75000000,100000000; then
	contract_fail 'A 26 ms uncovered edge must use main-thread CPU work.'
fi
if ! main_thread_work_estimate_within_budget \
  45000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  R 5000000 R 5000000; then
  contract_fail 'Runnable samples bracketing both edges must cover them.'
fi
if main_thread_work_estimate_within_budget \
  45000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  W 5000000 R 5000000; then
  contract_fail 'A waiting sample before the window must charge its covered edge.'
fi
if main_thread_work_estimate_within_budget \
  45000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  R 5000000 U 5000000; then
  contract_fail 'An uninterruptible sample after the window must charge its covered edge.'
fi
if ! main_thread_work_estimate_within_budget \
  45000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  R 20000000 R 5000000; then
  contract_fail 'A bracketing gap of 25 ms must cover the window edge.'
fi
if ! main_thread_work_estimate_within_budget \
  45000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  R 21000000 R 5000000; then
	contract_fail 'A bracketing gap of 26 ms must use main-thread CPU work.'
fi
if ! main_thread_work_estimate_within_budget \
  45000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  '?' 5000000 R 5000000; then
	contract_fail 'An unknown bracketing state must use main-thread CPU work.'
fi
if ! main_thread_work_estimate_within_budget \
  45000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  R -1 R 5000000; then
	contract_fail 'A negative bracketing distance must use main-thread CPU work.'
fi
if main_thread_work_estimate_within_budget \
  52000000 60000000 RRRR 50000000 5000000,25000000,45000000,55000000 \
  R 5000000 R 5000000; then
  contract_fail 'Runnable brackets must not excuse over-budget main-thread CPU.'
fi
startup_offsets=""
for ((offset = 0; offset <= 100000000; offset += 5000000)); do
  startup_offsets="${startup_offsets:+$startup_offsets,}$offset"
done
chat_switch_offsets=""
for ((offset = 0; offset <= 225000000; offset += 5000000)); do
  chat_switch_offsets="${chat_switch_offsets:+$chat_switch_offsets,}$offset"
done
callback_offsets=""
for ((offset = 0; offset <= 200000000; offset += 5000000)); do
  callback_offsets="${callback_offsets:+$callback_offsets,}$offset"
done
sparse_startup_offsets=""
for ((offset = 0; offset <= 225000000; offset += 12500000)); do
  sparse_startup_offsets="${sparse_startup_offsets:+$sparse_startup_offsets,}$offset"
done
if main_thread_work_estimate_within_budget \
  0 101000000 "$(printf '%21s' '' | tr ' ' W)" 100000000 \
  "$startup_offsets"; then
  contract_fail 'A fully blocked 101 ms window must fail the startup budget.'
fi
if ! main_thread_work_estimate_within_budget \
  18000000 225000000 "$(printf '%46s' '' | tr ' ' R)" 100000000 \
  "$chat_switch_offsets"; then
  contract_fail 'A runnable 225 ms window with 18 ms CPU must pass.'
fi
if ! main_thread_work_estimate_within_budget \
  18000000 225000000 \
  "$(printf '%15s' '' | tr ' ' R)$(printf '%4s' '' | tr ' ' W)" \
  100000000 "$sparse_startup_offsets"; then
  contract_fail 'A 12.5 ms gap must count blocked intervals and remain covered.'
fi
six_millisecond_offsets=""
for ((offset = 0; offset <= 222000000; offset += 6000000)); do
  six_millisecond_offsets="${six_millisecond_offsets:+$six_millisecond_offsets,}$offset"
done
six_millisecond_trace="$(printf '%38s' '' | tr ' ' R)"
if ! main_thread_work_estimate_within_budget \
  18000000 225000000 "$six_millisecond_trace" 100000000 \
  "$six_millisecond_offsets"; then
  contract_fail 'A 6 ms sample gap must remain covered.'
fi
if main_thread_work_estimate_within_budget \
	18000000 225000000 RRRW 100000000 0,75000000,150000000,225000000; then
	contract_fail 'A four-sample 75 ms gap with a waiting sample must fail on wall time.'
fi
if ! main_thread_work_estimate_within_budget 0 101000000 none 100000000 ""; then
	contract_fail 'A window without scheduler samples must use main-thread CPU work.'
fi
callback_runnable_trace="$(printf '%41s' '' | tr ' ' R)"
callback_unknown_trace="${callback_runnable_trace:0:20}?${callback_runnable_trace:21}"
if main_thread_work_estimate_within_budget \
  12000000 200000000 "$callback_unknown_trace" 50000000 "$callback_offsets"; then
	contract_fail 'An unknown scheduler state must use wall time.'
fi
callback_negative_offsets="-1${callback_offsets#0}"
if ! main_thread_work_estimate_within_budget \
  12000000 200000000 "$callback_runnable_trace" 50000000 \
  "$callback_negative_offsets"; then
	contract_fail 'An invalid scheduler offset must use main-thread CPU work.'
fi
if ! main_thread_work_estimate_within_budget \
	0 200000000 RRR 100000000 0,1000000,2000000; then
	contract_fail 'A clustered runnable trace must use main-thread CPU work.'
fi
if ! main_thread_work_estimate_within_budget \
  0 200000000 RRRRR 100000000 0,5000000,10000000,190000000,200000000; then
  contract_fail 'A long unobserved middle interval must use main-thread CPU work.'
fi
if main_thread_work_estimate_within_budget \
	1000000 200000000 RW 100000000 0,30000000; then
	contract_fail 'A 30 ms uncovered gap with a waiting sample must fail on wall time.'
fi
if ! main_thread_work_estimate_within_budget \
	1000000 200000000 RR 100000000 0,30000000; then
	contract_fail 'A 30 ms uncovered gap with only running samples must use CPU work.'
fi
if main_thread_work_estimate_within_budget \
  18000000 225000000 \
  "$(printf '%26s' '' | tr ' ' R)$(printf '%20s' '' | tr ' ' W)" \
  100000000 "$chat_switch_offsets"; then
  contract_fail 'A covered trace with a blocked tail must exceed the startup budget.'
fi
if ! main_thread_work_estimate_within_budget \
  17990000 225000000 "$(printf '%46s' '' | tr ' ' R)" 100000000 \
  "$chat_switch_offsets"; then
  contract_fail 'A covered runnable window with 17.99 ms CPU must pass.'
fi
if ! main_thread_work_estimate_within_budget \
  1138000 105113167 none 100000000 ''; then
  contract_fail 'A long wall delay with 1.138 ms CPU and no trace must pass the timing budget.'
fi
if main_thread_work_estimate_within_budget \
  101000000 105113167 none 100000000 ''; then
  contract_fail 'A 101 ms main-thread CPU stall must fail without scheduler evidence.'
fi
if main_thread_work_estimate_within_budget \
  12000000 200000000 "$(printf '%41s' '' | tr ' ' W)" 50000000 \
  "$callback_offsets"; then
  contract_fail 'A 200 ms blocked attach-menu callback must fail its 50 ms budget.'
fi
if ! main_thread_work_estimate_within_budget \
  12000000 200000000 none 50000000 ""; then
	contract_fail 'An attach-menu callback without scheduler evidence must use main-thread CPU work.'
fi
cat > "$temp_dir/cache-latency.log" <<'EOF'
Cache access latency regression: cache=cache seeded_directories=125 seeded_files=169 closed_clear_missing_active=1 cache_initialization_ns=585165042 initialization_allowed=1 queued_ops_before_ready=1 queued_ops_passed=1 cache_usable_ns=585252834 under_10s=1 max_main_thread_work_ns=17990000 max_startup_estimated_work_ns=66903043 max_heartbeat_gap_ns=230059167 max_heartbeat_gap_work_ns=17990000 heartbeat_gap_process_work_ns=117600000 heartbeat_gap_started_at=2026-11-06T00:47:53.000Z startup_heartbeat_gap_count=1 startup_uncovered_heartbeat_gaps=1 startup_heartbeat_gap_coverage_proven=0 startup_stall_count=1 max_startup_stall_ns=225000000 startup_stall_main_thread_work_ns=17990000 startup_stall_process_work_ns=117600000 startup_stall_started_at=2026-11-06T00:47:53.000Z startup_stall_heartbeat_gap_started_at=2026-11-06T00:47:53.000Z startup_stall_scheduler_samples=46 startup_stall_scheduler_running=36 startup_stall_scheduler_waiting=10 startup_stall_scheduler_uninterruptible=0 startup_stall_scheduler_other=0 startup_stall_scheduler_trace=RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRWWWWWWWWWW startup_stall_stack_captured=1 startup_stall_evidence_matched=1 startup_stall_scheduler_preemption_proven=0 startup_within_budget=1 first_access_ns=86917 under_50ms=1 root_swap_refused=1 direct_fallback_closed=1 root_restored=1 startup_walks=2 main_thread_walks=0.
Cache startup watchdog stall: cache=cache started_at=2026-11-06T00:47:53.000Z duration_ms=230 heartbeat_gap_started_at=2026-11-06T00:47:53.000Z scheduler_trace=WWWW stack_captured=1 stack=worker wait regression
Cache access latency regression: cache=media_cache seeded_directories=125 seeded_files=169 closed_clear_missing_active=1 cache_initialization_ns=120678959 initialization_allowed=1 queued_ops_before_ready=1 queued_ops_passed=1 cache_usable_ns=120763125 under_10s=1 max_main_thread_work_ns=48247625 max_startup_estimated_work_ns=48247625 max_heartbeat_gap_ns=48247625 max_heartbeat_gap_work_ns=48247625 heartbeat_gap_process_work_ns=25000000 heartbeat_gap_started_at=2026-11-06T00:47:53.000Z startup_heartbeat_gap_count=0 startup_uncovered_heartbeat_gaps=0 startup_heartbeat_gap_coverage_proven=1 startup_stall_count=0 max_startup_stall_ns=0 startup_stall_main_thread_work_ns=0 startup_stall_process_work_ns=0 startup_stall_started_at=none startup_stall_heartbeat_gap_started_at=none startup_stall_scheduler_samples=0 startup_stall_scheduler_running=0 startup_stall_scheduler_waiting=0 startup_stall_scheduler_uninterruptible=0 startup_stall_scheduler_other=0 startup_stall_scheduler_trace=none startup_stall_stack_captured=0 startup_stall_evidence_matched=0 startup_stall_scheduler_preemption_proven=0 startup_within_budget=1 first_access_ns=83292 under_50ms=1 root_swap_refused=1 direct_fallback_closed=1 root_restored=1 startup_walks=2 main_thread_walks=0.
First messages.getDialogs handler latency regression: seeded_directories=125 seeded_files=169 requests=1 parsed=1 chats=2 dialogs=1 messages=73 documents=73 cache_walks=2 max_handler_ns=9893666 main_thread_cpu_ns=9893666 blocked_estimate_ns=0 estimated_work_ns=9893666 trace=R within_budget=1 cpu_only_within_budget=1 runner_noise_eligible=0 under_50ms=1.
Initial history selection: peer_id=281474977760707 wall_ns=135344750 main_thread_work_ns=40000000 blocked_estimate_ns=0 estimated_work_ns=40000000 budget_ns=50000000 trace=R within_budget=1 cpu_only_within_budget=1 scheduler_preemption_proven=0.
Cache chat switch latency regression: seeded_directories=125 seeded_files=169 reactions=73 hydrated_documents=292 chat_switches=73 attach_menu_bot_count=20 minimized_restore=1 initial_history_window_activated=1 all_chat_switch_windows_activated=1 minimized_switch_ns=45000000 minimized_restore_budget_ns=50000000 max_hydration_ns=5210625 max_hydration_work_ns=4000000 hydration_scheduler_preemption_proven=0 max_switch_ns=45000000 max_switch_work_ns=40000000 max_recurring_switch_work_ns=40000000 max_switch_estimated_work_ns=40000000 max_recurring_switch_estimated_work_ns=40000000 max_switch_index=3 max_switch_peer_id=281474976711707 switch_scheduler_preemption_proven=0 attach_menu_first_hover_ns=12000000 attach_menu_hover_ns=92000000 attach_menu_update_hover_ns=11000000 attach_menu_uncached_peer_ns=20000000 attach_menu_first_hover_callback_cpu_ns=12000000 attach_menu_update_hover_callback_cpu_ns=1000000 attach_menu_uncached_peer_callback_cpu_ns=20000000 attach_menu_first_hover_callback_wall_ns=92000000 attach_menu_update_hover_callback_wall_ns=92000000 attach_menu_uncached_peer_callback_wall_ns=92000000 attach_menu_scheduler_preemption_proven=0 attach_menu_cached=1 attach_menu_invalidated=1 attach_menu_build_deferred=1 attach_menu_uncached_ready=1 attach_menu_uncached_deferred_failures=0 attach_menu_uncached_missing_bot_failures=0 attach_menu_uncached_build_count_failures=0 attach_menu_cache_bound=1 pending_reveal_after_controls_geometry=1 chat_switch_compose_ui_reused=1 translate_bar_reused=1 translate_bar_tracks_history=1 field_autocomplete_suggestions_ready=1 menu_commands_unfocused=1 bot_commands_unfocused=1 history_chat_mode_invalidation_rebuilt=1 dialog_histories_ready=1 synthetic_histories_ready=1 peer_scoped_update_routing=1 requests_bar_empty_deferred=1 requests_bar_created_after_pending_update=1 max_stall_ns=40000000 within_budgets=1.
Attach-menu callback work: cached_hover_wall_ns=92000000 cached_hover_estimated_work_ns=12000000 first_hover_estimated_work_ns=12000000 invalidation_estimated_work_ns=1000000 uncached_peer_estimated_work_ns=20000000 budget_ns=50000000.
EOF
sed \
  -e 's/first_access_ns=86917 under_50ms=1/first_access_ns=86917 first_access_main_thread_cpu_ns=80000 first_access_blocked_estimate_ns=0 first_access_estimated_work_ns=80000 first_access_trace=R first_access_within_budget=1 first_access_cpu_only_within_budget=1 under_50ms=1/' \
  -e 's/first_access_ns=83292 under_50ms=1/first_access_ns=83292 first_access_main_thread_cpu_ns=80000 first_access_blocked_estimate_ns=0 first_access_estimated_work_ns=80000 first_access_trace=R first_access_within_budget=1 first_access_cpu_only_within_budget=1 under_50ms=1/' \
  "$temp_dir/cache-latency.log" > "$temp_dir/cache-latency-with-cpu.log"
mv "$temp_dir/cache-latency-with-cpu.log" "$temp_dir/cache-latency.log"
check_cache_latency_evidence() (
  local AUTH_LOG="$1"
  source "$temp_dir/cache-latency-check.sh"
)
if ! check_cache_latency_evidence "$temp_dir/cache-latency.log" \
  > "$temp_dir/cache-latency-clean-output.log" 2>&1; then
  contract_fail 'A 225 ms startup gap with scheduler evidence under the 100 ms work budget must pass.'
fi
if ! grep -Fqx 'untraced_wall_overrun_count=0' \
  "$temp_dir/cache-latency-clean-output.log"; then
  contract_fail 'A clean timing run must report a numeric zero untraced-overrun count.'
fi
sed '/^First messages.getDialogs handler latency regression:/ {
  s/max_handler_ns=9893666/max_handler_ns=60000000/
  s/main_thread_cpu_ns=9893666/main_thread_cpu_ns=1000000/
  s/blocked_estimate_ns=0/blocked_estimate_ns=-1/
  s/estimated_work_ns=9893666/estimated_work_ns=1000000/
  s/trace=R/trace=none/
  s/within_budget=1/within_budget=0/
  s/runner_noise_eligible=0/runner_noise_eligible=1/
  s/under_50ms=1/under_50ms=0/
}' "$temp_dir/cache-latency.log" > "$temp_dir/cache-latency-getdialogs-runner-pause.log"
printf '%s\n' \
  'HARNESS ERROR: getDialogs scheduler trace unavailable: window_start_steady_ns=1000000000 wall_ns=60000000 main_thread_cpu_ns=1000000 budget_ns=50000000 runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-getdialogs-runner-pause.log"
if ! check_cache_latency_evidence \
  "$temp_dir/cache-latency-getdialogs-runner-pause.log" \
  > "$temp_dir/cache-latency-getdialogs-runner-pause-output.log" 2>&1; then
  contract_fail 'A 60 ms getDialogs handler with 1 ms CPU and no trace must pass once as runner noise.'
fi
if ! grep -Fq 'untraced_wall_overrun_count=1' \
  "$temp_dir/cache-latency-getdialogs-runner-pause-output.log"; then
  contract_fail 'The getDialogs runner pause must count as one untraced incident.'
fi
if ! grep -Fq 'trace=none within_budget=0 cpu_only_within_budget=1 runner_noise_eligible=1 under_50ms=0.' \
  "$temp_dir/cache-latency-getdialogs-runner-pause.log"; then
  contract_fail 'The getDialogs under_50ms field must reflect its 60 ms wall time.'
fi
if ! grep -Fq '::warning:: HARNESS ERROR: getDialogs scheduler trace unavailable' \
  "$temp_dir/cache-latency-getdialogs-runner-pause-output.log"; then
  contract_fail 'A missing getDialogs scheduler trace must be reported as a warning.'
fi
sed '/^First messages.getDialogs handler latency regression:/ {
  s/max_handler_ns=9893666/max_handler_ns=60000000/
  s/main_thread_cpu_ns=9893666/main_thread_cpu_ns=1000000/
  s/blocked_estimate_ns=0/blocked_estimate_ns=10000000/
  s/estimated_work_ns=9893666/estimated_work_ns=11000000/
  s/trace=R/trace=RR/
  s/under_50ms=1/under_50ms=0/
}' "$temp_dir/cache-latency.log" > "$temp_dir/cache-latency-getdialogs-traced-under-budget.log"
if ! check_cache_latency_evidence \
  "$temp_dir/cache-latency-getdialogs-traced-under-budget.log" \
  >/dev/null 2>&1; then
  contract_fail 'A traced getDialogs handler must pass from its timing verdict when wall time exceeds 50 ms.'
fi
cp "$temp_dir/cache-latency.log" "$temp_dir/cache-latency-responsive.log"
sed -e 's/startup_stall_count=1/startup_stall_count=0/' \
  -e 's/max_startup_stall_ns=225000000/max_startup_stall_ns=0/' \
  -e 's/startup_stall_main_thread_work_ns=17990000/startup_stall_main_thread_work_ns=0/' \
  -e 's/startup_stall_process_work_ns=117600000/startup_stall_process_work_ns=0/' \
  -e 's/startup_stall_started_at=2026-11-06T00:47:53.000Z/startup_stall_started_at=none/' \
  -e 's/startup_stall_heartbeat_gap_started_at=2026-11-06T00:47:53.000Z/startup_stall_heartbeat_gap_started_at=none/' \
  -e 's/startup_stall_scheduler_samples=46/startup_stall_scheduler_samples=0/' \
  -e 's/startup_stall_scheduler_running=36/startup_stall_scheduler_running=0/' \
  -e 's/startup_stall_scheduler_waiting=10/startup_stall_scheduler_waiting=0/' \
  -e 's/startup_stall_scheduler_trace=RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRWWWWWWWWWW/startup_stall_scheduler_trace=none/' \
  -e 's/startup_stall_stack_captured=1/startup_stall_stack_captured=0/' \
  -e 's/startup_stall_evidence_matched=1/startup_stall_evidence_matched=0/' \
  -e 's/max_startup_estimated_work_ns=66903043/max_startup_estimated_work_ns=17990000/' \
  -e '/^Cache startup watchdog stall:/d' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-no-watchdog-evidence.log"
printf '%s\n' \
  'Cache startup work window: started_at=2026-11-06T00:47:53.000Z wall_ns=105113167 main_thread_cpu_ns=1138000 blocked_estimate_ns=-1 estimated_work_ns=1138000 budget_ns=100000000 trace=none within_budget=0 cpu_work_within_budget=1 scheduler_evidence_available=0.' \
  'HARNESS ERROR: startup scheduler trace unavailable: runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-no-watchdog-evidence.log"
if ! check_cache_latency_evidence "$temp_dir/cache-latency-no-watchdog-evidence.log" \
  > "$temp_dir/cache-latency-no-watchdog-error.log" 2>&1; then
  contract_fail 'A missing startup trace with low CPU must not fail the profile step.'
fi
if ! grep -Fq '::warning:: HARNESS ERROR: startup scheduler trace unavailable' \
  "$temp_dir/cache-latency-no-watchdog-error.log"; then
  contract_fail 'A missing startup trace must be reported as a warning.'
fi
sed \
  -e 's/max_main_thread_work_ns=17990000/max_main_thread_work_ns=1138000/' \
  -e 's/max_startup_estimated_work_ns=17990000/max_startup_estimated_work_ns=1138000/' \
  -e 's/max_heartbeat_gap_ns=230059167/max_heartbeat_gap_ns=105113167/' \
  -e 's/max_heartbeat_gap_work_ns=17990000/max_heartbeat_gap_work_ns=1138000/' \
  "$temp_dir/cache-latency-responsive.log" \
  > "$temp_dir/cache-latency-attempt-1.log"
printf '%s\n' \
  'Cache startup work window: started_at=2026-11-06T00:47:53.000Z wall_ns=105113167 main_thread_cpu_ns=1138000 blocked_estimate_ns=-1 estimated_work_ns=1138000 budget_ns=100000000 capture_pending=0 trace_samples=0 trace=none within_budget=0 cpu_only_within_budget=1 scheduler_capture_required=1 scheduler_evidence_available=0.' \
  'HARNESS ERROR: startup scheduler trace unavailable: started_at=2026-11-06T00:47:53.000Z window_start_steady_ns=1000000000 wall_ns=105113167 main_thread_cpu_ns=1138000 budget_ns=100000000 runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-attempt-1.log"
if ! check_cache_latency_evidence "$temp_dir/cache-latency-attempt-1.log" \
  > "$temp_dir/cache-latency-attempt-1-output.log" 2>&1; then
  contract_fail 'The attempt-1 startup sample must pass the profile step with low CPU and no trace.'
fi
if ! grep -Fq '::warning:: HARNESS ERROR: startup scheduler trace unavailable' \
  "$temp_dir/cache-latency-attempt-1-output.log"; then
  contract_fail 'The attempt-1 startup sample must report the missing trace as a warning.'
fi
if ! grep -Fq 'untraced_wall_overrun_count=1' \
  "$temp_dir/cache-latency-attempt-1-output.log"; then
  contract_fail 'The profile step must report the attempt-1 untraced overrun count.'
fi
if ! grep -Fq 'startup_within_budget=1' \
  "$temp_dir/cache-latency-attempt-1.log"; then
  contract_fail 'The attempt-1 startup verdict must be reported as startup_within_budget.'
fi
if grep -Fq 'under_100ms=' "$temp_dir/cache-latency-attempt-1.log"; then
  contract_fail 'The startup verdict must not be labeled as a wall-time threshold.'
fi
cp "$temp_dir/cache-latency-attempt-1.log" \
  "$temp_dir/cache-latency-overlapping-windows.log"
printf '%s\n' \
  'HARNESS ERROR: first-access scheduler trace unavailable: window_start_steady_ns=1050000000 wall_ns=105000000 main_thread_cpu_ns=1138000 budget_ns=50000000 runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-overlapping-windows.log"
if ! check_cache_latency_evidence \
  "$temp_dir/cache-latency-overlapping-windows.log" \
  > "$temp_dir/cache-latency-overlapping-windows-output.log" 2>&1; then
  contract_fail 'Overlapping startup and first-access runner pauses must count as one incident.'
fi
if ! grep -Fq 'untraced_wall_overrun_count=1' \
  "$temp_dir/cache-latency-overlapping-windows-output.log"; then
  contract_fail 'Overlapping timing windows must be deduplicated in the shell verdict.'
fi
cp "$temp_dir/cache-latency-attempt-1.log" \
  "$temp_dir/cache-latency-two-untraced-overruns.log"
printf '%s\n' \
  'HARNESS ERROR: chat-switch scheduler trace unavailable: index=0 label=initial-history-selection window_start_steady_ns=1300000000 wall_ns=135000000 main_thread_cpu_ns=1138000 budget_ns=50000000 runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-two-untraced-overruns.log"
if check_cache_latency_evidence \
  "$temp_dir/cache-latency-two-untraced-overruns.log" \
  > "$temp_dir/cache-latency-two-untraced-overruns-output.log" 2>&1; then
  contract_fail 'Two untraced negligible-CPU overruns in one profile step must fail.'
fi
if ! grep -Fq 'untraced_wall_overrun_count=2' \
  "$temp_dir/cache-latency-two-untraced-overruns-output.log"; then
  contract_fail 'The profile step must report both untraced overrun windows.'
fi
sed \
  -e '/^Cache access latency regression: cache=cache / { s/first_access_ns=86917/first_access_ns=105113167/; s/first_access_main_thread_cpu_ns=80000/first_access_main_thread_cpu_ns=1138000/; s/first_access_blocked_estimate_ns=0/first_access_blocked_estimate_ns=-1/; s/first_access_estimated_work_ns=80000/first_access_estimated_work_ns=1138000/; s/first_access_within_budget=1/first_access_within_budget=0/; s/first_access_trace=R/first_access_trace=none/; s/under_50ms=1/under_50ms=0/; }' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-first-access-cpu-only.log"
if ! grep -Fq 'first_access_ns=105113167 first_access_main_thread_cpu_ns=1138000 first_access_blocked_estimate_ns=-1 first_access_estimated_work_ns=1138000 first_access_trace=none first_access_within_budget=0 first_access_cpu_only_within_budget=1 under_50ms=0' \
  "$temp_dir/cache-latency-first-access-cpu-only.log"; then
  contract_fail 'First access must report the wall threshold and CPU-only assessment separately.'
fi
printf '%s\n' \
  'HARNESS ERROR: first-access scheduler trace unavailable: runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-first-access-cpu-only.log"
if ! check_cache_latency_evidence \
  "$temp_dir/cache-latency-first-access-cpu-only.log" \
  > "$temp_dir/cache-latency-first-access-cpu-only-output.log" 2>&1; then
  contract_fail 'A 105 ms first access with 1.138 ms CPU and no trace must pass the timing gate.'
fi
if ! grep -Fq '::warning:: HARNESS ERROR: first-access scheduler trace unavailable' \
  "$temp_dir/cache-latency-first-access-cpu-only-output.log"; then
  contract_fail 'A first-access sample without scheduler evidence must be reported as a warning.'
fi
sed -e 's/heartbeat_gap_process_work_ns=117600000/heartbeat_gap_process_work_ns=200000000/' \
  -e 's/startup_stall_process_work_ns=117600000/startup_stall_process_work_ns=200000000/' \
  "$temp_dir/cache-latency.log" > "$temp_dir/cache-latency-high-process-work.log"
if ! check_cache_latency_evidence "$temp_dir/cache-latency-high-process-work.log" >/dev/null 2>&1; then
  contract_fail 'Process CPU and scheduler diagnostics must not override bounded startup main-thread CPU.'
fi
sed 's/max_startup_estimated_work_ns=66903043/max_startup_estimated_work_ns=100000000/' \
  "$temp_dir/cache-latency.log" > "$temp_dir/cache-latency-slow-startup.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-slow-startup.log" >/dev/null 2>&1; then
  contract_fail 'The main-thread startup work budget must reject 100 ms or more.'
fi
sed -e 's/max_switch_ns=135344750/max_switch_ns=227000000/' \
  -e 's/max_recurring_switch_estimated_work_ns=40000000/max_recurring_switch_estimated_work_ns=50000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-slow-switch.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-slow-switch.log" >/dev/null 2>&1; then
  contract_fail 'The recurring chat-switch CPU budget must reject 50 ms or more.'
fi
sed -e 's/max_switch_work_ns=40000000/max_switch_work_ns=52107000/' \
  -e 's/max_switch_estimated_work_ns=40000000/max_switch_estimated_work_ns=52107000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-slow-max-switch.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-slow-max-switch.log" >/dev/null 2>&1; then
  contract_fail 'The maximum chat-switch CPU budget must reject 52 ms despite scheduler proof.'
fi
sed -e 's/max_switch_ns=135344750/max_switch_ns=227000000/' \
  -e 's/max_recurring_switch_work_ns=40000000/max_recurring_switch_work_ns=45500000/' \
  -e 's/max_switch_estimated_work_ns=40000000/max_switch_estimated_work_ns=45500000/' \
  -e 's/max_recurring_switch_estimated_work_ns=40000000/max_recurring_switch_estimated_work_ns=45500000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-long-wall-switch.log"
sed '/^Cache chat switch latency regression:/s/within_budgets=1/within_budgets=0/' \
  "$temp_dir/cache-latency-long-wall-switch.log" > "$temp_dir/cache-latency-long-wall-switch-untraced-cpu-stall.log"
printf '%s\n' \
  'HARNESS ERROR: chat-switch scheduler trace unavailable: index=1 label=chat-switch wall_ns=227000000 main_thread_cpu_ns=45500000 budget_ns=50000000 runner_noise_eligible=0 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-long-wall-switch-untraced-cpu-stall.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-long-wall-switch-untraced-cpu-stall.log" >/dev/null 2>&1; then
	contract_fail 'A 227 ms chat switch with 45.5 ms untraced CPU must fail.'
fi
sed '/^Initial history selection:/s/estimated_work_ns=40000000/estimated_work_ns=52107000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-initial-selection-over-budget.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-initial-selection-over-budget.log" >/dev/null 2>&1; then
	contract_fail 'The initial history selection must reject 52 ms of main-thread work even with scheduler proof.'
fi
sed '/^Initial history selection:/ { s/blocked_estimate_ns=0/blocked_estimate_ns=-1/; s/trace=R/trace=none/; s/within_budget=1/within_budget=0/; }' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-initial-selection-cpu-only.log"
if ! grep -Fq 'within_budget=0 cpu_only_within_budget=1' \
  "$temp_dir/cache-latency-initial-selection-cpu-only.log"; then
  contract_fail 'The initial-selection gate must reject an untraced 135 ms window with 40 ms CPU.'
fi
printf '%s\n' \
  'HARNESS ERROR: chat-switch scheduler trace unavailable: index=0 label=initial-history-selection wall_ns=135344750 main_thread_cpu_ns=40000000 budget_ns=50000000 runner_noise_eligible=0 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-initial-selection-cpu-only.log"
sed '/^Cache chat switch latency regression:/s/within_budgets=1/within_budgets=0/' \
  "$temp_dir/cache-latency-initial-selection-cpu-only.log" > "$temp_dir/cache-latency-initial-selection-high-cpu.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-initial-selection-high-cpu.log" >/dev/null 2>&1; then
	contract_fail 'A 135 ms untraced initial selection with 40 ms CPU must fail.'
fi
sed '/^Initial history selection:/ { s/main_thread_work_ns=40000000/main_thread_work_ns=1138000/; s/blocked_estimate_ns=0/blocked_estimate_ns=-1/; s/estimated_work_ns=40000000/estimated_work_ns=1138000/; s/trace=R/trace=none/; s/within_budget=1/within_budget=0/; }' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-initial-selection-negligible-cpu.log"
printf '%s\n' \
  'HARNESS ERROR: chat-switch scheduler trace unavailable: index=0 label=initial-history-selection wall_ns=135344750 main_thread_cpu_ns=1138000 budget_ns=50000000 runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-initial-selection-negligible-cpu.log"
if ! check_cache_latency_evidence \
  "$temp_dir/cache-latency-initial-selection-negligible-cpu.log" \
  > "$temp_dir/cache-latency-initial-selection-negligible-cpu-output.log" 2>&1; then
	contract_fail 'One 135 ms untraced initial selection with 1.138 ms CPU must pass.'
fi
if ! grep -Fq 'untraced_wall_overrun_count=1' \
  "$temp_dir/cache-latency-initial-selection-negligible-cpu-output.log"; then
	contract_fail 'The profile step must report the single untraced overrun count.'
fi
if ! grep -Fq '::warning:: HARNESS ERROR: chat-switch scheduler trace unavailable' \
  "$temp_dir/cache-latency-initial-selection-negligible-cpu-output.log"; then
	contract_fail 'An initial-selection sample without a scheduler trace must be reported as a warning.'
fi
sed '/^Initial history selection:/ { s/main_thread_work_ns=40000000/main_thread_work_ns=52107000/; s/estimated_work_ns=40000000/estimated_work_ns=52107000/; s/cpu_only_within_budget=1/cpu_only_within_budget=0/; }' \
  "$temp_dir/cache-latency-initial-selection-high-cpu.log" > "$temp_dir/cache-latency-initial-selection-cpu-over-budget.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-initial-selection-cpu-over-budget.log" >/dev/null 2>&1; then
	contract_fail 'A 52 ms initial selection CPU stall must fail without scheduler evidence.'
fi
sed 's/max_hydration_ns=5210625/max_hydration_ns=92000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-long-wall-hydration.log"
if ! check_cache_latency_evidence "$temp_dir/cache-latency-long-wall-hydration.log" >/dev/null 2>&1; then
  contract_fail 'A long hydration wall time with bounded main-thread CPU must pass without scheduler proof.'
fi
sed 's/attach_menu_first_hover_callback_cpu_ns=12000000/attach_menu_first_hover_callback_cpu_ns=50000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-slow-first-hover.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-slow-first-hover.log" >/dev/null 2>&1; then
  contract_fail 'First-hover attach-menu callback must reject 50 ms or more.'
fi
sed 's/attach_menu_update_hover_callback_cpu_ns=1000000/attach_menu_update_hover_callback_cpu_ns=50000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-slow-update-hover.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-slow-update-hover.log" >/dev/null 2>&1; then
  contract_fail 'Updated attach-menu callback must reject 50 ms or more.'
fi
sed 's/attach_menu_uncached_peer_callback_cpu_ns=20000000/attach_menu_uncached_peer_callback_cpu_ns=50000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-slow-uncached-hover.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-slow-uncached-hover.log" >/dev/null 2>&1; then
  contract_fail 'Uncached attach-menu callback must reject 50 ms or more.'
fi
sed '/^Attach-menu callback work:/s/cached_hover_estimated_work_ns=12000000/cached_hover_estimated_work_ns=200000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-blocked-cached-hover.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-blocked-cached-hover.log" >/dev/null 2>&1; then
  contract_fail 'A blocked cached-hover callback must fail its estimated-work budget.'
fi
sed '/^Attach-menu callback work:/s/first_hover_estimated_work_ns=12000000/first_hover_estimated_work_ns=200000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-blocked-first-hover.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-blocked-first-hover.log" >/dev/null 2>&1; then
  contract_fail 'A blocked first-hover callback must fail its estimated-work budget.'
fi
sed '/^Attach-menu callback work:/s/invalidation_estimated_work_ns=1000000/invalidation_estimated_work_ns=200000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-blocked-invalidation-hover.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-blocked-invalidation-hover.log" >/dev/null 2>&1; then
  contract_fail 'A blocked invalidation callback must fail its estimated-work budget.'
fi
sed '/^Attach-menu callback work:/s/uncached_peer_estimated_work_ns=20000000/uncached_peer_estimated_work_ns=200000000/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-blocked-uncached-hover.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-blocked-uncached-hover.log" >/dev/null 2>&1; then
  contract_fail 'A blocked uncached-peer callback must fail its estimated-work budget.'
fi
sed 's/first_hover_estimated_work_ns=12000000/first_hover_estimated_work_ns=1000000/' \
  "$temp_dir/cache-latency-responsive.log" \
  > "$temp_dir/cache-latency-attach-menu-runner-pause.log"
printf '%s\n' \
  'Attach menu callback work window: label=first hover callback window_start_steady_ns=2000000000 wall_ns=60000000 main_thread_cpu_ns=1000000 blocked_estimate_ns=-1 estimated_work_ns=1000000 budget_ns=50000000 trace=none within_budget=0 cpu_only_within_budget=1 runner_noise_eligible=1 scheduler_evidence_available=0.' \
  'HARNESS ERROR: attach-menu scheduler trace unavailable: label=first hover callback window_start_steady_ns=2000000000 wall_ns=60000000 main_thread_cpu_ns=1000000 budget_ns=50000000 runner_noise_eligible=1 runner_noise_cpu_limit_ns=5000000.' \
  >> "$temp_dir/cache-latency-attach-menu-runner-pause.log"
if ! check_cache_latency_evidence \
  "$temp_dir/cache-latency-attach-menu-runner-pause.log" \
  > "$temp_dir/cache-latency-attach-menu-runner-pause-output.log" 2>&1; then
  contract_fail 'A 60 ms attach-menu callback with 1 ms CPU and no trace must pass once as runner noise.'
fi
if ! grep -Fq 'untraced_wall_overrun_count=1' \
  "$temp_dir/cache-latency-attach-menu-runner-pause-output.log"; then
  contract_fail 'An attach-menu runner pause must count as one untraced incident.'
fi
if ! grep -Fq '::warning:: HARNESS ERROR: attach-menu scheduler trace unavailable' \
  "$temp_dir/cache-latency-attach-menu-runner-pause-output.log"; then
  contract_fail 'A missing attach-menu scheduler trace must be reported as a warning.'
fi
sed 's/first_hover_estimated_work_ns=12000000/first_hover_estimated_work_ns=60000000/' \
  "$temp_dir/cache-latency-responsive.log" \
  > "$temp_dir/cache-latency-attach-menu-traced-stall.log"
printf '%s\n' \
  'Attach menu callback work window: label=first hover callback window_start_steady_ns=2000000000 wall_ns=60000000 main_thread_cpu_ns=1000000 blocked_estimate_ns=60000000 estimated_work_ns=60000000 budget_ns=50000000 trace=WWWWWWWWWWWWW within_budget=0 cpu_only_within_budget=1 runner_noise_eligible=0 scheduler_evidence_available=1.' \
  >> "$temp_dir/cache-latency-attach-menu-traced-stall.log"
if check_cache_latency_evidence \
  "$temp_dir/cache-latency-attach-menu-traced-stall.log" \
  >/dev/null 2>&1; then
  contract_fail 'A traced blocked attach-menu callback must fail its 50 ms gate.'
fi
for field in reactions=73 hydrated_documents=292 chat_switches=73; do
  sed "s/$field/${field%%=*}=0/" \
    "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-incomplete-switch.log"
  if check_cache_latency_evidence "$temp_dir/cache-latency-incomplete-switch.log" >/dev/null 2>&1; then
    contract_fail "Incomplete $field must remain a gate."
  fi
done
sed 's/minimized_restore=1/minimized_restore=0/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-unrestored.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-unrestored.log" >/dev/null 2>&1; then
  contract_fail 'The minimized window must be restored.'
fi
sed '/^First messages.getDialogs handler latency regression:/ { s/within_budget=1/within_budget=0/; s/under_50ms=1\./under_50ms=0./; }' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-bad-dialogs.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-bad-dialogs.log" >/dev/null 2>&1; then
  contract_fail 'The first getDialogs timing must remain a gate.'
fi
sed 's/root_swap_refused=1/root_swap_refused=0/' \
  "$temp_dir/cache-latency-responsive.log" > "$temp_dir/cache-latency-bad-protection.log"
if check_cache_latency_evidence "$temp_dir/cache-latency-bad-protection.log" >/dev/null 2>&1; then
  contract_fail 'The cache root-swap protection must remain a gate.'
fi
for cache_evidence_field in \
  queued_ops_before_ready \
  queued_ops_passed \
  cache_usable_ns \
  under_10s \
  startup_heartbeat_gap_count \
  startup_uncovered_heartbeat_gaps \
  startup_heartbeat_gap_coverage_proven; do
  grep -Fq "$cache_evidence_field" "$profile_harness" \
    || contract_fail "$profile_harness must match cache evidence field $cache_evidence_field."
done
grep -Fq 'run_mac_lifecycle_cases.sh' "$profile_harness" \
  || contract_fail "$profile_harness must use the case runner."
for case_name in \
  font-startup \
  profile-ipc \
  spoiler-cache-emoji \
  spoiler-cache-spoiler \
  spoiler-cache-text \
  spoiler-cache-image \
  authenticated-lifecycle \
  authenticated-cache \
  native-failure-continuation \
  folder-fault-barrier-timeout \
  folder-fault-cross-session-presence; do
  grep -Fq "$case_name" "$profile_harness" \
    || contract_fail "$profile_harness must include lifecycle case $case_name."
done
all_first_command="$(awk '
  /^all\)$/ { in_all = 1; next }
  in_all {
    line = $0
    sub(/^[[:space:]]+/, "", line)
    if (line == "" || line ~ /^#/) next
    print line
    exit
  }
' "$profile_harness")"
[[ "$all_first_command" == 'run_font_startup_regression' ]] \
  || contract_fail "$profile_harness must run the font-startup regression before any all-case app launches."
application_source='Telegram/SourceFiles/core/application.cpp'
if ! python3 - <<'PY'
from pathlib import Path

source = Path('Telegram/SourceFiles/core/application.cpp').read_text()
hold_selector = source.index('qEnvironmentVariable("TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE")')
hold_branch = source.index('if (!holdFile.isEmpty())', hold_selector)
shutdown_selector = source.index('"TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION"')
player = source.index('Media::Player::start(_audio.get());', hold_selector)
shutdown = source.index('RunEmojiCacheShutdownRegression', shutdown_selector)
if not (
    hold_selector < player < hold_branch
    and shutdown_selector < player < shutdown
):
    raise SystemExit('both headless relaunch fixtures must initialize Media::Player before waiting')
PY
then
	contract_fail 'Both headless relaunch fixtures must initialize the media player before their event-loop waits.'
fi
emoji_init_line="$(grep -n -F 'Ui::Emoji::Init();' "$application_source" | head -n 1 | cut -d: -f1)"
font_fallback_exit_line="$(grep -n -F \
	'QCoreApplication::exit(RunFontFallbackRegression());' "$application_source" \
	| head -n 1 | cut -d: -f1)"
[[ "$emoji_init_line" =~ ^[0-9]+$ \
	&& "$font_fallback_exit_line" =~ ^[0-9]+$ \
	&& "$emoji_init_line" -lt "$font_fallback_exit_line" ]] \
	|| contract_fail 'The font fallback exit must remain after Emoji initialization.'
if ! awk '
  /^authenticated-cache\)$/ { in_case = 1; next }
  /^authenticated-lifecycle\)$/ { in_case = 0 }
  in_case && /run_font_startup_regression/ { font = NR }
  in_case && /run_authenticated_cache_regression/ { cache = NR }
  END { exit !(font && cache && font < cache) }
' "$profile_harness"; then
  contract_fail "$profile_harness authenticated-cache case must run font-startup before its app launch."
fi
if ! awk '
  /^void Sandbox::launchApplication\(\) \{/ { in_launch = 1 }
  in_launch && /std::make_unique<UiStallWatchdog>/ { watchdog = NR }
  in_launch && /if \(!_fontStartupWindow\)/ { splash = NR }
  in_launch && /^void Sandbox::startFontDatabaseWarmup/ { in_launch = 0 }
  END { exit !(watchdog && splash && watchdog < splash) }
' Telegram/SourceFiles/core/sandbox.cpp; then
  contract_fail 'The UI stall watchdog must start before the font splash.'
fi
if ! awk '
  /^void Sandbox::startFontDatabaseWarmup\(\)/ { in_warmup = 1 }
  in_warmup && /prewarmFontMetrics\(QFont\(\)\.family\(\)\)/ { system_font = NR }
  in_warmup && /prewarmFontMetrics\(u"Open Sans"_q\)/ { bundled_font = NR }
  in_warmup && /QFontMetricsF\(font\)/ { metrics = NR }
  in_warmup && /QMetaObject::invokeMethod\(/ { callback = NR }
  in_warmup && /^void Sandbox::setupScreenScale\(\)/ { in_warmup = 0 }
  END { exit !(system_font && bundled_font && metrics && callback && system_font < callback && bundled_font < callback && metrics < callback) }
' Telegram/SourceFiles/core/sandbox.cpp; then
  contract_fail 'System and bundled font metrics must be prewarmed before application startup.'
fi
grep -Fq 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz' \
  Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Font glyph warmup must cover upper and lower case letters used by first paint.'
grep -Fq '0123456789 !' \
  Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Font glyph warmup must cover digits and punctuation used by first paint.'
grep -Fq '12, 13, 14, 16, 18, 20,' \
  Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Font glyph warmup must cover the UI text sizes used by first paint.'
grep -Fq 'std::array{QFont::Normal, QFont::DemiBold}' \
  Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Font glyph warmup must cover regular and emphasized UI text.'
if ! awk '
  /^int Sandbox::start\(\)/ { in_start = 1 }
  in_start && /const auto drainShutdown = \[this\]/ { helper = NR }
  in_start && /_shutdownLoop\.run\(pending\)/ { shutdown_loop = NR }
  in_start && /const auto result = exec\(\);/ { exec_line = NR }
  in_start && !joinable_line && /_fontDatabaseWarmupThread\.joinable\(\)/ { joinable_line = NR }
  in_start && /!_fontDatabaseWarmupDone/ { done_line = NR }
  in_start && /drainShutdown\(\);/ { drain_line = NR }
  in_start && /_fontDatabaseWarmupThread\.join\(\)/ { join_line = NR }
  in_start && /^int Sandbox::stopRunningInstance/ { in_start = 0 }
  END { exit !(helper && shutdown_loop && joinable_line && done_line && exec_line && drain_line && join_line && helper < shutdown_loop && joinable_line < done_line && exec_line < drain_line && drain_line < join_line) }
' Telegram/SourceFiles/core/sandbox.cpp; then
  contract_fail 'The GUI shutdown loop must drain font warmup work before joining its worker.'
fi
grep -Fq 'run_font_shutdown_regression shutdown-remove-pending' \
  Telegram/build/test_mac_profile_guard.sh \
  || contract_fail 'The macOS font shutdown regression must force a pending removal across application quit.'
grep -Fq 'qt_font_database_hold=1' Telegram/build/test_mac_profile_guard.sh \
	|| contract_fail 'The macOS font shutdown regression must assert its Qt database hold separately from CoreText enumeration.'
if ! python3 - <<'PY'
import re
from pathlib import Path

cmake = Path('Telegram/CMakeLists.txt').read_text()
probe_target = re.search(
    r'if \(APPLE AND TDESKTOP_TEAGRAM AND '
    r'TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST\)(.*?)\nendif\(\)',
    cmake,
    re.DOTALL,
)
if (
    not probe_target
    or 'add_library(TeagramFontShutdownProbe SHARED' not in probe_target.group(1)
):
    raise SystemExit('font probe must be built as a test-only dylib')
if (
    'target_link_libraries(Telegram PRIVATE TeagramFontShutdownProbe)'
    not in probe_target.group(1)
):
    raise SystemExit('Teagram must load the test probe dylib at launch')
apple_sources = re.search(
    r'if \(APPLE\)\s+nice_target_sources\(Telegram.*?\n\s*\)\nelse\(\)',
    cmake,
    re.DOTALL,
)
if not apple_sources or 'tests/font_shutdown_probe.mm' in apple_sources.group():
    raise SystemExit('font probe must not be compiled into the executable')
PY
then
  contract_fail 'The macOS integration probe must be a test-only dylib loaded at launch.'
fi
grep -Fq 'Font shutdown regression: probe_interposer=active' \
  Telegram/SourceFiles/tests/font_shutdown_probe.mm \
  || contract_fail 'The native font probe must report that dyld ran its interposer.'
if ! python3 - <<'PY'
from pathlib import Path

source = Path('Telegram/SourceFiles/tests/font_shutdown_probe.mm').read_text()
probe = source.split('Teagram_FontFamilyPopulationProbe() {', 1)[1]
eligibility = probe.index('if (!ProbeArmed.load')
skipped = probe.index('if (skipped > 0)')
marker = probe.index('"Font shutdown regression: probe_interposer=active\\n"')
first_call = probe.index('auto result = original();')
if not eligibility < skipped < marker < first_call:
    raise SystemExit('interposer marker must follow eligibility and precede the first native call')
PY
then
  contract_fail 'The native font probe must report activation before its first CoreText call.'
fi
grep -Fq 'Font shutdown regression: probe_interposer=active' \
  Telegram/build/test_mac_profile_guard.sh \
  || contract_fail 'The native fixture must require the dyld interposer activation marker.'
grep -Fq 'ArmFontShutdownProbe' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'The shutdown worker must arm the synchronized Qt CoreText probe.'
grep -Fq 'ReleaseFontShutdownProbe' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Application quit must release the synchronized Qt CoreText probe.'
if ! python3 - <<'PY'
from pathlib import Path

source = Path('Telegram/SourceFiles/core/sandbox.cpp').read_text()
warmup = source.split('void Sandbox::startFontDatabaseWarmup()', 1)[1]
blocks = (
    ('aboutToQuit',
     'connect(this, &QCoreApplication::aboutToQuit, [=] {',
     '\n\t\t});'),
    ('stopWorker',
     'const auto stopWorker = [=](ProbeExit exitMode) {',
     '\n\t\t\t};'),
)
for name, start, end in blocks:
    block = warmup.split(start, 1)[1].split(end, 1)[0]
    release = block.index('ReleaseFontShutdownProbe')
    final_release = block.index('ProbeFinalRelease.store')
    if release > final_release:
        raise SystemExit(f'{name} must release the probe before waking its worker')
PY
then
  contract_fail 'The font shutdown release must be visible before waking the pre-arm worker.'
fi
grep -Fq 'timer->start(0);' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'The repeated real CoreText population must be sampled without the old millisecond gap.'
grep -Fq 'IsFontShutdownProbeReleased' Telegram/SourceFiles/tests/font_shutdown_probe.h \
  || contract_fail 'The cancellation-before-arm regression must inspect the preserved release state.'
if sed -n '/void ArmFontShutdownProbe(/,/^}/p' \
    Telegram/SourceFiles/tests/font_shutdown_probe.mm \
    | grep -Fq 'ProbeReleased.store(false'; then
  contract_fail 'Arming the font shutdown probe must preserve an early release.'
fi
grep -Fq 'run_font_shutdown_regression probe-release-before-arm' \
  Telegram/build/test_mac_profile_guard.sh \
  || contract_fail 'The macOS shutdown regression must cover release before probe arm.'
grep -Fq 'IgnoreNextApplicationActivation' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Start-in-tray must suppress its delayed initial activation.'
grep -Fq 'QTimer::singleShot(500' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'The original 500 ms start-in-tray checkpoint must remain.'
grep -Fq 'delayed_activation_check' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'The start-in-tray test must retain a later delayed-activation check.'
grep -Fq 'applicationDidBecomeActive' Telegram/SourceFiles/platform/mac/specific_mac_p.mm \
  || contract_fail 'The delayed start-in-tray activation guard must stay on the AppKit activation path.'
grep -Fq 'kIgnoreNextActivationTimeoutMs = 1500' \
  Telegram/SourceFiles/platform/mac/specific_mac_p.mm \
  || contract_fail 'The initial activation suppression must expire after startup.'
if ! awk '
  /if \(!RemoveStaleLocalServer\(_localServerName\)\)/ { in_remove_failure = 1 }
  in_remove_failure && /return showSingleInstanceError\(diagnostic\);/ { error_window = 1 }
  in_remove_failure && /^[[:space:]]*}/ { in_remove_failure = 0 }
  END { exit !error_window }
' Telegram/SourceFiles/core/sandbox.cpp; then
  contract_fail 'A conflicting single-instance path must be reported in the failure window.'
fi
grep -Fq 'Could not use the single-instance IPC path' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'The IPC collision diagnostic must identify the path in use.'
grep -Fq "'ipc-collision'" .github/scripts/test_macos_socket_error.py \
  || contract_fail 'The macOS socket regression must cover a non-socket IPC path.'
grep -Fq 'entry_preserved' .github/scripts/test_macos_socket_error.py \
  || contract_fail 'The IPC collision regression must verify that the non-socket entry remains intact.'
grep -Fq "r'([^\\n]+)'" .github/scripts/test_macos_shutdown_relaunch.py \
  || contract_fail 'The relaunch recheck fixture must capture the socket path up to its newline.'
grep -Fq 'void retrySingleInstance(bool immediate = false);' \
  Telegram/SourceFiles/core/sandbox.h \
  || contract_fail 'Single-instance retries must keep their default backoff.'
grep -Fq 'result == Core::SingleInstanceLock::Result::Acquired);' \
  Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Only a successful profile-lock acquisition may retry immediately.'
grep -Fq 'const auto delay = immediate ? 0 : 50;' \
  Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Only successful acquisition retries may skip the 50 ms backoff.'
grep -Fq 'QTimer::singleShot(delay, this, [=] {' \
  Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'The selected single-instance retry delay must reach the timer.'
grep -Fq 'return retrySingleInstance();' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Unexpected socket errors must retain the default retry backoff.'
grep -Fq 'retrySingleInstance();' Telegram/SourceFiles/core/sandbox.cpp \
  || contract_fail 'Socket disconnects must retain the default retry backoff.'
grep -Fq 'Single instance retry regression: delay_ms=0' \
  .github/scripts/test_macos_shutdown_relaunch.py \
  || contract_fail 'The lock-acquire recheck must assert its immediate retry.'
grep -Fq 'Single instance retry regression: delay_ms=50' \
  .github/scripts/test_macos_socket_error.py \
  || contract_fail 'Socket retry regressions must assert their 50 ms backoff.'
if ! awk '
  /const auto stopWorker = \[=\]\(ProbeExit exitMode\)/ { in_stop_worker = 1 }
  in_stop_worker && /ReleaseFontShutdownProbe/ { release = 1 }
  in_stop_worker && /^\t\t\t};/ { exit !release }
  END { exit !release }
' Telegram/SourceFiles/core/sandbox.cpp; then
  contract_fail 'The timeout path must release a held CoreText probe before draining the worker.'
fi
grep -Fq 'return &CTFontManagerCopyAvailableFontFamilyNames;' \
  Telegram/SourceFiles/tests/font_shutdown_probe.mm \
  || contract_fail 'The shutdown probe must use dyld’s original CoreText binding.'
if grep -Fq 'RTLD_NEXT' Telegram/SourceFiles/tests/font_shutdown_probe.mm; then
  contract_fail 'The shutdown probe must not resolve its interposed CoreText symbol through RTLD_NEXT.'
fi
grep -Fq 'auto result = original();' \
  Telegram/SourceFiles/tests/font_shutdown_probe.mm \
	|| contract_fail 'The shutdown probe must keep making real CoreText queries inside Qt population.'
grep -Fq 'IsQtFontDatabasePopulation()' \
  Telegram/SourceFiles/tests/font_shutdown_probe.mm \
  || contract_fail 'The shutdown probe must only hold the Qt font-family population call.'
grep -Fq 'while (!ProbeReleased.load' \
  Telegram/SourceFiles/tests/font_shutdown_probe.mm \
	|| contract_fail 'The repeated CoreText queries must stop when shutdown releases the Qt population hold.'
grep -Fq 'Teagram_FontFamilyPopulationProbe()' \
	Telegram/SourceFiles/tests/font_shutdown_probe.mm \
	|| contract_fail 'The interposer must have a symbol that cannot masquerade as CoreText.'
if grep -Fq 'Teagram_CTFontManagerCopyAvailableFontFamilyNames' \
	Telegram/SourceFiles/tests/font_shutdown_probe.mm; then
	contract_fail 'The interposer symbol must not satisfy the real CoreText stack matcher.'
fi
if ! awk '
  /const auto sampled/ { in_sample = 1 }
  in_sample && /if \(!sampled/ { unmatched = NR }
  in_sample && /qt_font_database_hold=1/ { hold = NR }
  in_sample && /Core::Quit\(\);/ { quit = NR; exit !(unmatched && hold && unmatched < hold && hold < quit) }
  END { if (!quit) exit 1 }
' Telegram/SourceFiles/core/sandbox.cpp; then
	contract_fail 'The Qt hold marker must follow a matched Qt/CoreText stack sample before quit.'
fi
if grep -Fq 'qt_font_database_hold=1' \
	Telegram/SourceFiles/tests/font_shutdown_probe.mm; then
	contract_fail 'The interposer must not claim a Qt hold before the real stack sample.'
fi
if grep -Fq 'CTFontManagerCopyAvailableFontFamilyNames();' \
  Telegram/SourceFiles/core/sandbox.cpp; then
  contract_fail 'The shutdown probe must not bypass Qt font-database population with a direct CoreText query.'
fi
if ! awk '
  /bool FramesInSystemEnumeration\(/ { in_probe = 1 }
  in_probe && /name\.contains\("CTFontManagerCopyAvailableFontFamilyNames"\)/ { core_text = 1 }
  in_probe && /name\.contains\("QFontDatabase"\)/ { qt_database = 1 }
  in_probe && /name\.contains\("families"\)/ { qt_families = 1 }
  in_probe && /name\.contains\("QCoreTextFontDatabase"\)/ { qt_coretext = 1 }
  in_probe && /name\.contains\("populateFromDescriptor"\)/ { qt_descriptor = 1 }
  in_probe && /name\.contains\("populateFontDatabase"\)/ { qt_population = 1 }
  in_probe && /name\.contains\("QFontDatabasePrivate"\)/ { qt_private = 1 }
  in_probe && /name\.contains\("ensureFontDatabase"\)/ { qt_ensure = 1 }
  in_probe && /qtFamilyList \|\| qtCoreTextPopulation \|\| qtDatabasePopulation/ { qt_paths = 1 }
  in_probe && /return coreTextEnumeration && qtFontDatabasePopulation/ { paired = 1 }
  in_probe && /^}/ { exit !(core_text && qt_database && qt_families && qt_coretext && (qt_descriptor || qt_population) && qt_private && qt_ensure && qt_paths && paired) }
' Telegram/SourceFiles/core/sandbox.cpp; then
  contract_fail 'The shutdown probe must require a real CoreText family enumeration frame inside Qt font-database population.'
fi
if ! python3 - <<'PY'
from pathlib import Path

probe = Path('Telegram/SourceFiles/tests/font_shutdown_probe.mm').read_text()
function = probe.split('Teagram_FontFamilyPopulationProbe() {', 1)[1]
first_call = function.index('auto result = original();')
ready = function.index('ProbeInterposerReady.store(true, std::memory_order_release);')
repeat = function.index('while (!ProbeReleased.load')
if not first_call < ready < repeat:
    raise SystemExit('the interposer must signal readiness after the first real CoreText query')

header = Path('Telegram/SourceFiles/tests/font_shutdown_probe.h').read_text()
if 'IsFontShutdownProbeReady' not in header:
    raise SystemExit('the GUI must be able to observe the interposer readiness signal')

warmup = Path('Telegram/SourceFiles/core/sandbox.cpp').read_text().split(
    'void Sandbox::startFontDatabaseWarmup()', 1
)[1]
callback = warmup.split('connect(timer, &QTimer::timeout, this, [=] {', 1)[1]
ready_check = callback.index('Core::Tests::IsFontShutdownProbeReady()')
sample = callback.index('SampleWorkerStack(')
if ready_check > sample or 'testFailure != "probe-timeout"' not in callback[ready_check - 80:ready_check]:
    raise SystemExit('sampling must wait for interposer readiness except in the forced timeout case')
PY
then
	contract_fail 'The shutdown sampler must wait for the interposer before requiring a real CoreText frame.'
fi
if ! awk '
  /^run_font_startup_regression\(\) \{$/ { in_startup = 1; next }
  in_startup && /^}$/ { in_startup = 0 }
  in_startup && /run_font_shutdown_regression;/ { enumeration = NR }
  in_startup && /run_font_shutdown_regression shutdown-remove-pending/ { removal = NR }
  END { exit !(enumeration && removal && enumeration < removal) }
' Telegram/build/test_mac_profile_guard.sh; then
  contract_fail 'The startup regression must retain the baseline quit-during-enumeration case before the pending-removal case.'
fi
grep -Fq 'remove_pending_at_quit=1 removal_completed=1' \
  Telegram/build/test_mac_profile_guard.sh \
  || contract_fail 'The macOS font shutdown regression must assert that the pending removal completed.'
grep -Fq 'const auto startupWork = EstimateStartupWork(' \
  Telegram/SourceFiles/tests/account_lifecycle_regression.cpp \
  || contract_fail 'Startup work estimates must use complete matched stall samples when ping captures are pending.'
if grep -Eq 'QEventLoop' Telegram/SourceFiles/core/sandbox.cpp \
  || grep -Eq 'ResolveDeferredShapedMetrics|SetShapedMetricsDeferred' \
    Telegram/SourceFiles/core/application.cpp; then
  contract_fail 'Startup must continue from the warmup completion callback, not a nested event loop.'
fi
grep -Fq 'bash "$PROFILE_GUARD" "$FAKE_APP"' \
  Telegram/build/test_mac_lifecycle_case_runner.sh \
  || contract_fail 'The lifecycle runner self-test must invoke the profile harness without a case argument.'
native_harness='Telegram/SourceFiles/tests/account_lifecycle_regression.cpp'
history_widget='Telegram/SourceFiles/history/history_widget.cpp'
attach_menu_builder='Telegram/SourceFiles/inline_bots/bot_attach_web_view.cpp'
awk '
  /RunCacheChatSwitchLatencyRegression\(/ { copy = 1 }
  copy && /^struct FirstDialogsHandlerRegressionResult/ { exit }
  copy { print }
' "$native_harness" > "$temp_dir/cache-chat-switch-regression.cpp"
grep -Fq 'attachMenuHoverNanoseconds' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'QEvent::Enter' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'setAttachBotsForRegressionTest' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'showPeerHistoryMeasured(attachHistory)' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'maxChatSwitchIndex' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'maxChatSwitchPeerId' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'RecentSchedulerSamplesForTesting' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'SchedulerTraceForInterval' "$temp_dir/cache-chat-switch-regression.cpp" \
  || contract_fail 'The cache regression must measure attach-menu hover and identify the slowest peer switch.'
grep -Fq 'requestsBarDeferredForEmptyPeer' "$temp_dir/cache-chat-switch-regression.cpp" \
  && grep -Fq 'requestsBarCreatedAfterPendingUpdate' "$temp_dir/cache-chat-switch-regression.cpp" \
  || contract_fail 'The cache regression must verify lazy requests-bar creation and its pending-count update.'
grep -Fq 'schedulerTraceComplete = true' Telegram/SourceFiles/core/ui_stall_watchdog.cpp \
	&& grep -Fq 'schedulerSampleOffsetsNanoseconds' Telegram/SourceFiles/core/ui_stall_watchdog.h \
  || contract_fail 'The watchdog must retain completed per-ack scheduler samples for chat-switch windows.'
grep -Fq 'processWorkNanoseconds' "$native_harness" \
  && grep -Fq 'kMaxStartupHeartbeatProcessWorkNanoseconds' "$native_harness" \
  && grep -Fq 'mainThreadWorkNanoseconds' "$native_harness" \
  && grep -Fq 'matchedEvidenceStarts' "$native_harness" \
  && grep -Fq 'startupHeartbeatGaps.push_back' "$native_harness" \
  && grep -Fq 'RecentPingAcknowledgementsForTesting' "$native_harness" \
  && grep -Fq 'HeartbeatGapHasSuccessfulWatchdogCoverage' "$native_harness" \
  && grep -Fq 'CountUncoveredStartupHeartbeatGaps' "$native_harness" \
  && grep -Fq 'unobservedAppWait' "$native_harness" \
  && grep -Fq 'corroboratedStartupStalls' "$native_harness" \
  && grep -Fq 'AllMainThreadWorkIntervalsHaveSchedulerProof' "$native_harness" \
  && grep -Fq 'StartupWatchdogStallEvidenceRegressionPasses' "$native_harness" \
  && grep -Fq 'Cache startup watchdog stall:' "$native_harness" \
  && grep -Fq 'startup_stall_scheduler_preemption_proven' "$native_harness" \
  || contract_fail 'The cache regression must retain startup stall evidence as diagnostics.'
awk '
  /for \(auto i = 0; i != 10; \+\+i\)/ { copy = 1 }
  copy && /result.attachMenuCacheBounded/ { exit }
  copy { print }
' "$native_harness" > "$temp_dir/uncached-peer-switches.cpp"
grep -Fq 'showPeerHistoryMeasured(history);' \
  "$temp_dir/uncached-peer-switches.cpp" \
  && grep -Fq 'peerBuildCallbackCpuNanoseconds < 0' \
    "$temp_dir/uncached-peer-switches.cpp" \
  && grep -Fq 'result.attachMenuUncachedPeerCallbackCpuNanoseconds = -1;' \
    "$temp_dir/uncached-peer-switches.cpp" \
  || contract_fail 'Uncached peer switches must measure work and fail the CPU aggregate on any unavailable sample.'
grep -Fq 'attachMenuFirstHoverNanoseconds' "$native_harness" \
  && grep -Fq 'attachMenuFirstHoverCallbackCpuNanoseconds' "$native_harness" \
  && grep -Fq 'attachMenuInvalidationCallbackCpuNanoseconds' "$native_harness" \
  && grep -Fq 'attachMenuUncachedPeerCallbackCpuNanoseconds' "$native_harness" \
  && grep -Fq 'kAttachMenuRegressionBotCount = 20' "$native_harness" \
  && grep -Fq 'attachMenuBuildDeferredUntilHover' "$native_harness" \
  && grep -Fq 'buildDeferredUntilEnter' "$native_harness" \
  && grep -Fq 'lastAttachBotId' "$native_harness" \
  && grep -Fq 'attachMenuInvalidationRebuilt' "$native_harness" \
  && grep -Fq 'attachMenuContainsBotForRegressionTest' "$native_harness" \
  && grep -Fq 'attachMenuCacheSizeForRegressionTest() <= 8' "$native_harness" \
  || contract_fail 'The attach-menu regression must defer and gate first hover, uncached peers, bot invalidation, and bounded caching.'
grep -Fq 'ThreadCpuTimeNanosecondsForRegressionTest' "$history_widget" \
  && grep -Fq 'ThreadCpuTimeNanosecondsForRegressionTest' "$attach_menu_builder" \
  && grep -Fq 'ProcessCpuTimeNanosecondsForRegressionTest' "$history_widget" \
  && grep -Fq 'ProcessCpuTimeNanosecondsForRegressionTest' "$attach_menu_builder" \
  || contract_fail 'Attach-menu construction callbacks must be measured with the main-thread CPU clock.'
grep -Fq 'MainThreadCpuWorkWithinBudget' "$native_harness" \
  && grep -Fq 'MainThreadEstimatedWorkWithinBudget' "$native_harness" \
  && grep -Fq 'AccumulateMainThreadWorkEstimate' "$native_harness" \
  && grep -Fq 'MainThreadBlockedWorkBudgetRegressionPasses' "$native_harness" \
  && grep -Fq 'EstimateMainThreadWork' "$native_harness" \
  && grep -Fq 'for (const auto &callback : attachMenuCallbacks)' "$native_harness" \
  && grep -Fq 'Attach-menu callback work:' "$native_harness" \
  && ! grep -Fq 'ChatSwitchSchedulerProofRegressionPasses' "$native_harness" \
  && grep -Fq 'attachMenuFirstHoverCallbackWallNanoseconds' "$native_harness" \
  && grep -Fq 'attachMenuUncachedPeerCallbackWallNanoseconds' "$native_harness" \
  && grep -Fq 'slowAttachMenuCallbacks.push_back' "$native_harness" \
  && grep -Fq 'attachMenuHoverEstimatedWorkNanoseconds' "$native_harness" \
  && grep -Fq 'premiumBuildCallbackCpuNanoseconds' "$native_harness" \
  || contract_fail 'Every attach-menu callback must use the CPU-plus-blocked estimate with wall times as diagnostics.'
awk '
  /for \(const auto &callback : attachMenuCallbacks\)/ { copy = 1 }
  copy && /result.schedulerEvidenceSkippedRecords/ { exit }
  copy { print }
' "$native_harness" > "$temp_dir/attach-menu-callback-work.cpp"
grep -Fq 'timingBudgetRun.Observe(' "$temp_dir/attach-menu-callback-work.cpp" \
  && grep -Fq 'MainThreadWorkBudgetIsUntracedWallOverrun(' \
    "$temp_dir/attach-menu-callback-work.cpp" \
  && grep -Fq 'callback.startedAtSteadyNanoseconds' \
    "$temp_dir/attach-menu-callback-work.cpp" \
  && grep -Fq 'budgetAssessment.workNanoseconds' \
    "$temp_dir/attach-menu-callback-work.cpp" \
  && grep -Fq 'HARNESS ERROR: attach-menu scheduler trace unavailable:' \
    "$temp_dir/attach-menu-callback-work.cpp" \
  || contract_fail 'Attach-menu callbacks must use the shared incident verdict and report missing trace evidence.'
grep -Fq 'const auto firstAccessWithinBudget = timingBudgetRun.Observe(' \
  "$native_harness" \
  && grep -Fq 'MainThreadCpuWorkWithinBudget(' "$native_harness" \
  && grep -Fq 'firstAccessMainThreadWorkNanoseconds,' "$native_harness" \
  && grep -Fq 'firstAccessNanoseconds < kMaxChatSwitchNanoseconds,' "$native_harness" \
  || contract_fail 'First-access evidence must report its strict gate, CPU-only assessment, and wall threshold separately.'
grep -Fq 'RunFirstDialogsHandlerLatencyRegression(' "$native_harness" \
  && grep -Fq 'timingBudgetRun.Observe(' "$native_harness" \
  && grep -Fq 'HARNESS ERROR: getDialogs scheduler trace unavailable:' "$native_harness" \
  || contract_fail 'The first getDialogs handler must use the shared timing verdict and report missing scheduler coverage.'
grep -Fq 'handlerCpuOnlyWithinBudget = MainThreadCpuWorkWithinBudget(' \
  "$native_harness" \
  && grep -Fq 'chatSwitchWindows[i].mainThreadWorkNanoseconds,' "$native_harness" \
  && grep -Fq 'gap.mainThreadWorkNanoseconds, 100' "$native_harness" \
  && grep -Fq '.initialHistorySelectionMainThreadWorkNanoseconds,' "$native_harness" \
  || contract_fail 'CPU-only timing fields must be calculated from measured CPU, not scheduler estimates.'
grep -Fq 'slowHydrationCallbacks.push_back' "$native_harness" \
  && grep -Fq 'hydrationSchedulerPreemptionProven' "$native_harness" \
  && grep -Fq 'maxHydrationMainThreadWorkNanoseconds' "$native_harness" \
  && grep -Fq 'MainThreadCpuWorkWithinBudget' "$native_harness" \
  || contract_fail 'Reaction hydration must be bounded by main-thread CPU.'
if grep -E -q 'attach_menu_callback_latency_is_allowed|cache_stall_is_runner_scheduling' "$profile_harness"; then
  contract_fail 'The profile guard must not use wall time or scheduler proof as pass conditions.'
fi
for work_budget_field in \
  max_startup_estimated_work_ns \
  initial_history_selection_work_ns \
  max_hydration_work_ns \
  max_switch_work_ns \
  max_recurring_switch_work_ns \
  max_switch_estimated_work_ns \
  max_recurring_switch_estimated_work_ns \
  attach_menu_first_hover_callback_cpu_ns \
  attach_menu_update_hover_callback_cpu_ns \
  attach_menu_uncached_peer_callback_cpu_ns \
  cached_hover_estimated_work_ns \
  first_hover_estimated_work_ns \
  invalidation_estimated_work_ns \
  uncached_peer_estimated_work_ns; do
  grep -Fq "$work_budget_field" "$profile_harness" \
    || contract_fail "The profile guard must gate $work_budget_field."
done
if [[ $(grep -c 'work_value_within_budget' "$profile_harness") -lt 4 ]]; then
  contract_fail 'The profile guard must apply the estimated-work budget to startup and chat-switch gates.'
fi
awk '
  /void HistoryWidget::activateCachedAttachBotsMenuForCurrentPeer\(/ { copy = 1 }
  copy && /^void HistoryWidget::touchAttachBotsMenuCache\(/ { exit }
  copy { print }
' Telegram/SourceFiles/history/history_widget.cpp > "$temp_dir/activate-attach-menu.cpp"
if grep -Fq 'refreshAttachBotsMenu();' "$temp_dir/activate-attach-menu.cpp"; then
  contract_fail 'Switching to an uncached peer must not build its attach menu before hover or click.'
fi
grep -Fq 'std::vector<AttachWebViewBot> bots' \
  Telegram/SourceFiles/inline_bots/bot_attach_web_view.cpp \
  && grep -Fq 'menuBots.push_back(bot)' \
    Telegram/SourceFiles/inline_bots/bot_attach_web_view.cpp \
  || contract_fail 'Deferred attach-menu rows must own bot records across updates.'
mac_platform='Telegram/SourceFiles/platform/mac/specific_mac.mm'
grep -Fq "kMaxMinimizedRestoreNanoseconds = qint64(50'000'000)" \
  "$native_harness" \
  || contract_fail 'The minimized restore regression must retain its 50 ms budget.'
grep -Fq 'showPeerHistoryMeasured(firstHistory)' "$native_harness" \
	&& grep -Fq 'result.minimizedSwitchNanoseconds' "$native_harness" \
  || contract_fail 'The minimized restore and chat switches must use the measured switch path.'

awk '
  /A separate test window has just been created/ { copy = 1 }
  copy && /historyWidget->clearFieldFocusForRegressionTest\(\);/ { exit }
  copy { print }
' "$native_harness" > "$temp_dir/cache-first-selection.cpp"
grep -Fq 'showPeerHistoryMeasured(emptyFirstHistory)' "$temp_dir/cache-first-selection.cpp" \
	&& grep -Fq 'showPeerHistoryMeasured(target, true)' "$temp_dir/cache-first-selection.cpp" \
	&& grep -Fq 'constexpr auto kWarmupChatSwitches = 8;' "$temp_dir/cache-first-selection.cpp" \
	&& grep -Fq 'showPeerHistoryMeasured(target)' "$temp_dir/cache-first-selection.cpp" \
	|| contract_fail 'The cold selection, measured startup priming, and all eight measured warm-up switches must remain in the fixture.'
grep -Fq 'CacheHistoryWorkBudgetsForRegression' "$native_harness" \
	&& grep -Fq 'initialHistoryStartupBudgets.startupNanoseconds == 0' "$native_harness" \
	&& grep -Fq 'startupHistoryBudgets.startupNanoseconds' "$native_harness" \
	&& grep -Fq 'injectedStartupWorkWithinBudget' "$native_harness" \
	&& grep -Fq '!injectedStartupWorkOverBudget' "$native_harness" \
	&& grep -Fq 'startupTimingBudgetRun.Observe(' "$native_harness" \
	&& grep -Fq 'startupTimingBudgetRun.WithinBudget()' "$native_harness" \
	&& grep -Fq 'result.initialHistorySelectionWithinBudget' "$native_harness" \
	|| contract_fail 'The cold-selection mapping and measured startup budget must be covered by the native self-test.'
grep -Fq "kMaxChatSwitchNanoseconds = qint64(50'000'000)" "$native_harness" \
	&& grep -Fq "kMaxStartupHeartbeatThreadWorkNanoseconds = qint64(100'000'000)" "$native_harness" \
	|| contract_fail 'The 50 ms chat-switch and 100 ms startup budgets must remain unchanged.'
grep -Fq 'injectedChatSwitchStallAssessment' "$native_harness" \
	&& grep -Fq '!injectedChatSwitchStallWithinBudget' "$native_harness" \
	&& grep -Fq '!injectedChatSwitchStallRun.WithinBudget()' "$native_harness" \
	|| contract_fail 'An injected main-thread stall above the 50 ms chat-switch budget must still fail.'
awk '
  /void Sandbox::startFontDatabaseWarmup\(\)/ { copy = 1 }
  copy && /_fontDatabaseWarmupThread = std::thread/ { print; exit }
  copy { print }
' Telegram/SourceFiles/core/sandbox.cpp > "$temp_dir/font-database-warmup.cpp"
grep -Fq 'style::internal::PrepareFontSubstitutions();' "$temp_dir/font-database-warmup.cpp" \
	|| contract_fail 'Font substitutions must be installed before the worker prewarms the font database.'
grep -Fq 'painter.drawText(QPoint(0, 24 + row * 32), sample);' Telegram/SourceFiles/core/sandbox.cpp \
	|| contract_fail 'The font worker must warm glyph rasterization before the first history paint.'
grep -Fq '&& (mediaCache || chatSwitch.passed)' "$native_harness" \
  || contract_fail 'The chat switch completion and restore checks must remain a gate.'
grep -Fq 'completed && !timedOut' "$native_harness" \
  || contract_fail 'The chat switch regression must require completion without a timeout.'
for native_case in \
  settings-search \
  session-window-lifecycle \
  translation-controls \
  manage-controls \
  mini-app-open \
  settings-deep-link \
  messages-privacy \
  paid-link \
  boost-story-menus \
  folder-crud \
  stock-basic-group-creator \
  pinned-basic-group-creator \
  authenticated-cache-file-symlinks \
  failed-init-cache-access \
  failed-init-media-cache-access \
  cache-stats-initialization \
  media-cache-stats-initialization \
  cold-post-open-cache \
  cold-post-open-media-cache \
  cache-access-no-walk \
  media-cache-access-no-walk \
  post-open-cache \
  post-open-media-cache; do
  grep -Fq "\"$native_case\"" "$native_harness" \
    || contract_fail "$native_harness must report native lifecycle case $native_case."
done
grep -Fq 'queued_ops_before_ready=%d' "$native_harness" \
  || contract_fail 'The cache lifecycle regression must prove startup cache calls queue before initialization.'
grep -Fq '&& queuedOperationsPassed' "$native_harness" \
  && grep -Fq '&& cacheUsableWithinBound' "$native_harness" \
  || contract_fail 'The cache lifecycle regression must require usable queued cache operations within its bound.'
grep -Fq 'cacheStatsOnMain' \
  Telegram/SourceFiles/settings/sections/settings_local_storage.cpp \
  || contract_fail 'Local storage must subscribe to cache statistics after initialization.'
for native_case in \
  settings-deep-link \
  messages-privacy \
  paid-link \
  boost-story-menus; do
  awk -v case_name="$native_case" '
    /nativeLifecycleResults->Report\(/ { remaining = 4 }
    remaining > 0 && index($0, "\"" case_name "\"") { found = 1 }
    remaining > 0 { remaining-- }
    END { exit !found }
  ' "$native_harness" \
    || contract_fail "$native_harness must report $native_case independently."
done
grep -Fq 'status=START' "$native_harness" \
  || contract_fail "$native_harness must print active native cases before running them."
for active_case in \
  settings-search \
  session-window-lifecycle \
  translation-controls \
  manage-controls \
  mini-app-open \
  settings-deep-link \
  messages-privacy \
  paid-link \
  boost-story-menus \
  chatlist-invite-check \
  pinned-chat-participants \
  folder-crud \
  bio-editor-target \
  admin-session-identities \
  stock-basic-group-creator \
  pinned-basic-group-creator \
  stock-full-list-admin \
  stock-full-list-demotion \
  custom-full-list-admin \
  stock-live-admin-promotion \
  stock-live-admin-demotion \
  custom-live-admin-promotion \
  custom-live-admin-demotion \
  pinned-migration-fixture \
  post-close-window-lifecycle \
  chat-migration \
  protected-cache-cleanup; do
  grep -Fq "StartCase(\"$active_case\")" "$native_harness" \
    || contract_fail "$native_harness must start named case $active_case before its checks."
done
awk '
/nativeLifecycleResults->StartCase\(/ && index($0, "authenticated-cache-file-symlinks") { found = 1 }
/nativeLifecycleResults->StartCase\(/ { call = 1; next }
call && index($0, "authenticated-cache-file-symlinks") { found = 1 }
call { call = 0 }
END { exit !found }
' "$native_harness" \
  || contract_fail "$native_harness must start authenticated cache checks before running them."
require_native_start_before() {
  local case_name="$1"
  local invocation="$2"
  awk -v case_name="$case_name" -v invocation="$invocation" '
    /StartChatParticipantsRegression\(/ { in_harness = 1 }
    in_harness && !start_line && index($0, "StartCase(\"" case_name "\")") {
      start_line = NR
    }
    in_harness && start_line && !invocation_line && NR > start_line \
      && index($0, invocation) {
      invocation_line = NR
    }
    in_harness && /ReadOnlyUsernameInputRejectsWordDeletion\(\)/ { exit }
    END { exit !(start_line && invocation_line && start_line < invocation_line) }
  ' "$native_harness" \
    || contract_fail "$native_harness must start $case_name before $invocation."
}
require_native_start_before settings-search \
  'SettingsSearchMatchesExpected(&stock->session()'
require_native_start_before session-window-lifecycle \
  'windowsMatch("initial separate windows")'
require_native_start_before translation-controls \
  'RunTranslationControlsLifecycleRegression('
require_native_start_before manage-controls \
  'RunManageControlsLifecycleRegression('
require_native_start_before mini-app-open \
  'RunMiniAppOpenRefusalRegression('
require_native_start_before settings-deep-link \
  'RunSettingsDeepLinkRefusalRegression('
awk '
  /^void LogWindowActivationForRegression/ { in_activation = 1 }
  in_activation && /QGuiApplication::focusWindow\(\)/ { logs_focus_window = 1 }
  in_activation && /->isActiveWindow\(\)/ { logs_active_window = 1 }
  in_activation && /->windowState\(\)/ { logs_window_state = 1 }
  in_activation \
    && /Platform::LogWindowActivationStateForRegression\(widget\)/ {
      logs_appkit_state = 1
    }
  in_activation && /^\[\[nodiscard\]\] bool/ { exit }
  END {
    exit !(logs_focus_window && logs_active_window &&
      logs_window_state && logs_appkit_state)
  }
' "$native_harness" \
  || contract_fail 'Window activation failures must log Qt and AppKit activation diagnostics.'
for appkit_state in \
  '[NSApp isActive]' \
  '[NSApp keyWindow]' \
  '[targetWindow isKeyWindow]' \
  '[targetWindow canBecomeKeyWindow]'; do
  grep -Fq "$appkit_state" "$mac_platform" \
    || contract_fail "$mac_platform must log AppKit activation state: $appkit_state."
done
for platform_header in \
  Telegram/SourceFiles/platform/mac/specific_mac.h \
  Telegram/SourceFiles/platform/linux/specific_linux.h \
  Telegram/SourceFiles/platform/win/specific_win.h; do
  grep -Fq 'IsApplicationActiveForRegression' "$platform_header" \
    || contract_fail "$platform_header must expose application activation state to the regression fixture."
  grep -Fq 'IsWindowKeyForRegression' "$platform_header" \
    || contract_fail "$platform_header must expose actual native key-window focus to the regression fixture."
done
awk '
  /^bool IsApplicationActiveForRegression\(\)/ { in_check = 1 }
  in_check && /return \[NSApp isActive\]/ { reads_appkit = 1 }
  in_check && /^}/ { exit }
  END { exit !reads_appkit }
' "$mac_platform" \
  || contract_fail "$mac_platform must expose the current AppKit application activation state."
awk '
  /^bool IsWindowKeyForRegression\(/ { in_check = 1 }
  in_check && /window->winId\(\)/ { uses_native_handle = 1 }
  in_check && /\[targetWindow isKeyWindow\]/ { reads_key_window = 1 }
  in_check && /^}/ { exit }
  END { exit !(uses_native_handle && reads_key_window) }
' "$mac_platform" \
  || contract_fail "$mac_platform must report whether the target NSWindow is the actual AppKit key window."
awk '
  /^void RequestWindowActivationForRegression\(/ { in_request = 1 }
  in_request && /window->activate\(\)/ { qt_activate = 1 }
  in_request && /Platform::ActivateThisProcess\(window->widget\(\)\)/ {
    appkit_activate = 1
  }
  in_request && /^}/ { exit }
  END { exit !(qt_activate && appkit_activate) }
' "$native_harness" \
  || contract_fail 'The shared activation request must retry both Qt and AppKit activation.'
awk '
  /^ActivateWindowForRegression\(/ { in_activation = 1 }
  in_activation && /RequestWindowActivationForRegression\(window\)/ { uses_shared_request = 1 }
  in_activation && /^}/ { exit }
  END { exit !uses_shared_request }
' "$native_harness" \
  || contract_fail 'The bounded activation retry must use the shared Qt and AppKit activation request.'
if grep -Fq 'WindowActivationForRegressionResult::AppInactive' "$native_harness"; then
  contract_fail "$native_harness must fail instead of classifying AppKit inactivity as an unavailable focus check."
fi
if grep -Fq 'focusCheckAvailable = false;' "$native_harness"; then
  contract_fail "$native_harness must not skip settings deep-link focus assertions when the app becomes inactive."
fi
awk '
  /const auto applicationActive/ { in_active_failure = 1; app_active = NR }
  in_active_failure && /if \(!applicationActive\)/ { inactive_check = 1; in_inactive = 1 }
  in_inactive && /AppKit app remained inactive during focus verification/ {
    inactive_diagnostic = 1
  }
  in_inactive && /return WindowActivationForRegressionResult::Failed/ {
    inactive_failure = 1
  }
  in_inactive && /^[[:space:]]*}/ { inactive_branch_end = 1; in_inactive = 0 }
  in_active_failure && /return WindowActivationForRegressionResult::Failed/ {
    if (!in_inactive) active_failure = 1
  }
  in_active_failure && /^}/ { exit }
  END {
    exit !(app_active && inactive_check && inactive_diagnostic &&
      inactive_failure && inactive_branch_end && active_failure)
  }
' "$native_harness" \
  || contract_fail 'AppKit inactivity and app-active focus failures must both fail activation instead of skipping assertions.'
grep -Fq 'AppKit app remained inactive during focus verification.' "$native_harness" \
  || contract_fail "$native_harness must report AppKit inactivity after the focus wait."
awk '
  /ActivateWindowForRegression\(/ { activation_signature = 1 }
  activation_signature && /Core::Application &app/ { in_activation = 1 }
  in_activation && /if \(!activated\)/ { activation_timeout = 1 }
  in_activation && /RequestWindowActivationForRegression\(window\)/ { activation_call = 1 }
  in_activation && /WaitForRegressionCondition\(isActive, timeoutMs\)/ { bounded_wait = 1 }
  in_activation && /if \(!applicationActive\)/ {
    timeout_appkit_check = 1
  }
  in_activation && /^}/ { exit }
  END {
    exit !(activation_call && activation_timeout && bounded_wait &&
      timeout_appkit_check)
  }
' "$native_harness" \
  || contract_fail "$native_harness must issue activation before failing on AppKit inactivity at timeout."
awk '
  /IsWindowActiveForRegression\(/ { in_check = 1 }
  in_check && /Platform::IsApplicationActiveForRegression\(\)/ { requires_foreground = 1 }
  in_check && /widget->isActiveWindow\(\)/ { checks_active_widget = 1 }
  in_check && /widget->isVisible\(\)/ { checks_visible_widget = 1 }
  in_check && /Qt::WindowMinimized/ { checks_unminimized_widget = 1 }
  in_check && /requireForeground/ { foreground_opt_out = 1 }
  in_check && /^}/ { exit }
  END {
    exit !(requires_foreground && !foreground_opt_out &&
      checks_active_widget && checks_visible_widget && checks_unminimized_widget)
  }
' "$native_harness" \
  || contract_fail "$native_harness must require foreground, active, visible, unminimized window state for every caller."
awk '
  /WindowActivationEvidenceMatchesForRegression\(/ { in_evidence = 1 }
  in_evidence && /evidence.qtFocusWindow/ { checks_qt_focus = 1 }
  in_evidence && /evidence.appKitKeyWindow/ { checks_appkit_key = 1 }
  in_evidence && /^}/ { exit }
  END { exit !(checks_qt_focus && checks_appkit_key) }
' "$native_harness" \
  || contract_fail "$native_harness must require both Qt focusWindow and AppKit key-window evidence."
awk '
  /WindowActivationFocusLossRegressionPasses\(/ { self_test++ }
  /const auto stable = WaitForRegressionCondition/ { in_stability = 1 }
  in_stability && /IsWindowActiveForRegression\(/ { checks_actual_focus = 1 }
  in_stability && /RequestWindowActivationForRegression\(/ { retries_both_activators = 1 }
  in_stability && /return stableSince.elapsed\(\) >= 100/ { keeps_full_stability_window = 1 }
  in_stability && /const auto stockToastBeforeMissingController/ { in_stability = 0 }
  END {
    exit !(self_test >= 2 && checks_actual_focus && retries_both_activators &&
      keeps_full_stability_window)
  }
' "$native_harness" \
  || contract_fail "$native_harness must test focus loss with unchanged controller pointers and recheck Qt plus AppKit focus throughout the stability window."
awk '
  /ActivateWindowForRegression\(/ { activation_signature = 1 }
  activation_signature && /Core::Application &app/ { in_activation = 1 }
  in_activation && /RequestWindowActivationForRegression\(window\)/ { activation_call = 1 }
  in_activation && /const auto timeoutMs/ { timeout_choice = 1 }
  in_activation && /Platform::IsApplicationActiveForRegression\(\)/ && !appkit_state {
    appkit_state = 1
  }
  in_activation && /5000/ { active_timeout = 1 }
  in_activation && /: 500/ { inactive_timeout = 1 }
  in_activation && /WaitForRegressionCondition\(isActive, timeoutMs\)/ { bounded_wait = 1 }
  in_activation && /^}/ { exit }
  END {
    exit !(activation_call && timeout_choice && appkit_state &&
      active_timeout && inactive_timeout && bounded_wait)
  }
' "$native_harness" \
  || contract_fail "$native_harness must use a bounded shorter activation wait when AppKit starts inactive."
awk '
  /ActivateWindowForRegression\(/ { activation_signature = 1 }
  activation_signature && /Core::Application &app/ { in_activation = 1 }
  in_activation && /const auto isActive =/ { in_predicate = 1 }
  in_predicate && /IsWindowActiveForRegression\(/ { uses_window_predicate = 1 }
  in_predicate && /app, window, requirePrimary\)/ { passes_options = 1 }
  in_activation && /WaitForRegressionCondition\(isActive, timeoutMs\)/ { waits_for_predicate = 1 }
  in_activation && /^}/ { exit }
  END { exit !(uses_window_predicate && passes_options && waits_for_predicate) }
' "$native_harness" \
  || contract_fail "$native_harness must wait on the shared window activity predicate before reporting activation."
awk '
  /const auto showPeerHistoryMeasured =/ { in_measurement = 1 }
  in_measurement && /IsWindowActiveForRegression\(Core::App\(\), window, false\)/ {
    active_check = 1
  }
  in_measurement && /ActivateWindowForRegression\(/ { activation = 1 }
  in_measurement && /chatSwitchWindowsActivated/ { activation_result = 1 }
  in_measurement && /timer.start\(\)/ { timer_start = 1; exit }
  END {
    exit !(active_check && activation && activation_result && timer_start)
  }
' "$native_harness" \
  || contract_fail "$native_harness must confirm or restore foreground activation for every measured chat switch before starting its timer."
grep -Fq 'result.chatSwitchWindowsActivated = false;' "$native_harness" \
  && grep -Fq '&& result.chatSwitchWindowsActivated' "$native_harness" \
  || contract_fail "$native_harness must fail the chat-switch regression if a foreground window cannot be established for every sample."
awk '
  /const auto showPeerHistoryMeasured =/ { in_measurement = 1 }
  in_measurement && /index == 0.*&& !result.initialHistoryWindowActivated/ { initial_guard = 1 }
  in_measurement && /controller->showPeerHistory\(history\)/ { shows_history = 1 }
  in_measurement && /timer.start\(\)/ { starts_timer = 1 }
  in_measurement && /^}/ { exit }
  END { exit !(initial_guard && shows_history && starts_timer) }
' "$native_harness" \
  || contract_fail "$native_harness must keep the initial selection and its timing guard."
awk '
  /Platform::ActivateThisProcess\(window->widget\(\)\)/ {
    process_activation = 1
  }
  /result.initialHistoryWindowActivated/ { activation_result = 1 }
  /WindowActivationForRegressionResult::Activated/ { activation_complete = 1 }
  /settleHistoryUnfocused\(\)/ { settled = 1 }
  /IsWindowActiveForRegression\(Core::App\(\), window, false\)/ { activation_recheck = 1 }
  /showPeerHistoryMeasured\(emptyFirstHistory\)/ {
    cold_selection = 1
  }
  END { exit !(process_activation && activation_result && activation_complete &&
    settled && activation_recheck && cold_selection) }
' "$native_harness" \
  || contract_fail "$native_harness must activate and recheck foreground around the cold first history selection."
if grep_file_pattern_status 'focusCheckAvailable|native focus assertion skipped' \
	"$native_harness"; then
	contract_fail "$native_harness must keep every Settings deep-link focus assertion mandatory."
else
	grep_status=$?
	if (( grep_status != 1 )); then
		contract_fail "$native_harness could not be read for the mandatory focus assertion check."
	fi
fi
grep -Fq 'ordinaryWindowActive' "$native_harness" \
	|| contract_fail "$native_harness must assert focus for the ordinary Settings route."
awk '
  /RunSettingsDeepLinkRefusalRegression\(/ { in_settings = 1 }
  in_settings && /ordinaryWindowActive/ { ordinary_window_active = 1 }
  in_settings && /activateSettingsWindow\(&pinnedController->window\(\)\)/ {
    custom_window_activations++
    unsupported_route_activation = 1
  }
  in_settings && /pinnedController, activeBeforePinnedMatrix/ {
    custom_window_matrix = 1
  }
  in_settings && /blockedController,/ {
    blocked_window_matrix = 1
  }
  in_settings && !unsupported_route_checks && /const auto minimumLayerWidth/ {
    unsupported_route_checks = 1
  }
  in_settings && /const auto pinnedMessageState/ { exit }
  END {
    exit !(custom_window_activations >= 2 && custom_window_matrix &&
      blocked_window_matrix && unsupported_route_activation &&
      unsupported_route_checks && ordinary_window_active)
  }
' "$native_harness" \
  || contract_fail 'Settings deep-link checks must keep the base activation check and run the custom-active URL matrix once.'
awk '
  /ActivateWindowForRegression\(/ { activation_signature = 1 }
  activation_signature && /Core::Application &app/ { in_activation = 1 }
  in_activation && /if \(!activated\)/ { activation_timeout = 1 }
  in_activation && /LogWindowActivationForRegression\(app, window\)/ {
    activation_diagnostics = 1
  }
  in_activation && /WindowActivationForRegressionResult::Failed/ { exit }
  END { exit !(activation_timeout && activation_diagnostics) }
' "$native_harness" \
  || contract_fail 'Activation failures must log the target Qt and AppKit window state.'
awk '
  /RunSettingsDeepLinkRefusalRegression\(/ { in_settings = 1 }
  in_settings && /if \(!ordinarySettingsStateRestored/ { ordinary_settings_failure = NR }
  in_settings \
    && /LogWindowActivationForRegression\(app, &pinnedController->window\(\)\)/ {
      ordinary_settings_diagnostics = NR
    }
  in_settings && /^\[\[nodiscard\]\] HistoryItem/ { exit }
  END {
    exit !(ordinary_settings_failure &&
      ordinary_settings_diagnostics > ordinary_settings_failure)
  }
' "$native_harness" \
  || contract_fail 'Ordinary Settings failures must log the target Qt and AppKit window state.'
awk '
/if \(authenticatedCacheOnly\) \{/ { in_cache_branch = 1; next }
in_cache_branch && /const auto capabilitiesMatch/ { exit }
in_cache_branch && /crl::on_main\(\[=\] \{/ { completion_queued = 1 }
in_cache_branch && /done\(/ {
  if (!completion_queued) {
    direct_completion = 1
  } else {
    queued_completion = 1
  }
}
END { exit !(completion_queued && queued_completion && !direct_completion) }
' "$native_harness" \
  || contract_fail "$native_harness must defer authenticated-cache completion until its setup stack unwinds."
require_native_start_before messages-privacy \
  'RunMessagesPrivacyBoxRegression('
require_native_start_before paid-link \
  'RunPaidLinkRefusalRegression('
require_native_start_before boost-story-menus \
  'RunBoostMenuVisibilityRegression('
require_native_start_before chatlist-invite-check \
  'Api::CheckFilterInvite('
require_native_start_before folder-crud \
  'Settings::RunFoldersCrudRegressionForTest('
require_native_start_before admin-session-identities \
  'const auto stockSelf = stock->session().user();'
require_native_start_before stock-basic-group-creator \
  'RegressionStockChatFullReply('
require_native_start_before post-close-window-lifecycle \
  'windowsMatch("after primary close")'
for independent_case in \
  settings-deep-link \
  messages-privacy \
  paid-link \
  boost-story-menus; do
  awk -v case_name="$independent_case" '
    /nativeLifecycleResults->Report\("mini-app-open"/ { mini_app_line = NR }
    /nativeLifecycleResults->StartCase\(/ &&
      index($0, "\"" case_name "\"") { case_line = NR }
    END { exit !(mini_app_line && case_line && mini_app_line < case_line) }
  ' "$native_harness" \
    || contract_fail "$independent_case must run after mini-app-open completes."
done
awk '
  /RunMiniAppOpenRefusalRegression\(/ { in_mini_app = 1 }
  in_mini_app && /restorePinnedServerOnExit = gsl::finally/ {
    cleanup_guard = NR
    in_cleanup = 1
  }
  in_cleanup && /std::fprintf/ { cleanup_diagnostic = NR }
  in_cleanup && /Expects\(/ { fatal_cleanup = 1 }
  in_mini_app && /ConfigurePinnedServer\(pinned, RegressionServerKey\(\)\)/ {
    if (cleanup_guard && !blocked_setup && NR > cleanup_guard) {
      cleanup_restore = NR
    }
    if (blocked_setup && NR > blocked_setup) {
      normal_restore = NR
    }
  }
  in_mini_app && /pinned->mtp\(\)\.dcOptions\(\)\.constructBlocked\(\)/ {
    blocked_setup = NR
  }
  in_cleanup && /^[[:space:]]*}\);/ { in_cleanup = 0 }
  in_mini_app && /pinnedServerRestored = true/ { normal_restore_attempted = NR }
  in_mini_app && /if \(!customPinConfigured/ { normal_restore_check = NR }
  in_mini_app && /FailChatParticipantsRegression\(/ { exit }
  END {
    exit !(cleanup_guard && cleanup_restore < blocked_setup && normal_restore && cleanup_diagnostic && !fatal_cleanup && normal_restore < normal_restore_attempted && normal_restore_attempted < normal_restore_check)
  }
' "$native_harness" \
  || contract_fail 'The custom-plus-blocked mini-app cleanup must report restore failures without aborting or retrying a normal restore.'
awk '
  /RunMiniAppOpenRefusalRegression\(/ { in_mini_app = 1 }
  in_mini_app && /cleanupStockWebViewOnExit = gsl::finally/ {
    cleanup_guard = NR
    in_cleanup = 1
  }
  in_cleanup && /stock->session\(\)\.attachWebView\(\)\.cancel\(\)/ {
    cancel = NR
  }
  in_cleanup && /stock->session\(\)\.attachWebView\(\)\.closeAll\(\)/ {
    close_all = NR
  }
  in_cleanup && /^[[:space:]]*}\);/ { in_cleanup = 0 }
  in_mini_app && /stock->session\(\)\.attachWebView\(\)\.open\(\{/ {
    first_stock_open = NR
    exit
  }
  in_mini_app && /FailChatParticipantsRegression\(/ { exit }
  END {
    exit !(cleanup_guard && cancel > cleanup_guard && close_all > cancel && first_stock_open > close_all)
  }
' "$native_harness" \
  || contract_fail 'The mini-app case must cancel and close stock webviews on every exit after its first open.'
awk '
  /const auto blankFixtureReady = \(blankController != nullptr\)/ {
    in_blank_fixture_cleanup = 1
  }
  in_blank_fixture_cleanup && /const auto blankLifetime = base::make_weak\(blank.get\(\)\)/ {
    weak_account = NR
  }
  in_blank_fixture_cleanup && /cleanupBlank = gsl::finally\(\[blankLifetime\]/ {
    cleanup_guard = NR
    in_cleanup = 1
  }
  in_cleanup && /blankLifetime\.get\(\)/ { weak_check = NR }
  in_cleanup && /blank->(sessionExists|forcedLogOut)\(\)/ { raw_account_use = 1 }
  in_cleanup && /^[[:space:]]*}\);/ { in_cleanup = 0 }
  in_blank_fixture_cleanup && /nativeLifecycleResults->StartCase\("settings-deep-link"\)/ { exit }
  END {
    exit !(weak_account && cleanup_guard > weak_account && weak_check && !raw_account_use)
  }
' "$native_harness" \
  || contract_fail 'The blocked mini-app cleanup must check a weak account reference before logout.'
grep -Fq 'StartCase(caseName)' "$native_harness" \
  || contract_fail "$native_harness must start each post-open cache case before setup."
grep -Fq 'TDESKTOP_AUTH_LIFECYCLE_REGRESSION_FAIL_CASE' "$native_harness" \
  || contract_fail 'The native lifecycle harness must support a deliberate early-case failure.'
grep -Fq 'native_lifecycle_cases_failed=' "$native_harness" \
  || contract_fail 'The native lifecycle harness must summarize failed case names.'
grep -Fq 'dependency-chat-participants-prerequisites' "$native_harness" \
  || contract_fail 'The native lifecycle harness must name skipped prerequisite-dependent cases.'
grep -Fq 'dependency-pinned-migration-fixture' "$native_harness" \
  || contract_fail 'The native lifecycle harness must name migration skips when its fixture fails.'
grep -Fq 'dependency-admin-session-identities' "$native_harness" \
  || contract_fail 'The native lifecycle harness must skip admin cases when session identities fail.'
grep -Fq 'native_lifecycle_case=settings-search result=FAIL' \
  "$profile_harness" \
  || contract_fail 'The profile harness must verify the injected native failure.'
grep -Fq 'native_lifecycle_case=post-open-media-cache result=' \
  "$profile_harness" \
  || contract_fail 'The profile harness must verify a later native cache result.'
grep -Fq 'username-input,teagram-icon-persistence,legacy-cleanup-symlink,cache-callback-timeout,cache-concurrent-deletion' \
  "$profile_harness" \
  || contract_fail 'The profile harness must inject early native failures.'
grep -Fq 'failed_cases=username-input,teagram-icon-persistence,legacy-cleanup-symlink,cache-callback-timeout,cache-concurrent-deletion,stock-incoming-call-answer,pinned-incoming-call-answer,settings-search,folder-crud,admin-session-identities later_cases=' \
  "$profile_harness" \
  || contract_fail 'The continuation summary must list every injected native failure.'
grep -Fq 'stock-incoming-call-answer,pinned-incoming-call-answer,settings-search,folder-crud,admin-session-identities' \
  "$profile_harness" \
  || contract_fail 'The profile harness must inject failures before and after the collected native checks.'
grep -Fq 'native_lifecycle_case=folder-crud result=FAIL' "$profile_harness" \
  || contract_fail 'The profile harness must verify that a folder failure is collected.'
grep -Fq 'native_lifecycle_case=stock-basic-group-creator result=' "$profile_harness" \
  || contract_fail 'The profile harness must verify later native checks after folder CRUD.'
require_step_field "$mac_workflow" build-macos 'Report skipped profile and IPC integration tests.' '.if' \
  "\${{ !cancelled() && steps.ccache-profile.outputs.protected_path_integration == 'ON' && steps.build.outcome != 'success' }}"
require_step_script "$mac_workflow" build-macos 'Report skipped profile and IPC integration tests.' \
  'Profile and IPC cases skipped: Teagram build did not succeed'
require_step_field "$mac_workflow" build-macos 'Report skipped full chat info session regression.' '.if' \
  "\${{ !cancelled() && steps.build.outcome != 'success' && (github.event_name == 'workflow_dispatch' || (github.event_name == 'pull_request' && github.event.label.name == 'macos-build')) }}"
require_step_script "$mac_workflow" build-macos 'Report skipped full chat info session regression.' \
  'Full chat info session regression skipped: Teagram build did not succeed'
cleanup_mutation="$temp_dir/mac-without-cleanup.yml"
sed 's/^\( *\)signal_pid(app_pid, "Teagram", cleanup_signal)$/\1# signal_pid(app_pid, "Teagram", cleanup_signal)\n\1pass/' \
  "$mac_workflow" > "$cleanup_mutation"
if cmp -s "$mac_workflow" "$cleanup_mutation"; then
  contract_fail 'The cleanup mutation must change the regression step.'
fi
if (require_step_python "$cleanup_mutation" build-macos "$regression_step" \
    "signal_pid(app_pid, 'Teagram', cleanup_signal)") > /dev/null 2>&1; then
  contract_fail 'A commented-out signal_pid call must fail the regression step contract.'
fi
# Only a heredoc that is the stdin program of a python3 command counts; the
# same Python handed to any other command is inert data.
inert_mutation="$temp_dir/mac-inert-python.yml"
sed 's|^\( *\)python3 - "$repo/.github/scripts"|\1echo python3 - "$repo/.github/scripts"|' "$mac_workflow" > "$inert_mutation"
if cmp -s "$mac_workflow" "$inert_mutation"; then
  contract_fail 'The inert-heredoc mutation must change the regression step.'
fi
if (require_step_python "$inert_mutation" build-macos "$regression_step" \
    "signal_pid(app_pid, 'Teagram', cleanup_signal)") > /dev/null 2>&1; then
  contract_fail 'Python in a heredoc that python3 does not run must fail the regression step contract.'
fi
python_heredoc_parser=".github/scripts/workflow_python_heredocs.py"
for python_command in \
  "python3 - <<'PY'" \
  "env HOME=x A=1 python3 - \"\$a\" > f 2>&1 <<'PY'" \
  "if ! python3 - \"\$r\" > \"\$d/x\" 2>&1 <<'PY'" \
  "python3 -I <<'PY'"; do
  printf '%s\nprobe = 1\nPY\n' "$python_command" | python3 -I "$python_heredoc_parser" | grep -Fxq 'probe = 1' \
    || contract_fail "A heredoc read by python3 must count: $python_command"
done
for inert_command in \
  "echo python3 - <<'PY'" \
  ": python3 <<'PY'" \
  "python3 -c 'pass' <<'PY'" \
  "python3 script.py <<'PY'" \
  "cat <<'PY' | python3 -" \
  "python3 --version <<'PY'" \
  "python3 --help <<'PY'" \
  "python3 -V - <<'PY'" \
  "python3 -X dev - <<'PY'" \
  "env -u python3 - <<'PY'" \
  "env -i python3 - <<'PY'" \
  "command -v python3 - <<'PY'" \
  "exec -a python3 echo - <<'PY'"; do
  if printf '%s\nprobe = 1\nPY\n' "$inert_command" | python3 -I "$python_heredoc_parser" | grep -Fxq 'probe = 1'; then
    contract_fail "A heredoc that python3 does not run as its program must not count: $inert_command"
  fi
done
reject_value "$mac_workflow" \
  'out/Debug/CMakeCache.txt' \
  'out/Debug/compile_commands.json' \
  'ninja -C "$repo/out/Debug"' \
  'os.killpg(process.pid, signal.SIGTERM)'
evidence_step='Upload deferred switch mutation evidence.'
require_step_field "$mac_workflow" build-macos "$evidence_step" '.if' \
  "\${{ always() && steps.lifecycle-regression.outcome == 'success' }}"
require_step_field "$mac_workflow" build-macos "$evidence_step" '.with["if-no-files-found"]' 'error'
require_step_field "$mac_workflow" build-macos "$evidence_step" '.with.path | test("/deferred-switch-mutation-evidence\.txt$")' 'true'

# MacOS Packaged. builds dev and main pushes in one queue and publishes from a
# protected environment.
require_triggers "$packaged_workflow" 'push,workflow_dispatch'
require_workflow_field "$packaged_workflow" '.on.push.branches | join(",")' 'dev,main'
require_workflow_field "$packaged_workflow" '.concurrency.group' \
  "\${{ github.workflow }}-\${{ github.event_name == 'push' && 'release' || inputs.branch || github.ref_name }}"
require_workflow_field "$packaged_workflow" '.concurrency["cancel-in-progress"]' 'false'
require_workflow_field "$packaged_workflow" '.concurrency.queue' 'null'
require_permission_map "$packaged_workflow" 'workflow: inherit
build-macos-packaged: actions=write,contents=read,packages=read
publish-release: actions=read,contents=write'
require_job_field "$packaged_workflow" publish-release '.if' \
  "\${{ github.event_name == 'push' && github.repository == 'teagramhq/teagram-desktop' && (github.ref == 'refs/heads/dev' || github.ref == 'refs/heads/main') }}"
require_job_field "$packaged_workflow" publish-release '.environment.name' 'release'
require_job_field "$packaged_workflow" build-macos-packaged '.env.CCACHE_MAXSIZE' '5G'
require_step_field "$packaged_workflow" build-macos-packaged 'Upload Teagram manual QA artifact.' '.with.name' \
  'Teagram-macOS-arm64-QA'
packaged_build_script="$(step_script "$packaged_workflow" build-macos-packaged 'Teagram build.')"
expect_script_contains "$packaged_workflow job build-macos-packaged step 'Teagram build.' run" "$packaged_build_script" \
  'openssl pkey -pubin -in "Telegram/build/teagram_update_public_key.pem"'
public_key_path="$(sed -n 's/.*openssl pkey -pubin -in "\([^"]*\)".*/\1/p' <<< "$packaged_build_script" | sed -n '1p')"
if [[ -z "$public_key_path" ]] || ! openssl pkey -pubin -in "$public_key_path" -outform DER | shasum -a 256 >/dev/null; then
  contract_fail 'The packaged workflow public key path must work from the cloned repository directory.'
fi
reject_value "$packaged_workflow" \
  '"$REPO_NAME/Telegram/build/teagram_update_public_key.pem"' \
  'CCACHE_DISABLE=1 cmake --build'
expect_equal "$packaged_workflow full chat info regression steps" \
  "$(yq -r '.jobs[].steps[] | select(.name == "Full chat info session regression.") | .name' "$packaged_workflow")" ''

# MacOS Packaged. builds against the same pinned Libraries and build tools as
# MacOS., never against Homebrew or a moving branch.
prepare_libraries='./$REPO_NAME/Telegram/build/prepare/mac.sh skip-release silent'
require_step_script "$mac_workflow" build-macos 'Libraries.' "$prepare_libraries"
require_step_script "$packaged_workflow" build-macos-packaged 'Libraries.' "$prepare_libraries" \
  'install_macos_build_tools.sh" "$RUNNER_TEMP/build-tools" libraries'
require_step_script "$packaged_workflow" build-macos-packaged 'First set up.' \
  'install_macos_build_tools.sh" "$tools" client'
require_step_script "$packaged_workflow" build-macos-packaged 'First set up.' 'for formula in oras zstd'
require_step_script "$packaged_workflow" build-macos-packaged 'Teagram build.' './configure.sh'
require_step_field "$packaged_workflow" build-macos-packaged 'Set macOS dependency image tag.' \
  '.env.DEPENDENCY_HASH' \
  "\${{ hashFiles(format('{0}/Telegram/build/prepare/prepare.py', env.REPO_NAME), format('{0}/Telegram/build/prepare/mac.sh', env.REPO_NAME), format('{0}/Telegram/build/qt_version.py', env.REPO_NAME)) }}"
require_step_field "$packaged_workflow" build-macos-packaged 'Restore macOS libraries artifact.' '.run' \
  "bash \"\$REPO_NAME/$macos_libraries_image_script\" pull \"\$MACOS_LIBRARIES_IMAGE\" \"\$GITHUB_WORKSPACE\" \"\$GITHUB_OUTPUT\""
require_step_field "$packaged_workflow" build-macos-packaged 'Restore legacy macOS libraries cache.' '.uses' \
  'actions/cache/restore@v6'
require_step_field "$packaged_workflow" build-macos-packaged 'Restore legacy macOS libraries cache.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true'"
expect_equal "$packaged_workflow legacy cache key" \
  "$(step_field "$packaged_workflow" build-macos-packaged 'Restore legacy macOS libraries cache.' '.with.key')" \
  "$(step_field "$mac_workflow" build-macos-deps-artifact 'Restore legacy macOS libraries cache.' '.with.key')"
require_step_field "$packaged_workflow" build-macos-packaged 'Libraries.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true' && steps.legacy-cache-libs.outputs.cache-hit != 'true'"
reject_value "$packaged_workflow" \
  'refs/heads/master' \
  'git clone' \
  '/opt/homebrew/opt/' \
  '/opt/homebrew/Cellar/' \
  'CMAKE_PREFIX_PATH' \
  'macdeployqt'
build_tools_script=".github/scripts/install_macos_build_tools.sh"
tool_entries="$(grep -E '^  "[a-z0-9]+\|' "$build_tools_script" || true)"
[[ -n "$tool_entries" ]] || contract_fail "$build_tools_script must list its pinned tools."
floating_tools="$(grep -vE '^  "[a-z0-9]+\|https://[^|]*[0-9][^|]*\|[0-9a-f]{64}"$' <<< "$tool_entries" || true)"
[[ -z "$floating_tools" ]] \
  || contract_fail "$build_tools_script must pin every tool to a versioned URL and SHA-256:
$floating_tools"
build_tools_body="$(strip_shell_comments < "$build_tools_script")" \
  || contract_fail "$build_tools_script does not parse as bash."
expect_script_lacks "$build_tools_script" "$build_tools_body" 'brew '
expect_script_contains "$build_tools_script" "$build_tools_body" 'ln -sf glibtoolize "$prefix/bin/libtoolize"'
grep -Fq '"meson|' <<< "$tool_entries" \
  || contract_fail "$build_tools_script must install pinned Meson for a cold dependency publisher."
expect_script_contains "$build_tools_script" "$build_tools_body" \
  'for command in m4 autoconf automake autoreconf libtoolize glibtool nasm meson; do'
prepare_recipe="Telegram/build/prepare/prepare.py"
mutable_checkouts="$(grep -nE '^ *git checkout ' "$prepare_recipe" \
  | grep -vE 'git checkout ([0-9a-f]{7,40}|v[0-9]+(\.[0-9]+)+)$' || true)"
[[ -z "$mutable_checkouts" ]] \
  || contract_fail "$prepare_recipe must check out a commit SHA or a version tag:
$mutable_checkouts"
mutable_clones="$(grep -nE '^ *git clone .*-b (master|main|dev)( |$)' "$prepare_recipe" || true)"
[[ -z "$mutable_clones" ]] \
  || contract_fail "$prepare_recipe must not clone a moving branch:
$mutable_clones"

# MacOS unit feedback. is dispatch-only and reads the recipe-tagged GHCR artifact.
require_triggers "$unit_workflow" 'workflow_dispatch'
require_workflow_field "$unit_workflow" '.concurrency.group' \
  '${{ github.workflow }}-${{ inputs.branch || github.event.pull_request.head.ref || inputs.ref || github.ref_name }}'
require_workflow_field "$unit_workflow" '.concurrency["cancel-in-progress"]' 'true'
require_permission_map "$unit_workflow" 'workflow: contents=read,packages=read
test-macos-unit: inherit'
require_step_field "$unit_workflow" test-macos-unit 'Set macOS dependency image tag.' \
  '.env.DEPENDENCY_HASH' \
  "\${{ hashFiles(format('{0}/Telegram/build/prepare/prepare.py', env.REPO_NAME), format('{0}/Telegram/build/prepare/mac.sh', env.REPO_NAME), format('{0}/Telegram/build/qt_version.py', env.REPO_NAME)) }}"
require_step_field "$unit_workflow" test-macos-unit 'Restore macOS libraries artifact.' '.run' \
  "bash \"\$REPO_NAME/$macos_libraries_image_script\" pull \"\$MACOS_LIBRARIES_IMAGE\" \"\$GITHUB_WORKSPACE\" \"\$GITHUB_OUTPUT\""
require_step_field "$unit_workflow" test-macos-unit 'Restore legacy macOS libraries cache.' '.uses' \
  'actions/cache/restore@v6'
require_step_field "$unit_workflow" test-macos-unit 'Restore legacy macOS libraries cache.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true'"
expect_equal "$unit_workflow legacy cache key" \
  "$(step_field "$unit_workflow" test-macos-unit 'Restore legacy macOS libraries cache.' '.with.key')" \
  "$(step_field "$mac_workflow" build-macos-deps-artifact 'Restore legacy macOS libraries cache.' '.with.key')"
require_step_field "$unit_workflow" test-macos-unit 'Libraries.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true' && steps.legacy-cache-libs.outputs.cache-hit != 'true'"

# macOS updater fixtures. tests the exact PR merge ref with read-only package access.
require_triggers "$updater_workflow" 'pull_request'
updater_paths="$(workflow_field "$updater_workflow" '.on.pull_request.paths[]')"
for updater_path in \
  'Telegram/SourceFiles/core/teagram_update_*' \
  'Telegram/SourceFiles/core/mac_protected_path_access.h' \
  'Telegram/SourceFiles/core/mac_protected_path_runtime.h' \
  'Telegram/SourceFiles/tests/unit/mac_protected_path_runtime_stub.cpp' \
  'Telegram/SourceFiles/tests/unit/mac_file_bookmark_stub.cpp' \
  "$updater_runner"; do
  grep -Fxq -- "$updater_path" <<< "$updater_paths" \
    || contract_fail "$updater_workflow on.pull_request.paths must list $updater_path"
done
require_permission_map "$updater_workflow" 'workflow: contents=read,packages=read
test-updater-fixtures: inherit'
require_workflow_field "$updater_workflow" '.concurrency["cancel-in-progress"]' 'true'
require_job_field "$updater_workflow" test-updater-fixtures '.["runs-on"]' 'macos-latest'
require_job_field "$updater_workflow" test-updater-fixtures '.["timeout-minutes"]' '180'
expect_contains "$updater_workflow GITHUB_TOKEN reference" \
  "$(workflow_strings "$updater_workflow")" 'secrets.GITHUB_TOKEN'
reject_value "$updater_workflow" \
  'TEAGRAM_UPDATE_PRIVATE_KEY' \
  'EXPECTED_BASE_SHA' \
  'github.event.pull_request.base.sha' \
  'github.event.pull_request.head.repo.full_name' \
  '"$REPO_NAME/Telegram/out'
checkout_step='Check out the exact PR merge ref.'
require_step_field "$updater_workflow" test-updater-fixtures "$checkout_step" '.with.ref' '${{ github.sha }}'
require_step_field "$updater_workflow" test-updater-fixtures "$checkout_step" '.with["fetch-depth"]' '2'

# The merge ref check passes only for a two-parent merge of the PR head.
verify_step='Verify the checked out PR merge ref.'
require_step_field "$updater_workflow" test-updater-fixtures "$verify_step" '.env.EXPECTED_MERGE_SHA' '${{ github.sha }}'
require_step_field "$updater_workflow" test-updater-fixtures "$verify_step" '.env.EXPECTED_HEAD_SHA' \
  '${{ github.event.pull_request.head.sha }}'
require_step_field "$updater_workflow" test-updater-fixtures 'Set macOS dependency image tag.' \
  '.env.DEPENDENCY_HASH' \
  "\${{ hashFiles(format('{0}/Telegram/build/prepare/prepare.py', env.REPO_NAME), format('{0}/Telegram/build/prepare/mac.sh', env.REPO_NAME), format('{0}/Telegram/build/qt_version.py', env.REPO_NAME)) }}"
require_step_field "$updater_workflow" test-updater-fixtures 'Restore macOS libraries artifact.' '.run' \
  "bash \"\$REPO_NAME/$macos_libraries_image_script\" pull \"\$MACOS_LIBRARIES_IMAGE\" \"\$GITHUB_WORKSPACE\" \"\$GITHUB_OUTPUT\""
require_step_field "$updater_workflow" test-updater-fixtures 'Restore legacy macOS libraries cache.' '.uses' \
  'actions/cache/restore@v6'
require_step_field "$updater_workflow" test-updater-fixtures 'Restore legacy macOS libraries cache.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true'"
expect_equal "$updater_workflow legacy cache key" \
  "$(step_field "$updater_workflow" test-updater-fixtures 'Restore legacy macOS libraries cache.' '.with.key')" \
  "$(step_field "$mac_workflow" build-macos-deps-artifact 'Restore legacy macOS libraries cache.' '.with.key')"
require_step_field "$updater_workflow" test-updater-fixtures 'Prepare libraries.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true' && steps.legacy-cache-libs.outputs.cache-hit != 'true'"
for libraries_cache_workflow in "$mac_workflow" "$packaged_workflow" "$unit_workflow" "$updater_workflow"; do
  expect_lacks "$libraries_cache_workflow legacy cache writers" \
    "$(cache_writers "$libraries_cache_workflow")" 'Save libraries cache.'
done
verify_script="$temp_dir/verify-merge-ref.sh"
step_field "$updater_workflow" test-updater-fixtures "$verify_step" '.run' > "$verify_script"
merge_repo="$temp_dir/merge-repo"
git_quiet() {
  git -C "$merge_repo" -c user.name=contract -c user.email=contract@example.invalid \
    -c commit.gpgsign=false "$@" > /dev/null
}
git init -q -b base "$merge_repo"
git_quiet commit -q --allow-empty -m base
git_quiet checkout -q -b head
git_quiet commit -q --allow-empty -m head
head_sha="$(git -C "$merge_repo" rev-parse HEAD)"
git_quiet checkout -q base
git_quiet commit -q --allow-empty -m advanced-base
git_quiet merge -q --no-ff -m merge head
merge_sha="$(git -C "$merge_repo" rev-parse HEAD)"
run_verify() {
  env REPO_NAME="$merge_repo" RUNNER_TEMP="$temp_dir/runner" \
    EXPECTED_MERGE_SHA="$1" EXPECTED_HEAD_SHA="$2" \
    bash "$verify_script" > /dev/null 2>&1
}
run_verify "$merge_sha" "$head_sha" \
  || contract_fail "$verify_step must accept the PR merge ref of the PR head."
if run_verify "$merge_sha" "$merge_sha"; then
  contract_fail "$verify_step must refuse a merge whose second parent is not the PR head."
fi
if run_verify "$head_sha" "$head_sha"; then
  contract_fail "$verify_step must refuse a checkout that is not the expected merge ref."
fi

# Fixture temporary paths are canonical so symlinked /var and /private/var agree.
canonical_step='Canonicalize the fixture temporary directory.'
canonical_script="$temp_dir/canonicalize.sh"
step_field "$updater_workflow" test-updater-fixtures "$canonical_step" '.run' > "$canonical_script"
mkdir -p "$temp_dir/real-tmp"
ln -s "$temp_dir/real-tmp" "$temp_dir/linked-tmp"
env TMPDIR="$temp_dir/linked-tmp" GITHUB_ENV="$temp_dir/github-env" \
  bash "$canonical_script" > /dev/null
expect_equal "$canonical_step GITHUB_ENV" "$(cat "$temp_dir/github-env")" \
  "TMPDIR=$(cd "$temp_dir/real-tmp" && pwd -P)"

require_step_script "$updater_workflow" test-updater-fixtures 'Build updater fixture runner.' \
  'cmake --build "$REPO_NAME/out"'
require_step_script "$updater_workflow" test-updater-fixtures 'Run focused updater fixtures.' \
  "bash \"\$REPO_NAME/$updater_runner\"" \
  '"$REPO_NAME/out/Debug/test_unit"'
diagnostics_step='Upload updater fixture diagnostics.'
require_step_field "$updater_workflow" test-updater-fixtures "$diagnostics_step" '.uses | sub("@.*", "")' \
  'actions/upload-artifact'
require_step_field "$updater_workflow" test-updater-fixtures "$diagnostics_step" '.with["retention-days"]' '14'

# The fixture runner runs each registered case once, in its own process, and
# bounds every case's log.
fixtures=(
  TeagramUpdateArchiveWritesStayConfined
  TeagramUpdateInstallFailurePreservesApp
  TeagramUpdateMarkerRejectsHardLinkWithoutTruncatingCanary
  TeagramUpdateInstallLocationAllowsSharedWritableParent
  TeagramUpdateReplacementSwapsInSharedWritableParent
  TeagramUpdateInstallLockLeavesNoFileAfterUpdate
  TeagramUpdateInstallLockHonorsLegacyLock
  TeagramUpdateInstallLocationAllowsGroupWritableApplications
  TeagramUpdateInstallerExercisesAuthenticatedCandidateAndFallback
  TeagramUpdateInstallerRemovesInterruptedExtraction
  TeagramUpdateInstallerClearsUncommittedCandidateOnRestart
  TeagramUpdateStartupKeepsOlderPreviousBundleAndRemovesLegacyLock
  TeagramUpdateDowngradeKeepsNewerPreviousAndRelaunchesIt
  TeagramUpdateDataCopyKeepsOriginalFolder
  TeagramUpdateShutdownInstallCopiesFinalWrites
  TeagramUpdateInstallerKeepsCustomIcon
  TeagramUpdateReplacementRetainsPreviousThroughStartup
  TeagramUpdateReplacementRestoresMissingTarget
  TeagramUpdateDownloadFailsWithoutMutation
  TeagramUpdateRejectsUntrustedOrIneligiblePackage
  TeagramUpdateInstallerRejectsUnwritableTargetWithoutMutation
  TeagramUpdateInstallerRejectsUnsupportedTargetWithoutMutation
  TeagramUpdateWorkerShutdownCleansCandidateArchive
  TeagramUpdateManagerInstallsProducerBundle
)
for fixture in "${fixtures[@]}"; do
  if ! grep -Fq "TEST_CASE($fixture)" \
    Telegram/SourceFiles/tests/unit/teagram_update_download_tests.cpp \
    Telegram/SourceFiles/tests/unit/teagram_update_verification_tests.cpp; then
    contract_fail "Updater fixture runner names a missing test case: $fixture"
  fi
done

mkdir -p "$temp_dir/arm64-bin"
cat > "$temp_dir/arm64-bin/uname" <<'MOCK_UNAME'
#!/usr/bin/env bash
printf 'arm64\n'
MOCK_UNAME
cat > "$temp_dir/test_unit" <<'MOCK_UNIT'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$MOCK_UNIT_CALLS"
fixture="$2"
head -c 70000 /dev/zero | tr '\0' 'x'
printf '\nrun  %s\n' "$fixture"
if [[ "$fixture" == "${MOCK_FAILING_FIXTURE:-}" ]]; then
  printf 'fail %s\n1 case(s), 1 failed, 1 failed check(s).\n' "$fixture"
  exit 1
fi
printf 'ok   %s\n1 case(s), 0 failed, 0 failed check(s).\n' "$fixture"
MOCK_UNIT
chmod +x "$temp_dir/arm64-bin/uname" "$temp_dir/test_unit"
run_updater_fixtures() {
  rm -rf "$temp_dir/fixture-logs"
  : > "$temp_dir/unit-calls.txt"
  env PATH="$temp_dir/arm64-bin:$PATH" MOCK_UNIT_CALLS="$temp_dir/unit-calls.txt" \
    MOCK_FAILING_FIXTURE="${1:-}" GITHUB_STEP_SUMMARY= \
    bash "$updater_runner" "$temp_dir/test_unit" "$temp_dir/fixture-logs" > /dev/null 2>&1
}
run_updater_fixtures || contract_fail 'The updater fixture runner must pass when every case passes.'
expect_equal 'Updater fixture runner test_unit invocations' \
  "$(sort "$temp_dir/unit-calls.txt")" \
  "$(printf -- '--case %s\n' "${fixtures[@]}" | sort)"
for fixture in "${fixtures[@]}"; do
  log_bytes="$(wc -c < "$temp_dir/fixture-logs/$fixture.log")"
  ((log_bytes <= 65536)) || contract_fail "Updater fixture log for $fixture must stay within 65536 bytes."
done
grep -Fxq 'fixture_timeout_seconds=300' "$temp_dir/fixture-logs/results.txt" \
  || contract_fail 'The updater fixture runner must give each case a 300-second timeout.'
if run_updater_fixtures TeagramUpdateInstallerKeepsCustomIcon; then
  contract_fail 'The updater fixture runner must fail when one case fails.'
fi
grep -Fxq 'TeagramUpdateInstallerKeepsCustomIcon result=failed exit=1 timeout_seconds=300 log_bytes<=65536' \
  "$temp_dir/fixture-logs/results.txt" \
  || contract_fail 'The updater fixture runner must record the failing case.'

bash Telegram/build/test_mac_lifecycle_case_runner.sh

# A case that outlives its timeout is stopped and reported as exit 124.
fixture_launcher="$temp_dir/run_fixture.py"
sed -n "/<<'PY'$/,/^PY$/p" "$updater_runner" | sed '1d;$d' > "$fixture_launcher"
cat > "$temp_dir/slow_unit" <<'MOCK_SLOW'
#!/usr/bin/env bash
exec sleep 30
MOCK_SLOW
chmod +x "$temp_dir/slow_unit"
set +e
python3 "$fixture_launcher" "$temp_dir/slow_unit" SlowFixture 1 > /dev/null 2>&1
launcher_status=$?
set -e
expect_equal 'Updater fixture launcher status for a timed-out case' "$launcher_status" '124'

python3 .github/scripts/test_macos_launch_smoke.py
python3 .github/scripts/test_mac_profile_fixture.py

printf 'macOS CI contract tests passed.\n'
