#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  printf 'Usage: %s <test_unit> <log-directory>\n' "$0" >&2
  exit 2
fi

unit_binary="$1"
log_directory="$2"
max_log_bytes=65536
fixture_timeout_seconds=300
fixtures=(
  TeagramUpdateArchiveWritesStayConfined
  TeagramUpdateInstallFailurePreservesApp
  TeagramUpdateMarkerRejectsHardLinkWithoutTruncatingCanary
  TeagramUpdateInstallLocationAllowsSharedWritableParent
  TeagramUpdateReplacementSwapsInSharedWritableParent
  TeagramUpdateInstallLocationAllowsGroupWritableApplications
  TeagramUpdateInstallerExercisesAuthenticatedCandidateAndFallback
  TeagramUpdateInstallerRemovesInterruptedExtraction
  TeagramUpdateInstallerClearsUncommittedCandidateOnRestart
  TeagramUpdateStartupKeepsOlderPreviousBundle
  TeagramUpdateDowngradeKeepsNewerPreviousAndRelaunchesIt
  TeagramUpdateDataCopyKeepsOriginalFolder
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

mkdir -p "$log_directory"
results="$log_directory/results.txt"
architecture="$(uname -m)"
tested_sha="${TESTED_SHA:-unknown}"
failed=0

{
  printf 'tested_sha=%s\n' "$tested_sha"
  printf 'architecture=%s\n' "$architecture"
  printf 'fixture_count=%s\n' "${#fixtures[@]}"
  printf 'fixture_timeout_seconds=%s\n' "$fixture_timeout_seconds"
} >> "$results"

if [[ "$architecture" != arm64 ]]; then
  printf 'Expected arm64 runner, got %s.\n' "$architecture" >&2
  printf 'setup=failed architecture=%s\n' "$architecture" >> "$results"
  exit 1
fi

if [[ ! -x "$unit_binary" ]]; then
  printf 'test_unit is missing or not executable: %s\n' "$unit_binary" >&2
  printf 'setup=failed test_unit=missing\n' >> "$results"
  exit 1
fi

run_fixture() {
  python3 - "$unit_binary" "$fixture" "$fixture_timeout_seconds" <<'PY'
import subprocess
import sys

unit_binary, fixture, timeout_seconds = sys.argv[1:]
try:
    result = subprocess.run(
        [unit_binary, "--case", fixture],
        timeout=int(timeout_seconds),
    )
except subprocess.TimeoutExpired:
    print(
        f"{fixture} exceeded its {timeout_seconds}-second timeout.",
        file=sys.stderr,
    )
    sys.exit(124)
sys.exit(result.returncode)
PY
}

for fixture in "${fixtures[@]}"; do
  log_file="$log_directory/$fixture.log"
  printf 'Running %s\n' "$fixture"

  set +e
  run_fixture 2>&1 \
    | tail -c "$max_log_bytes" > "$log_file"
  pipeline_status=("${PIPESTATUS[@]}")
  set -e
  test_status="${pipeline_status[0]}"
  tail_status="${pipeline_status[1]}"

  run_count="$(grep -Fxc "run  $fixture" "$log_file" || true)"
  pass_count="$(grep -Fxc "ok   $fixture" "$log_file" || true)"
  summary_count="$(grep -Fxc \
    '1 case(s), 0 failed, 0 failed check(s).' "$log_file" || true)"

  if [[ "$test_status" == 0 && "$tail_status" == 0 \
      && "$run_count" == 1 && "$pass_count" == 1 \
      && "$summary_count" == 1 ]]; then
    result=passed
  else
    result=failed
    failed=1
  fi

  printf '%s result=%s exit=%s timeout_seconds=%s log_bytes<=%s\n' \
    "$fixture" "$result" "$test_status" "$fixture_timeout_seconds" "$max_log_bytes" \
    | tee -a "$results"
done

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  {
    printf '## macOS updater fixtures\n\n'
    printf -- '- Tested PR merge ref: `%s`\n' "$tested_sha"
    printf -- '- Runner architecture: `%s`\n' "$architecture"
    printf -- '- Per-case timeout: %s seconds.\n' "$fixture_timeout_seconds"
    printf -- '- Fixture logs: at most %s bytes each.\n\n' "$max_log_bytes"
    printf '```text\n'
    tail -n "${#fixtures[@]}" "$results"
    printf '```\n'
  } >> "$GITHUB_STEP_SUMMARY"
fi

if ((failed)); then
  printf 'One or more updater fixtures failed; see the bounded per-case logs.\n' >&2
  exit 1
fi
