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

require_contract_tests_in_lint
for trigger in push pull_request; do
  paths="$(workflow_field .github/workflows/workflow_lint.yml ".on.$trigger.paths[]")"
  for script_path in \
    .github/scripts/test_macos_launch_smoke.py \
    .github/scripts/extract_dwarf_subprogram_address.py; do
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
  "\${{ github.event.label.name || '' }}"
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
hash_expression="\${{ hashFiles('CCACHE_KEY.txt') }}"
profile_expression="\${{ steps.ccache-profile.outputs.name }}"
require_step_field "$mac_workflow" build-macos 'Teagram compiler cache.' '.uses | sub("@.*", "")' \
  'actions/cache/restore'
require_step_field "$mac_workflow" build-macos 'Teagram compiler cache.' '.with.key' \
  "macOS-arm64-ccache-dev-${profile_expression}-${hash_expression}-\${{ github.run_id }}-\${{ github.run_attempt }}"
require_step_field "$mac_workflow" build-macos 'Teagram compiler cache.' '.with["restore-keys"]' \
  "macOS-arm64-ccache-dev-${profile_expression}-${hash_expression}-
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
require_step_field "$mac_workflow" build-macos "$regression_step" '.timeout-minutes' '20'
require_step_script "$mac_workflow" build-macos "$regression_step" \
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
# The app bounds and cleanup live in the Python the step runs, so they are
# read from its syntax tree: a commented-out call does not count.
require_step_python "$mac_workflow" build-macos "$regression_step" \
  'app_startup_timeout = 300' \
  'app_runtime_timeout = 180' \
  "signal_pid(app_pid, 'Teagram', cleanup_signal)"
require_step_script "$mac_workflow" build-macos "$regression_step" \
  'process handle SIGUSR2 -p true -s false -n false'
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
  'test_mac_profile_guard.sh'
profile_harness='Telegram/build/test_mac_profile_guard.sh'
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
for cache_evidence_field in \
  queued_ops_before_ready \
  queued_ops_passed \
  cache_usable_ns \
  under_10s; do
  grep -Fq "$cache_evidence_field" "$profile_harness" \
    || contract_fail "$profile_harness must match cache evidence field $cache_evidence_field."
done
grep -Fq 'run_mac_lifecycle_cases.sh' "$profile_harness" \
  || contract_fail "$profile_harness must use the case runner."
for case_name in \
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
grep -Fq 'bash "$PROFILE_GUARD" "$FAKE_APP"' \
  Telegram/build/test_mac_lifecycle_case_runner.sh \
  || contract_fail 'The lifecycle runner self-test must invoke the profile harness without a case argument.'
native_harness='Telegram/SourceFiles/tests/account_lifecycle_regression.cpp'
mac_platform='Telegram/SourceFiles/platform/mac/specific_mac.mm'
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
done
awk '
  /^bool IsApplicationActiveForRegression\(\)/ { in_check = 1 }
  in_check && /return \[NSApp isActive\]/ { reads_appkit = 1 }
  in_check && /^}/ { exit }
  END { exit !reads_appkit }
' "$mac_platform" \
  || contract_fail "$mac_platform must expose the current AppKit application activation state."
grep -Fq 'WindowActivationForRegressionResult::AppInactive' "$native_harness" \
  || contract_fail "$native_harness must classify focus checks as unavailable when AppKit has no active application."
grep -Fq 'AppKit app became inactive before focus verification.' "$native_harness" \
  || contract_fail "$native_harness must use timeout AppKit state to decide whether focus could be verified."
awk '
  /ActivateWindowForRegression\(Core::Application/ { in_activation = 1 }
  in_activation && /if \(!activated\)/ { activation_timeout = NR }
  in_activation && /window->activate\(\)/ && !activation_call { activation_call = NR }
  in_activation && /if \(!Platform::IsApplicationActiveForRegression\(\)\)/ {
    if (activation_timeout) {
      timeout_appkit_check = NR
    } else {
      pre_attempt_appkit_check = NR
    }
  }
  in_activation && /return WindowActivationForRegressionResult::AppInactive/ {
    if (!activation_call || NR < activation_call) inactive_return_before_attempt = 1
  }
  in_activation && /^}/ { exit }
  END {
    exit !(activation_call && activation_timeout > activation_call &&
      !pre_attempt_appkit_check && timeout_appkit_check > activation_timeout &&
      !inactive_return_before_attempt)
  }
' "$native_harness" \
  || contract_fail "$native_harness must issue activation before classifying AppKit inactivity at timeout."
grep -Fq 'Settings deep link native focus assertion skipped:' "$native_harness" \
  || contract_fail "$native_harness must report when the synthetic macOS replay cannot assert native focus."
grep -Fq 'ordinaryFocusBehaviorMatches' "$native_harness" \
  || contract_fail "$native_harness must retain the ordinary Settings focus assertion whenever AppKit is active."
awk '
  /RunSettingsDeepLinkRefusalRegression\(/ { in_settings = 1 }
  in_settings && /ordinaryWindowActive/ { ordinary_window_active = NR }
  in_settings && /activateSettingsWindow\(&pinnedController->window\(\)\)/ {
    custom_window_activations++
    if (custom_window_activations == 1) {
      custom_window_activation = NR
    } else if (custom_window_activations == 2) {
      unsupported_route_activation = NR
    }
  }
  in_settings && /pinnedController, activeBeforePinnedMatrix/ {
    custom_window_matrices++
    custom_window_matrix = NR
  }
  in_settings && /blockedController,/ {
    blocked_window_matrix = NR
  }
  in_settings && !unsupported_route_checks && /const auto minimumLayerWidth/ {
    unsupported_route_checks = NR
  }
  in_settings && /^\[\[nodiscard\]\] HistoryItem/ { exit }
  END {
    exit !(custom_window_activations == 2 && custom_window_matrices == 1 &&
      custom_window_activation < custom_window_matrix &&
      custom_window_matrix < blocked_window_matrix &&
      blocked_window_matrix < unsupported_route_activation &&
      unsupported_route_activation < unsupported_route_checks &&
      unsupported_route_checks < ordinary_window_active)
  }
' "$native_harness" \
  || contract_fail 'Settings deep-link checks must keep the base activation check and run the custom-active URL matrix once.'
awk '
  /ActivateWindowForRegression\(Core::Application/ { in_activation = 1 }
  in_activation && /if \(!activated\)/ { activation_timeout = NR }
  in_activation && /LogWindowActivationForRegression\(app, window\)/ {
    activation_diagnostics = NR
  }
  in_activation && /WindowActivationForRegressionResult::Failed/ { exit }
  END { exit !(activation_timeout && activation_diagnostics > activation_timeout) }
' "$native_harness" \
  || contract_fail 'Activation timeouts must log the target Qt and AppKit window state.'
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
sed 's/^\( *\)python3 - "$executable"/\1echo python3 - "$executable"/' "$mac_workflow" > "$inert_mutation"
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
