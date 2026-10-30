#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=workflow_contract.sh
source "$scripts_dir/workflow_contract.sh"

linux_workflow=".github/workflows/linux.yml"
linux_arm64_workflow=".github/workflows/linux-arm64.yml"
cache_cleanup_workflow=".github/workflows/pr_cache_cleanup.yml"
workflow_lint_workflow=".github/workflows/workflow_lint.yml"
unit_feedback_workflow="${UNIT_FEEDBACK_WORKFLOW:-.github/workflows/unit_feedback.yml}"
macos_unit_feedback_workflow="${MACOS_UNIT_FEEDBACK_WORKFLOW:-.github/workflows/macos_unit_feedback.yml}"

temp_dir="$(mktemp -d)"
trap 'rm -rf "$temp_dir"' EXIT

require_contract_tests_in_lint

while IFS=$'\t' read -r workflow job expected_name; do
  require_job_field "$workflow" "$job" '.name' "$expected_name"
done <<'JOB_NAMES'
.github/workflows/cpp_format.yml	lint-cpp-format	lint: cpp-format
.github/workflows/epoch-fixture-toolchain.yml	test-epoch-fixture-conformance	test: epoch-fixture-conformance
.github/workflows/linux-arm64.yml	build-linux	build: linux (arm64)
.github/workflows/linux.yml	check-linux-changes	check: linux-changes
.github/workflows/linux.yml	publish-linux-deps-image	publish: linux-deps-image
.github/workflows/linux.yml	test-linux-unit	test: linux-unit (${{ matrix.defines || 'default' }})
.github/workflows/linux.yml	check-linux	Rocky Linux 8
.github/workflows/linux.yml	ci	ci
.github/workflows/mac.yml	check-macos-deps-artifact	check: macos-deps-artifact
.github/workflows/mac.yml	build-macos-deps-artifact	build: macos-deps-artifact
.github/workflows/mac.yml	ops-prune-macos-libraries	ops: prune-macos-libraries
.github/workflows/mac.yml	build-macos	MacOS
.github/workflows/mac.yml	test-libiconv	test: libiconv (macos-arm64)
.github/workflows/mac_packaged.yml	build-macos-packaged	MacOS
.github/workflows/mac_packaged.yml	publish-release	publish: release
.github/workflows/mac_pr_approval.yml	ops-clear-macos-approval	ops: clear-macos-approval
.github/workflows/macos_unit_feedback.yml	test-macos-unit	test: macos-unit (arm64)
.github/workflows/macos_updater_fixtures.yml	test-updater-fixtures	test: updater-fixtures (macos-arm64)
.github/workflows/pr_cache_cleanup.yml	ops-prune-pr-caches	ops: prune-pr-caches
.github/workflows/pr_cache_cleanup.yml	ops-prune-dev-macos-ccache	ops: prune-dev-macos-ccache
.github/workflows/pr_cache_cleanup.yml	ops-prune-dev-linux-ccache	ops: prune-dev-linux-ccache
.github/workflows/unit_feedback.yml	test-linux-unit-feedback	test: linux-unit-feedback
.github/workflows/workflow_lint.yml	lint-workflows	lint: workflows
JOB_NAMES

dependency_hash="\${{ hashFiles('Telegram/build/docker/centos_env/**') }}"
libraries_image='ghcr.io/teagramhq/teagram-desktop-linux-libraries:${DEPENDENCY_HASH}'

# Linux. publishes the dependency image; nothing pulls centos_env:latest.
for retired_workflow in .github/workflows/docker.yml .github/workflows/linux_libraries.yml; do
  if [[ -e "$retired_workflow" ]]; then
    contract_fail "$retired_workflow must not exist"
  fi
done

# Workflow lint. runs on dev pushes and PRs, and every script it runs (plus the
# helper the contract tests source) is in its path filters, so editing a
# contract test lints it.
require_workflow_field "$workflow_lint_workflow" '.on.push.branches | join(",")' 'dev'
lint_scripts="$(job_field "$workflow_lint_workflow" lint-workflows \
  '.steps[].run // "" | capture("(?P<path>\.github/scripts/[A-Za-z0-9_.]+)") | .path')"
