#!/usr/bin/env bash
# Assertions over parsed workflow YAML for the CI contract tests. Source it.
# Fields are read with mikefarah yq v4, which GitHub-hosted runners ship.
# Comments are never part of a parsed value; shell comment lines are also
# stripped from `run:` bodies before they are searched.

contract_fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

if ! yq --version 2>/dev/null | grep -Fq 'mikefarah/yq'; then
  contract_fail 'The CI contract tests need mikefarah yq v4 on PATH.'
fi

# Collapses whitespace runs so folded `if:` expressions compare as written.
contract_squash() {
  tr -s '[:space:]' ' ' <<< "$1" | sed 's/^ //; s/ $//'
}

# Prints FIELD (a yq expression such as `.if` or `.with.key`) of the workflow,
# of job JOB, or of the single step named STEP in JOB. Missing values print
# `null`.
workflow_field() {
  local file="$1" field="$2"
  yq -r "$field" "$file"
}

job_field() {
  local file="$1" job="$2" field="$3"
  if [[ "$(JOB="$job" yq -r '.jobs | has(strenv(JOB))' "$file")" != true ]]; then
    contract_fail "$file has no job $job."
  fi
  JOB="$job" yq -r ".jobs[strenv(JOB)] | $field" "$file"
}

step_field() {
  local file="$1" job="$2" step="$3" field="$4"
  local count
  count="$(JOB="$job" STEP="$step" yq -r \
    '[.jobs[strenv(JOB)].steps[] | select(.name == strenv(STEP))] | length' "$file")"
  if [[ "$count" != 1 ]]; then
    contract_fail "$file job $job must have exactly one step named '$step' (found $count)."
  fi
  JOB="$job" STEP="$step" yq -r \
    ".jobs[strenv(JOB)].steps[] | select(.name == strenv(STEP)) | $field" "$file"
}

# Prints a shell body as bash itself parses it, without comments: the body is
# defined (never run) as a function and printed back with `declare -f`. Bash
# reformats whitespace and quoting, so compare with contract_compact. Fails
# when the body does not parse cleanly on its own, as GitHub runs it. That
# check, which executes nothing, comes first: only a complete body cannot
# close the wrapper early and leave commands at the top level of the eval.
strip_shell_comments() {
  local body parse_errors
  body="$(cat)"
  if ! parse_errors="$(bash -n <<< "$body" 2>&1)" || [[ -n "$parse_errors" ]]; then
    printf '%s\n' "$parse_errors" >&2
    return 1
  fi
  bash -c 'eval "__contract_body() {
$1
}" && declare -f __contract_body | sed "1,2d;\$d"' contract "$body"
}

# Drops all whitespace, for comparing text against a bash-printed body.
contract_compact() {
  tr -d '[:space:]' <<< "$1"
}

# The step's `run:` body as bash parses it, without comments.
step_script() {
  local body
  body="$(step_field "$1" "$2" "$3" '.run')"
  strip_shell_comments <<< "$body" \
    || contract_fail "$1 job $2 step '$3' run does not parse as bash."
}

# Prints a job's or the workflow's (JOB empty) token permissions as sorted
# `scope=level` pairs joined by commas, or `inherit` when the job sets none.
permissions_of() {
  local file="$1" job="$2" expr
  expr='if . == null then "inherit" else (to_entries | map(.key + "=" + .value) | sort | join(",")) end'
  if [[ -z "$job" ]]; then
    yq -r ".permissions | $expr" "$file"
  else
    job_field "$file" "$job" ".permissions | $expr"
  fi
}

# Every string value in the workflow, comments excluded.
workflow_strings() {
  yq -r '.. | select(tag == "!!str")' "$1"
}

