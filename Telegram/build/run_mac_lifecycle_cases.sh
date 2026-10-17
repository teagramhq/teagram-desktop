#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 4 ]]; then
	echo "usage: $0 <case runner> <executable> <log directory> <case>..." >&2
	exit 2
fi

CASE_RUNNER="$1"
EXECUTABLE="$2"
LOG_DIRECTORY="$3"
CASE_TIMEOUT_SECONDS="${TDESKTOP_MAC_LIFECYCLE_CASE_TIMEOUT_SECONDS:-1200}"
shift 3

if ! [[ "$CASE_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]]; then
	echo "case timeout must be a positive integer: $CASE_TIMEOUT_SECONDS" >&2
	exit 2
fi

mkdir -p "$LOG_DIRECTORY"
CASES=("$@")
FAILED_CASES=()
for CASE_NAME in "${CASES[@]}"; do
	if ! [[ "$CASE_NAME" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]]; then
		echo "case name must contain lowercase letters, digits, and single hyphens: $CASE_NAME" >&2
		exit 2
	fi
done

for ((case_index = 0; case_index < ${#CASES[@]}; ++case_index)); do
	CASE_NAME="${CASES[$case_index]}"
	CASE_LOG="$LOG_DIRECTORY/$CASE_NAME.log"
	: > "$CASE_LOG"
	printf 'lifecycle_case=%s result=START\n' "$CASE_NAME"
	if python3 - "$CASE_RUNNER" "$EXECUTABLE" "$CASE_NAME" \
		"$CASE_LOG" "$CASE_TIMEOUT_SECONDS" 2>>"$CASE_LOG" <<'PY'
import os
import signal
import subprocess
import sys

runner, executable, case_name, log_path, timeout = sys.argv[1:]
timeout = int(timeout)
with open(log_path, "wb") as log:
    process = subprocess.Popen(
        ["bash", runner, executable, case_name],
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        status = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        except OSError as error:
            log.write(f"case timeout TERM failed: {error}\n".encode())
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except OSError as error:
                log.write(f"case timeout KILL failed: {error}\n".encode())
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                log.write(
                    f"lifecycle_case_timeout={case_name} "
                    "process_group_stopped=0\n".encode()
                )
                sys.exit(125)
        else:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except OSError as error:
                log.write(f"case timeout final cleanup failed: {error}\n".encode())
        log.write(
            f"lifecycle_case_timeout={case_name} "
            f"timeout_seconds={timeout} process_group_stopped=1\n".encode()
        )
        sys.exit(124)
sys.exit(status if status >= 0 else 128 - status)
PY
	then
		cat "$CASE_LOG"
		printf 'lifecycle_case=%s result=PASS\n' "$CASE_NAME"
	else
		CASE_STATUS=$?
		cat "$CASE_LOG" >&2
		printf 'lifecycle_case=%s result=FAIL exit_status=%s\n' \
			"$CASE_NAME" "$CASE_STATUS"
		FAILED_CASES+=("$CASE_NAME")
		if (( CASE_STATUS == 125 )); then
			printf 'Lifecycle runner stopped in case %s because its process group remained active.\n' \
				"$CASE_NAME" >&2
			for ((skipped_index = case_index + 1; skipped_index < ${#CASES[@]}; ++skipped_index)); do
				printf 'lifecycle_case=%s result=SKIPPED dependency=%s process_group_stopped=0\n' \
					"${CASES[$skipped_index]}" "$CASE_NAME" >&2
			done
			break
		fi
	fi
done

if (( ${#FAILED_CASES[@]} > 0 )); then
	printf 'Lifecycle cases failed: %s\n' "${FAILED_CASES[*]}" >&2
	exit 1
fi

printf 'Lifecycle cases passed: %s\n' "${#CASES[@]}"