lint_scripts+=$'\n.github/scripts/workflow_contract.sh\n.github/scripts/workflow_python_heredocs.py'
for event in push pull_request; do
  lint_paths="$(workflow_field "$workflow_lint_workflow" ".on.$event.paths[]")"
  while IFS= read -r lint_script; do
    if ! grep -Fxq -- "$lint_script" <<< "$lint_paths"; then
      contract_fail "$workflow_lint_workflow on.$event.paths must list $lint_script"
    fi
  done <<< "$lint_scripts"
done

# Linux. builds the pushed dev commit itself (never from workflow_run), with a
# read-only build token, and only a dev push or schedule saves the compiler
# cache.
require_triggers "$linux_workflow" 'pull_request,push,schedule,workflow_dispatch'
require_workflow_field "$linux_workflow" '.on.push.branches | join(",")' 'dev'
require_workflow_field "$linux_workflow" \
  '.on.push.paths[] | select(. == "Telegram/build/docker/centos_env/**")' \
  'Telegram/build/docker/centos_env/**'
require_workflow_field "$linux_workflow" '.concurrency["cancel-in-progress"]' \
  "\${{ github.ref != 'refs/heads/dev' }}"
reject_value "$linux_workflow" 'workflow_run' 'head_sha'
expect_equal "$linux_workflow checkouts that override the ref" \
  "$(yq -r '.jobs[].steps[] | select(.uses // "" | test("^actions/checkout@")) | select(.with.ref != null) | .name' "$linux_workflow")" ''
require_permission_map "$linux_workflow" 'workflow: contents=read
check-linux-changes: inherit
publish-linux-deps-image: contents=read,packages=write
test-linux-unit: actions=read,contents=read,packages=read
check-linux: inherit
ci: inherit'

# The build verifies it checked out GITHUB_SHA before compiling.
verify_index="$(job_field "$linux_workflow" test-linux-unit \
  '.steps | to_entries[] | select(.value.name == "Verify the checked-out commit.") | .key')"
build_index="$(job_field "$linux_workflow" test-linux-unit \
  '.steps | to_entries[] | select(.value.name == "Telegram Desktop build.") | .key')"
if [[ -z "$verify_index" || -z "$build_index" ]] || ((verify_index > build_index)); then
  contract_fail "$linux_workflow job test-linux-unit must verify the checked-out commit before 'Telegram Desktop build.'"
fi
require_step_field "$linux_workflow" test-linux-unit 'Verify the checked-out commit.' '.run' \
  'bash .github/scripts/verify_built_sha.sh "$GITHUB_SHA"'

require_step_field "$linux_workflow" test-linux-unit 'Set Linux dependency image tag.' \
  '.env.DEPENDENCY_HASH' "$dependency_hash"
require_step_script "$linux_workflow" test-linux-unit 'Set Linux dependency image tag.' \
  "LINUX_LIBRARIES_IMAGE=$libraries_image"
require_step_field "$linux_workflow" test-linux-unit 'Pull shared Linux dependency image.' '.run' \
  'bash .github/scripts/linux_libraries_image.sh pull "$LINUX_LIBRARIES_IMAGE" "$IMAGE_TAG" "$GITHUB_OUTPUT"'
require_step_field "$linux_workflow" test-linux-unit 'Restore Telegram Desktop cache.' '.with.key' \
  "\${{ runner.OS }}-tdesktop-\${{ matrix.defines || 'default' }}-\${{ github.run_id }}"
require_step_field "$linux_workflow" test-linux-unit 'Restore Telegram Desktop cache.' '.with["restore-keys"]' \
  "\${{ runner.OS }}-tdesktop-\${{ matrix.defines || 'default' }}-"
require_step_field "$linux_workflow" test-linux-unit 'Save Telegram Desktop cache.' '.with.key' \
  '${{ steps.cache-tdesktop.outputs.cache-primary-key }}'
for step_name in 'Trim Telegram Desktop cache.' 'Save Telegram Desktop cache.'; do
  expect_equal "$linux_workflow job test-linux-unit step '$step_name' if" \
    "$(contract_squash "$(step_field "$linux_workflow" test-linux-unit "$step_name" '.if')")" \
    "env.ONLY_CACHE == 'false' && github.event_name != 'pull_request' && github.ref == 'refs/heads/dev' && fromJSON(steps.ccache-stats.outputs.misses) > 300"
done
expect_equal "$linux_workflow cache writers" "$(cache_writers "$linux_workflow")" \
  'test-linux-unit / Save Telegram Desktop cache.'