# Lists `job / step` for every step that can write the Actions cache.
cache_writers() {
  yq -r '.jobs | to_entries[] | .key as $job | .value.steps[]
    | select(.uses // "" | test("^actions/cache(/save)?@"))
    | $job + " / " + .name' "$1"
}

expect_equal() {
  local where="$1" actual="$2" expected="$3"
  if [[ "$actual" != "$expected" ]]; then
    contract_fail "$where must be: $expected
got: $actual"
  fi
}

expect_contains() {
  local where="$1" haystack needle
  haystack="$(contract_squash "$2")"
  shift 2
  for needle in "$@"; do
    if [[ "$haystack" != *"$(contract_squash "$needle")"* ]]; then
      contract_fail "$where must contain: $needle"
    fi
  done
}

expect_lacks() {
  local where="$1" haystack needle
  haystack="$(contract_squash "$2")"
  shift 2
  for needle in "$@"; do
    if [[ "$haystack" == *"$(contract_squash "$needle")"* ]]; then
      contract_fail "$where must not contain: $needle"
    fi
  done
}

require_step_field() {
  local file="$1" job="$2" step="$3" field="$4" expected="$5"
  expect_equal "$file job $job step '$step' $field" \
    "$(step_field "$file" "$job" "$step" "$field")" "$expected"
}

# Whitespace-free containment, for a body printed by strip_shell_comments.
expect_script_contains() {
  local where="$1" haystack needle
  haystack="$(contract_compact "$2")"
  shift 2
  for needle in "$@"; do
    if [[ "$haystack" != *"$(contract_compact "$needle")"* ]]; then
      contract_fail "$where must contain: $needle"
    fi
  done
}

expect_script_lacks() {
  local where="$1" haystack needle
  haystack="$(contract_compact "$2")"
  shift 2
  for needle in "$@"; do
    if [[ "$haystack" == *"$(contract_compact "$needle")"* ]]; then
      contract_fail "$where must not contain: $needle"
    fi
  done
}

require_step_script() {
  local file="$1" job="$2" step="$3" script
  shift 3
  script="$(step_script "$file" "$job" "$step")" || exit 1
  expect_script_contains "$file job $job step '$step' run" "$script" "$@"
}

reject_step_script() {
  local file="$1" job="$2" step="$3" script
  shift 3
  script="$(step_script "$file" "$job" "$step")" || exit 1
  expect_script_lacks "$file job $job step '$step' run" "$script" "$@"
}

require_job_field() {
  local file="$1" job="$2" field="$3" expected="$4"
  expect_equal "$file job $job $field" "$(job_field "$file" "$job" "$field")" "$expected"
}

require_workflow_field() {
  local file="$1" field="$2" expected="$3"
  expect_equal "$file $field" "$(workflow_field "$file" "$field")" "$expected"
}

require_permissions() {
  local file="$1" job="$2" expected="$3"
  expect_equal "$file ${job:+job $job }permissions" \
    "$(permissions_of "$file" "$job")" "$expected"
}

# Pins the token permissions of the workflow and of every job it has, one
# `name: permissions` line each (workflow first, then jobs in file order), so
# a new job cannot take a scope unnoticed.
require_permission_map() {
  local file="$1" expected="$2" actual job
  actual="workflow: $(permissions_of "$file" '')"
  while IFS= read -r job; do
    actual+=$'\n'"$job: $(permissions_of "$file" "$job")"
  done < <(yq -r '.jobs | keys | .[]' "$file")
  expect_equal "$file permissions" "$actual" "$expected"
}

require_triggers() {
  local file="$1" expected="$2"
  require_workflow_field "$file" '.on | keys | sort | join(",")' "$expected"
}

# A removed defect stays removed: no value anywhere in the workflow holds it.
reject_value() {
  local file="$1"
  shift
  expect_lacks "$file values" "$(workflow_strings "$file")" "$@"
}

# Each contract test checks that Workflow lint. still runs both of them, so
# removing either step fails the other.
require_contract_tests_in_lint() {
  local lint=".github/workflows/workflow_lint.yml" test_script command step
  for test_script in \
    test_macos_ci_contract.sh \
    test_ci_cache_contract.sh \
    test_macos_libraries_image.sh; do
    command="bash .github/scripts/$test_script"
    step="$(COMMAND="$command" yq -r '.jobs["lint-workflows"].steps[]
      | select(.run == strenv(COMMAND)) | .name' "$lint")"
    if [[ -z "$step" || "$step" == *$'\n'* ]]; then
      contract_fail "$lint job lint-workflows must run '$command' in exactly one step."
    fi
    expect_equal "$lint job lint-workflows step '$step' if and continue-on-error" \
      "$(step_field "$lint" lint-workflows "$step" '[.if, .["continue-on-error"]] | map(tostring) | join(",")')" \
      'null,null'
  done
  expect_equal "$lint job lint-workflows if" "$(job_field "$lint" lint-workflows '.if')" 'null'
  require_triggers "$lint" 'pull_request,push,workflow_dispatch'
}

# Prints the Python that a step feeds as stdin to `python3` through quoted
# heredocs, as Python parses it: one line per call and per assignment (see
# workflow_python_heredocs.py). Fails when that Python does not parse.
step_python_statements() {
  step_field "$1" "$2" "$3" '.run' \
    | python3 -I "$(dirname -- "${BASH_SOURCE[0]}")/workflow_python_heredocs.py"
}

require_step_python() {
  local file="$1" job="$2" step="$3" statements statement
  shift 3
  statements="$(step_python_statements "$file" "$job" "$step")" \
    || contract_fail "$file job $job step '$step' has Python that does not parse."
  for statement in "$@"; do
    grep -Fxq -- "$statement" <<< "$statements" \
      || contract_fail "$file job $job step '$step' Python must run: $statement"
  done
}