expect_equal "$linux_workflow steps that export a build cache" \
  "$(yq -r '.jobs[].steps[] | select(.with["cache-to"] != null) | .name' "$linux_workflow")" ''
require_step_field "$linux_workflow" test-linux-unit 'Report Linux Libraries duration.' '.if' 'always()'
require_step_field "$linux_workflow" test-linux-unit 'Report Actions cache total.' '.if' 'always()'
require_step_script "$linux_workflow" test-linux-unit 'Report Actions cache total.' \
  '(( total_bytes >= 9000000000 ))'
reject_value "$linux_workflow" 'Linux-libs-' 'buildkit-cache-dance'

require_step_script "$linux_arm64_workflow" build-linux 'Generate Linux dependency image.' \
  $'DOCKERFILE=$(DEBUG= LTO= poetry run gen_dockerfile);\necho "$DOCKERFILE" > Dockerfile'
reject_step_script "$linux_arm64_workflow" build-linux 'Generate Linux dependency image.' \
  'DEBUG= LTO= poetry run gen_dockerfile > Dockerfile'
require_step_script "$linux_arm64_workflow" build-linux \
  'Verify and stage ELF64 aarch64 Teagram executable.' \
  'executable="out/Debug/Teagram"' \
  'echo "::error::Teagram executable was not produced."' \
  'cp "${executable}" artifact/Teagram'
reject_step_script "$linux_arm64_workflow" build-linux \
  'Verify and stage ELF64 aarch64 Teagram executable.' \
  'out/Debug/Telegram' 'artifact/Telegram'
require_step_field "$linux_arm64_workflow" build-linux 'Upload artifact.' '.with.name' \
  'Teagram-Linux-arm64-${{ github.sha }}'

# The publish-linux-deps-image job is the only GHCR writer, only for dev pushes in this
# repository, and the build waits for it and stops when it fails.
require_job_field "$linux_workflow" publish-linux-deps-image '.if' \
  "github.event_name == 'push' && github.ref == 'refs/heads/dev' && github.repository == 'teagramhq/teagram-desktop'"
require_step_field "$linux_workflow" publish-linux-deps-image 'Set Linux dependency image tag.' \
  '.env.DEPENDENCY_HASH' "$dependency_hash"
require_step_script "$linux_workflow" publish-linux-deps-image 'Set Linux dependency image tag.' \
  "image=\"$libraries_image\""
require_step_field "$linux_workflow" publish-linux-deps-image 'Build and publish Linux dependency image.' \
  '.uses | sub("@.*", "")' 'docker/build-push-action'
expect_equal "$linux_workflow steps that push an image" \
  "$(yq -r '.jobs | to_entries[] | .key as $job | .value.steps[] | select(.with.push == true) | $job + " / " + .name' "$linux_workflow")" \
  'publish-linux-deps-image / Build and publish Linux dependency image.'
require_job_field "$linux_workflow" test-linux-unit '.needs | join(",")' \
  'check-linux-changes,publish-linux-deps-image'
expect_contains "$linux_workflow job test-linux-unit if" \
  "$(job_field "$linux_workflow" test-linux-unit '.if')" \
  "(needs.publish-linux-deps-image.result == 'success' || needs.publish-linux-deps-image.result == 'skipped')" \
  "needs.check-linux-changes.outputs.required == 'true'"
expect_equal "$linux_workflow test-linux-unit matrix fail-fast default" \
  "$(job_field "$linux_workflow" test-linux-unit '.strategy.fail-fast')" 'null'

expect_equal "$linux_workflow in-job run cancellation steps" \
  "$(yq -r '[.jobs[] | .steps[]? | select(.name == "Cancel PR workflow run after job failure")] | length' "$linux_workflow")" '0'

# The PR aggregate waits for each job that can run on a pull request and
# rejects failed or cancelled results while allowing only the filtered build
# skip.
pr_jobs="$(yq -r '.jobs | keys | map(select(. != "ci" and . != "publish-linux-deps-image")) | sort | join(",")' "$linux_workflow")"
expect_equal "$linux_workflow PR aggregate dependencies" \
  "$(job_field "$linux_workflow" ci '.needs | sort | join(",")')" \
  "$pr_jobs"
require_job_field "$linux_workflow" check-linux '.if' \
  "\${{ (github.event_name == 'pull_request' && !cancelled()) || (github.event_name != 'pull_request' && always()) }}"
require_job_field "$linux_workflow" ci '.if' \
  "always() && github.event_name == 'pull_request'"

ci_report_script="$temp_dir/ci-report.sh"
step_script "$linux_workflow" ci 'Report CI result.' > "$ci_report_script"
run_ci_report() {
  env CHANGE_RESULT="$1" BUILD_REQUIRED="$2" BUILD_RESULT="$3" \
    LINUX_RESULT="$4" bash "$ci_report_script" > "$temp_dir/ci-report-output" 2>&1
}
expect_ci_report_success() {
  local scenario="$1"
  shift
  if ! run_ci_report "$@"; then
    contract_fail "$linux_workflow ci result reporting must accept $scenario."
  fi
}
expect_ci_report_failure() {
  local scenario="$1"
  shift
  if run_ci_report "$@"; then
    contract_fail "$linux_workflow ci result reporting must reject $scenario."
  fi
}
expect_ci_report_success 'a required successful build' success true success success
expect_ci_report_success 'a path-filtered build skip' success false skipped success
expect_ci_report_failure 'failed change detection' failure false skipped success
expect_ci_report_failure 'cancelled change detection' cancelled false skipped success
expect_ci_report_failure 'a failed Linux check' success false skipped failure
expect_ci_report_failure 'a cancelled Linux check' success false skipped cancelled
expect_ci_report_failure 'a failed required build' success true failure success
expect_ci_report_failure 'a cancelled required build' success true cancelled success
expect_ci_report_failure 'a skipped required build' success true skipped success
expect_ci_report_failure 'a path-filtered build that ran' success false success success
expect_ci_report_failure 'an unknown build requirement' success unknown skipped success

# Prune pull request caches. runs with only actions: write, never checks out
# code, and acts only on a numeric PR number or a trusted dev source run.
require_triggers "$cache_cleanup_workflow" 'pull_request_target,workflow_run'
require_workflow_field "$cache_cleanup_workflow" '.on.pull_request_target.types | join(",")' 'closed'
require_workflow_field "$cache_cleanup_workflow" '.on.workflow_run.workflows | join(",")' 'MacOS.,MacOS Packaged.,Linux.'
require_workflow_field "$cache_cleanup_workflow" '.on.workflow_run.branches | join(",")' 'dev'
require_permission_map "$cache_cleanup_workflow" 'workflow: 
ops-prune-pr-caches: actions=write
ops-prune-dev-macos-ccache: actions=write
ops-prune-dev-linux-ccache: actions=write'
expect_equal "$cache_cleanup_workflow checkout steps" \
  "$(yq -r '.jobs[].steps[] | select(.uses // "" | test("^actions/checkout@")) | .name' "$cache_cleanup_workflow")" ''
require_job_field "$cache_cleanup_workflow" ops-prune-pr-caches '.if' "github.event_name == 'pull_request_target'"
require_job_field "$cache_cleanup_workflow" ops-prune-dev-macos-ccache '.if' \
  "github.event_name == 'workflow_run' && github.event.workflow_run.name != 'Linux.'"
require_job_field "$cache_cleanup_workflow" ops-prune-dev-linux-ccache '.if' \
  "github.event_name == 'workflow_run' && github.event.workflow_run.name == 'Linux.'"

mkdir -p "$temp_dir/bin"
cat > "$temp_dir/bin/gh" <<'MOCK_GH'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$MOCK_GH_CALLS"
MOCK_GH
chmod +x "$temp_dir/bin/gh"
gh_calls="$temp_dir/gh-calls.txt"

prune_pr_script="$temp_dir/prune-pr.sh"
step_field "$cache_cleanup_workflow" ops-prune-pr-caches 'Delete pull request cache scope.' '.run' > "$prune_pr_script"
require_step_field "$cache_cleanup_workflow" ops-prune-pr-caches 'Delete pull request cache scope.' \
  '.env.PR_NUMBER' '${{ github.event.pull_request.number }}'
run_prune_pr() {
  : > "$gh_calls"
  env PATH="$temp_dir/bin:$PATH" MOCK_GH_CALLS="$gh_calls" \
    GITHUB_REPOSITORY=teagramhq/teagram-desktop PR_NUMBER="$1" \
    bash "$prune_pr_script" > /dev/null 2>&1
}
run_prune_pr 42 || contract_fail 'Pruning a numeric PR cache scope must succeed.'
expect_equal 'PR cache prune gh calls' "$(cat "$gh_calls")" \
  'cache delete --all --succeed-on-no-caches --ref refs/pull/42/merge --repo teagramhq/teagram-desktop'
for malformed in '' '42/head' '42 --ref refs/heads/dev' '-1'; do
  if run_prune_pr "$malformed"; then
    contract_fail "Pruning must refuse PR number '$malformed'."
  fi
  [[ ! -s "$gh_calls" ]] || contract_fail "A malformed PR number '$malformed' must not reach gh."
done

# Every job a workflow_run can start, in any workflow, first verifies that the
# source run is a dev run from this repository. A job is exempt only when its
# `if` pins another event.
trust_step='Verify the source run is trusted.'
# Prints how many jobs in DIR's workflows (.yml and .yaml) a workflow_run can
# start; each must pass the trust checks.
count_verified_workflow_run_jobs() {
  local workflow_dir="$1" workflow job job_if trust_script workflow_run_jobs=0
  for workflow in "$workflow_dir"/*.yml "$workflow_dir"/*.yaml; do
    [[ -f "$workflow" ]] || continue
    [[ "$(yq -r '.on | has("workflow_run")' "$workflow")" == true ]] || continue
    while IFS= read -r job; do
      job_if="$(job_field "$workflow" "$job" '.if // ""')"
      if [[ "$job_if" =~ ^github\.event_name\ ==\ \'([a-z_]+)\'$ \
          && "${BASH_REMATCH[1]}" != workflow_run ]]; then
        continue
      fi
      workflow_run_jobs=$((workflow_run_jobs + 1))
      require_job_field "$workflow" "$job" '.steps[0].name' "$trust_step"
      # The trust step always runs, and its failure stops every later step.
      require_job_field "$workflow" "$job" '.["continue-on-error"]' 'null'
      expect_equal "$workflow job $job steps with if or continue-on-error" \
        "$(job_field "$workflow" "$job" \
          '.steps[] | select(.if != null or .["continue-on-error"] != null) | .name')" ''
      require_step_field "$workflow" "$job" "$trust_step" '.env.SOURCE_BRANCH' \
        '${{ github.event.workflow_run.head_branch }}'
      require_step_field "$workflow" "$job" "$trust_step" '.env.SOURCE_REPOSITORY' \
        '${{ github.event.workflow_run.head_repository.full_name }}'
      trust_script="$temp_dir/$job-trust.sh"
      step_field "$workflow" "$job" "$trust_step" '.run' > "$trust_script"
      run_trust() {
        env GITHUB_REPOSITORY=teagramhq/teagram-desktop SOURCE_BRANCH="$1" SOURCE_REPOSITORY="$2" \
          bash "$trust_script" > /dev/null 2>&1
      }
      run_trust dev teagramhq/teagram-desktop \
        || contract_fail "$workflow job $job must accept a dev run from this repository."
      if run_trust feature teagramhq/teagram-desktop; then
        contract_fail "$workflow job $job must refuse a non-dev source run."
      fi
      if run_trust dev steward/tdesktop; then
        contract_fail "$workflow job $job must refuse a fork source run."
      fi
    done < <(yq -r '.jobs | keys | .[]' "$workflow")
  done
  printf '%s\n' "$workflow_run_jobs"
}
expect_equal 'Jobs started by workflow_run with a verified source' \
  "$(count_verified_workflow_run_jobs .github/workflows)" '2'
mkdir -p "$temp_dir/yaml-workflows"
cat > "$temp_dir/yaml-workflows/untrusted.yaml" <<'UNTRUSTED'
on:
  workflow_run:
    workflows: ["Linux."]
    types: [completed]
jobs:
  act:
    runs-on: ubuntu-26.04
    steps:
      - run: true
UNTRUSTED
if (count_verified_workflow_run_jobs "$temp_dir/yaml-workflows") > /dev/null 2>&1; then
  contract_fail 'An unguarded workflow_run job in a .yaml workflow must fail the trust checks.'
fi
for bypass in 'if: false' 'continue-on-error: true'; do
  bypass_dir="$temp_dir/trust-bypass-${bypass%%:*}"
  mkdir -p "$bypass_dir"
  BYPASS="$bypass" awk '
    { print }
    /^      - name: Verify the source run is trusted\.$/ { print "        " ENVIRON["BYPASS"] }
  ' "$cache_cleanup_workflow" > "$bypass_dir/pr_cache_cleanup.yml"
  if (count_verified_workflow_run_jobs "$bypass_dir") > /dev/null 2>&1; then
    contract_fail "A source-trust step with '$bypass' must fail the trust checks."
  fi
done
require_step_script "$cache_cleanup_workflow" ops-prune-dev-macos-ccache \
  'Keep the newest compiler cache per profile.' \
  "cache_prefix='macOS-arm64-ccache-dev-'" '--ref refs/heads/dev'
reject_value "$cache_cleanup_workflow" 'macOS-arm64-ccache-pr-'

# Feedback lanes run on dev against a selected feature ref, so they carry the
# dev cache scope. The cache service, not skipped steps, keeps them read-only.
for feedback_workflow in "$unit_feedback_workflow" "$macos_unit_feedback_workflow"; do
  require_triggers "$feedback_workflow" 'workflow_dispatch'
  require_workflow_field "$feedback_workflow" '.["cache-mode"]' 'read'
  expect_equal "$feedback_workflow job cache-mode values" \
    "$(yq -r '.jobs[]["cache-mode"] // "read" | select(. != "read" and . != "none")' "$feedback_workflow")" ''
  expect_equal "$feedback_workflow cache writers" "$(cache_writers "$feedback_workflow")" ''
  expect_equal "$feedback_workflow steps that export a build cache" \
    "$(yq -r '.jobs[].steps[] | select(.with["cache-to"] != null) | .name' "$feedback_workflow")" ''
  reject_value "$feedback_workflow" 'type=gha'
done

require_permission_map "$unit_feedback_workflow" 'workflow: contents=read,packages=read
test-linux-unit-feedback: inherit'
expect_equal "$unit_feedback_workflow steps that push an image" \
  "$(yq -r '.jobs[].steps[] | select(.with.push == true) | .name' "$unit_feedback_workflow")" ''
require_step_field "$unit_feedback_workflow" test-linux-unit-feedback 'Set Linux dependency image tag.' \
  '.env.DEPENDENCY_HASH' "$dependency_hash"
require_step_script "$unit_feedback_workflow" test-linux-unit-feedback 'Set Linux dependency image tag.' \
  "LINUX_LIBRARIES_IMAGE=$libraries_image"
require_step_field "$unit_feedback_workflow" test-linux-unit-feedback 'Pull shared Linux dependency image.' '.run' \
  'bash "$RUNNER_TEMP/linux_libraries_image.sh" pull "$LINUX_LIBRARIES_IMAGE" "$IMAGE_TAG" "$GITHUB_OUTPUT"'
require_step_field "$unit_feedback_workflow" test-linux-unit-feedback 'Libraries.' '.if' \
  "steps.pull-libraries.outputs.cache-hit != 'true'"

require_permission_map "$macos_unit_feedback_workflow" 'workflow: contents=read,packages=read
test-macos-unit: inherit'
require_step_script "$macos_unit_feedback_workflow" test-macos-unit 'First set up.' \
  'HOMEBREW_NO_AUTO_UPDATE=1 brew install "${missing[@]}"'
require_step_field "$macos_unit_feedback_workflow" test-macos-unit 'Restore macOS libraries artifact.' '.run' \
  'bash "$REPO_NAME/.github/scripts/macos_libraries_image.sh" pull "$MACOS_LIBRARIES_IMAGE" "$GITHUB_WORKSPACE" "$GITHUB_OUTPUT"'
require_step_field "$macos_unit_feedback_workflow" test-macos-unit 'Restore legacy macOS libraries cache.' '.uses' \
  'actions/cache/restore@v6'
require_step_field "$macos_unit_feedback_workflow" test-macos-unit 'Restore legacy macOS libraries cache.' '.if' \
  "steps.cache-libs.outputs.cache-hit != 'true'"
reject_value "$macos_unit_feedback_workflow" 'brew upgrade' 'brew update' '|| true' 'sed -i'

bash .github/scripts/test_prune_macos_dev_ccache.sh
bash .github/scripts/test_prune_linux_dev_ccache.sh

printf 'CI cache contract tests passed.\n'
