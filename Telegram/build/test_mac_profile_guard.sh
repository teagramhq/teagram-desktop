#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
	echo "usage: $0 <Teagram executable> [case]" >&2
	exit 2
fi

CASE_NAME="${2:-all}"

if [[ "$(uname -s)" != Darwin ]]; then
	echo "macOS profile integration test requires Darwin." >&2
	exit 2
fi
printf 'macos_version=%s architecture=%s\n' \
	"$(sw_vers -productVersion)" "$(uname -m)"

APP="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
APP_BUNDLE="$(cd "$(dirname "$APP")/../.." && pwd -P)"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
TESTED_REVISION="$(git -C "$REPO_ROOT" rev-parse HEAD)"
TEST_TMP_BASE="${TDESKTOP_MAC_PROFILE_TEST_TMP_BASE:-/tmp}"
if [[ ! -x "$APP" ]]; then
	echo "Teagram executable is not executable: $APP" >&2
	exit 2
fi
IPC_DIRECTORY="$(mktemp -d /tmp/tg-ipc.XXXXXX)"
IPC_DIRECTORY="$(cd "$IPC_DIRECTORY" && pwd -P)"
export TDESKTOP_MAC_PROFILE_TEST_IPC="$IPC_DIRECTORY"
TEST_HOME="$(mktemp -d "$TEST_TMP_BASE/teagram-profile-test.XXXXXX")"
TEST_HOME="$(cd "$TEST_HOME" && pwd -P)"
IPC_SEARCH_DIRECTORY="$(cd "$IPC_DIRECTORY" 2>/dev/null && pwd -P || printf '%s' "$IPC_DIRECTORY")"

PROFILE="$TEST_HOME/Library/Application Support/Teagram"
HOSTILE_HOME="$TEST_HOME/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram"
START_LOG="$TEST_HOME/start.log"
PROFILE_ARTIFACT="${TDESKTOP_MAC_PROFILE_TEST_PROFILE_ARTIFACT:-}"
LOCK_SUFFIX="$(printf '%s' "$APP_BUNDLE" | md5 -q | cut -c1-16)"
SOCKET_SUFFIX="$(printf '%s' "$PROFILE" | md5 -q | cut -c1-16)"
LOCK_NAME="Teagram-lock-$LOCK_SUFFIX"
LOCK_PATH="$IPC_DIRECTORY/$LOCK_NAME"
SOCKET_PATH="$IPC_DIRECTORY/Teagram-$SOCKET_SUFFIX"
MAC_SOCKET_PATH_LIMIT=103
SOCKET_PATH_BYTES="$(LC_ALL=C printf '%s' "$SOCKET_PATH" | wc -c | tr -d '[:space:]')"
FIRST_PID=""

find_lock_path() {
	local candidate
	for candidate in "$IPC_SEARCH_DIRECTORY"/"$LOCK_NAME"*; do
		if [[ -e "$candidate" ]]; then
			printf '%s' "$candidate"
			return 0
		fi
	done
	return 0
}

has_start_record() {
	local record="$1"
	grep -F -q "$record" "$START_LOG" \
		|| grep -F -q "$record" "$PROFILE/log.txt" 2>/dev/null
}

report_result() {
	local name="$1"
	local passed="$2"
	local detail="$3"
	if [[ "$passed" == true ]]; then
		printf '%s=PASS %s\n' "$name" "$detail"
	else
		printf '%s=FAIL %s\n' "$name" "$detail" >&2
		return 1
	fi
}

fixture_unchanged() {
	local fixture="$1"
	local is_file="$2"
	local expected_contents="$3"
	if [[ "$is_file" == true ]]; then
		[[ -f "$fixture" ]] && [[ "$(cat "$fixture")" == "$expected_contents" ]]
	else
		[[ -d "$fixture" ]] \
			&& [[ -z "$(find "$fixture" -mindepth 1 -print -quit)" ]]
	fi
}

run_seatbelt_cat_probe() {
	local name="$1"
	local option="$2"
	local home="$3"
	local path="$4"
	local output
	local status
	local expected_errno=EPERM
	if [[ "$option" == --mac-seatbelt-cat-allow-probe ]]; then
		expected_errno=0
	fi
	if [[ ! -f "$path" ]]; then
		echo "$name fixture is missing: $path" >&2
		return 1
	fi
	set +e
	output="$(env HOME="$home" TMPDIR="$TEST_TMP_BASE" LC_ALL=C \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$home" \
		TDESKTOP_MAC_PROFILE_TEST_DIAGNOSTICS=1 \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		"$APP" "$option" "$path" 2>&1)"
	status=$?
	set -e
	if [[ "$status" -ne 0 \
		|| "$output" != "Mac profile IPC selected: variant=non-store directory=$IPC_DIRECTORY" ]]; then
		echo "$name failed: path=$path status=$status" >&2
		printf '%s\n' "$output" >&2
		if [[ "$output" == *"stage=seatbelt"* ]]; then
			if [[ -n "$PROFILE_ARTIFACT" && -s "$PROFILE_ARTIFACT" ]]; then
				local profile_bytes
				profile_bytes="$(wc -c < "$PROFILE_ARTIFACT" | tr -d '[:space:]')"
				printf 'seatbelt_generated_profile_artifact=PASS path=%s bytes=%s\n' \
					"$PROFILE_ARTIFACT" "$profile_bytes" >&2
			else
				echo "Seatbelt compilation failed without a generated-profile artifact." >&2
			fi
		fi
		return 1
	fi
	printf '%s=PASS mode=posix_spawn child=/bin/cat errno=%s path=%s\n' \
		"$name" "$expected_errno" "$path"
}

verify_unsandboxed_canary_readable() {
	local name="$1"
	local path="$2"
	local expected_contents="$3"
	local actual_contents
	if [[ ! -f "$path" ]]; then
		echo "$name unsandboxed fixture is missing: $path" >&2
		return 1
	fi
	actual_contents="$(/bin/cat "$path")" || {
		echo "$name unsandboxed /bin/cat could not read: $path" >&2
		return 1
	}
	if [[ "$actual_contents" != "$expected_contents" ]]; then
		echo "$name unsandboxed control read unexpected contents: $path" >&2
		return 1
	fi
	printf '%s=PASS unsandboxed=/bin/cat readable=1 path=%s\n' "$name" "$path"
}

verify_casefold_directory_alias() {
	local name="$1"
	local canonical_path="$2"
	local alias_path="$3"
	local canonical_id
	local alias_id
	if [[ "$canonical_path" == "$alias_path" ]]; then
		echo "$name canonical and alias paths are identical." >&2
		return 1
	fi
	mkdir -p "$canonical_path" || return 1
	if [[ ! -d "$alias_path" ]]; then
		echo "$name alias does not resolve to a directory: $alias_path" >&2
		return 1
	fi
	canonical_id="$(stat -f '%d:%i' "$canonical_path")" || return 1
	alias_id="$(stat -f '%d:%i' "$alias_path")" || return 1
	if [[ "$canonical_id" != "$alias_id" ]]; then
		echo "$name alias resolved to a different directory: $alias_path" >&2
		return 1
	fi
	printf '%s=PASS canonical=%s alias=%s inode=%s\n' \
		"$name" "$canonical_path" "$alias_path" "$canonical_id"
}

run_home_spelling_probes() {
	local name="$1"
	local profile_home="$2"
	local path_home="$3"
	local protected_relative="${4:-Library/Application Support/Telegram Desktop}"
	local allowed_relative="${5:-Library/Application Support/Teagram/tdata}"
	local unrelated_relative="${6:-}"
	local protected_canary="$path_home/$protected_relative/synthetic-canary"
	local allowed_canary="$path_home/$allowed_relative/synthetic-allowed-file"
	local protected_contents="synthetic $name protected bytes"
	local allowed_contents="synthetic $name allowed Teagram bytes"
	mkdir -p "$(dirname "$protected_canary")" "$(dirname "$allowed_canary")"
	printf '%s' "$protected_contents" > "$protected_canary"
	printf '%s' "$allowed_contents" > "$allowed_canary"
	local unrelated_canary=""
	local unrelated_contents="synthetic $name allowed unrelated bytes"
	if [[ -n "$unrelated_relative" ]]; then
		unrelated_canary="$path_home/$unrelated_relative/synthetic-allowed-file"
		mkdir -p "$(dirname "$unrelated_canary")"
		printf '%s' "$unrelated_contents" > "$unrelated_canary"
	fi
	verify_unsandboxed_canary_readable \
		"${name}_protected_unsandboxed_control" \
		"$protected_canary" "$protected_contents" \
		|| return 1
	verify_unsandboxed_canary_readable \
		"${name}_allowed_unsandboxed_control" \
		"$allowed_canary" "$allowed_contents" \
		|| return 1
	run_seatbelt_cat_probe \
		"${name}_protected_denial" \
		--mac-seatbelt-cat-probe "$profile_home" "$protected_canary" \
		|| return 1
	run_seatbelt_cat_probe \
		"${name}_allowed_teagram_file" \
		--mac-seatbelt-cat-allow-probe "$profile_home" "$allowed_canary" \
		|| return 1
	if [[ -n "$unrelated_canary" ]]; then
		verify_unsandboxed_canary_readable \
			"${name}_unrelated_unsandboxed_control" \
			"$unrelated_canary" "$unrelated_contents" \
			|| return 1
		run_seatbelt_cat_probe \
			"${name}_allowed_unrelated_file" \
			--mac-seatbelt-cat-allow-probe "$profile_home" "$unrelated_canary" \
			|| return 1
	fi
}

run_seatbelt_mode_probe() {
	local name="$1"
	local option="$2"
	local home="$3"
	local path="$4"
	local mode="$5"
	local expected_errno="$6"
	local output
	local status
	set +e
	output="$(env HOME="$home" TMPDIR="$TEST_TMP_BASE" LC_ALL=C \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$home" \
		TDESKTOP_MAC_PROFILE_TEST_DIAGNOSTICS=1 \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		"$APP" "$option" "$path" 2>&1)"
	status=$?
	set -e
	if [[ "$status" -ne 0 \
		|| "$output" != "Mac profile IPC selected: variant=non-store directory=$IPC_DIRECTORY" ]]; then
		echo "$name failed: path=$path status=$status" >&2
		printf '%s\n' "$output" >&2
		return 1
	fi
	printf '%s=PASS mode=%s errno=%s path=%s\n' \
		"$name" "$mode" "$expected_errno" "$path"
}

run_bundle_keyed_probe() {
	local name="$1"
	local parent="$2"
	local canary="$TEST_HOME/Library/$parent/org.telegram.desktop.fixture/synthetic-canary"
	local canary_hash
	mkdir -p "$(dirname "$canary")"
	printf '%s' 'synthetic protected bundle-keyed bytes' > "$canary"
	canary_hash="$(shasum -a 256 "$canary" | awk '{print $1}')"
	run_seatbelt_cat_probe \
		"$name" \
		--mac-seatbelt-cat-probe "$TEST_HOME" "$canary" \
		|| return 1
	if [[ "$(shasum -a 256 "$canary" | awk '{print $1}')" != "$canary_hash" ]]; then
		echo "$name canary changed during the denial probe." >&2
		return 1
	fi
}

run_spoiler_cache_symlink_case() {
	local case_name="$1"
	local link_name="$2"
	local case_home="$TEST_HOME/$case_name"
	local PROFILE="$case_home/Library/Application Support/Teagram"
	local fixture="$case_home/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/SyntheticSpoilerCache"
	local fixture_is_file=false
	local fixture_contents='synthetic protected cache bytes'
	local START_LOG="$case_home/start.log"
	local quit_log="$case_home/quit.log"
	local LOCK_NAME="$LOCK_NAME"
	local IPC_SEARCH_DIRECTORY="$IPC_SEARCH_DIRECTORY"
	local SOCKET_PATH="$IPC_DIRECTORY/Teagram-$(printf '%s' "$PROFILE" | md5 -q | cut -c1-16)"
	local socket_bytes
	local PROFILE_READY=false
	local IPC_SELECTION_READY=false
	local LOCK_READY=false
	local SOCKET_PATH_READY=false
	local SOCKET_LISTENER_READY=false
	local SOCKET_FILE_READY=false
	local FIRST_PROCESS_ALIVE=false
	local FAILURES=0
	local quit_status
	local first_status
	local response
	local refusal

	case "$link_name" in
	emoji)
		mkdir -p "$PROFILE/tdata" "$fixture"
		ln -s "$fixture" "$PROFILE/tdata/emoji"
		;;
	spoiler)
		mkdir -p "$PROFILE/tdata/emoji" "$fixture"
		ln -s "$fixture" "$PROFILE/tdata/emoji/spoiler"
		;;
	text|image)
		mkdir -p "$PROFILE/tdata/emoji/spoiler" "$(dirname "$fixture")"
		printf '%s' "$fixture_contents" > "$fixture"
		ln -s "$fixture" "$PROFILE/tdata/emoji/spoiler/$link_name"
		fixture_is_file=true
		;;
	*)
		echo "unsupported spoiler cache symlink: $link_name" >&2
		return 2
		;;
	esac
	socket_bytes="$(LC_ALL=C printf '%s' "$SOCKET_PATH" | wc -c | tr -d '[:space:]')"
	if (( socket_bytes > MAC_SOCKET_PATH_LIMIT )); then
		echo "socket path is $socket_bytes bytes; macOS sun_path limit is $MAC_SOCKET_PATH_LIMIT: $SOCKET_PATH" >&2
		return 1
	fi

	env HOME="$case_home" TMPDIR="$TEST_TMP_BASE" \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$case_home" \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		"$APP" >"$START_LOG" 2>&1 &
	FIRST_PID=$!
	for ((attempt = 0; attempt < 150; ++attempt)); do
		if grep -F -q "Working dir: $PROFILE/" "$PROFILE/log.txt" 2>/dev/null; then
			PROFILE_READY=true
		fi
		if has_start_record "Mac profile IPC selected: variant=non-store directory=$IPC_DIRECTORY"; then
			IPC_SELECTION_READY=true
		fi
		if grep -F -q "Connecting local socket to $SOCKET_PATH" "$PROFILE/log.txt" 2>/dev/null; then
			SOCKET_PATH_READY=true
		fi
		if [[ -n "$(find_lock_path)" ]]; then
			LOCK_READY=true
		fi
		if has_start_record "Mac profile IPC ready: listening=1 full_server_name=$SOCKET_PATH"; then
			SOCKET_LISTENER_READY=true
		fi
		if [[ -S "$SOCKET_PATH" ]]; then
			SOCKET_FILE_READY=true
		fi
		if kill -0 "$FIRST_PID" 2>/dev/null; then
			FIRST_PROCESS_ALIVE=true
		fi
		if [[ "$PROFILE_READY" == true \
			&& "$IPC_SELECTION_READY" == true \
			&& "$LOCK_READY" == true \
			&& "$SOCKET_PATH_READY" == true \
			&& "$SOCKET_LISTENER_READY" == true \
			&& "$SOCKET_FILE_READY" == true \
			&& "$FIRST_PROCESS_ALIVE" == true ]]; then
			break
		fi
		if ! kill -0 "$FIRST_PID" 2>/dev/null; then
			break
		fi
		sleep 0.2
	done

	report_result "${case_name}_profile" "$PROFILE_READY" "path=$PROFILE" || FAILURES=$((FAILURES + 1))
	report_result "${case_name}_ipc_directory" "$IPC_SELECTION_READY" \
		"variant=non-store directory=$IPC_DIRECTORY" || FAILURES=$((FAILURES + 1))
	report_result "${case_name}_lock" "$LOCK_READY" \
		"expected=$LOCK_PATH found=$(find_lock_path)" || FAILURES=$((FAILURES + 1))
	report_result "${case_name}_bound_socket" "$SOCKET_LISTENER_READY" \
		"path=$SOCKET_PATH filesystem_socket=$SOCKET_FILE_READY" || FAILURES=$((FAILURES + 1))
	if ! fixture_unchanged "$fixture" "$fixture_is_file" "$fixture_contents"; then
		report_result "${case_name}_fixture_untouched" false "fixture=$fixture" || FAILURES=$((FAILURES + 1))
	else
		report_result "${case_name}_fixture_untouched" true "fixture=$fixture"
	fi

	set +e
	env HOME="$case_home" TMPDIR="$TEST_TMP_BASE" \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$case_home" \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		"$APP" -quit >"$quit_log" 2>&1
	quit_status=$?
	set -e
	report_result "${case_name}_second_instance_exit" "$([[ "$quit_status" -eq 0 ]] && printf true || printf false)" \
		"status=$quit_status" || FAILURES=$((FAILURES + 1))
	response="$(grep -hF "Show command response received, processId = $FIRST_PID, windowId = 0" "$PROFILE"/log*.txt || true)"
	report_result "${case_name}_first_pid_handshake" "$([[ -n "$response" ]] && printf true || printf false)" \
		"response=$response expected_pid=$FIRST_PID" || FAILURES=$((FAILURES + 1))
	for ((attempt = 0; attempt < 150; ++attempt)); do
		if ! kill -0 "$FIRST_PID" 2>/dev/null; then
			break
		fi
		sleep 0.2
	done
	if kill -0 "$FIRST_PID" 2>/dev/null; then
		report_result "${case_name}_first_instance_exit" false "pid=$FIRST_PID still_alive=1" || true
		return 1
	fi
	set +e
	wait "$FIRST_PID"
	first_status=$?
	set -e
	FIRST_PID=""
	report_result "${case_name}_first_instance_exit" "$([[ "$first_status" -eq 0 ]] && printf true || printf false)" \
		"status=$first_status" || FAILURES=$((FAILURES + 1))
	refusal="$(grep -F 'class=group-container callsite=emojiCacheFolder' \
		"$START_LOG" "$PROFILE/log.txt" 2>/dev/null || true)"
	if [[ -n "$refusal" ]]; then
		report_result "${case_name}_cache_refusal" true "$refusal"
	else
		report_result "${case_name}_cache_refusal" false "missing protected-path refusal from emojiCacheFolder" || FAILURES=$((FAILURES + 1))
	fi
	if ! fixture_unchanged "$fixture" "$fixture_is_file" "$fixture_contents"; then
		report_result "${case_name}_fixture_untouched_after_exit" false "fixture=$fixture" || FAILURES=$((FAILURES + 1))
	else
		report_result "${case_name}_fixture_untouched_after_exit" true "fixture=$fixture"
	fi
	if (( FAILURES > 0 )); then
		cat "$quit_log" "$START_LOG" >&2
		return 1
	fi
}

cleanup() {
	local status=$?
	if [[ -n "$FIRST_PID" ]] && kill -0 "$FIRST_PID" 2>/dev/null; then
		kill -TERM "$FIRST_PID" 2>/dev/null || true
		wait "$FIRST_PID" 2>/dev/null || true
	fi
	if [[ "$status" -eq 0 ]]; then
		rm -rf "$TEST_HOME"
		rm -rf "$IPC_DIRECTORY"
	else
		cp -R "$IPC_DIRECTORY" "$TEST_TMP_BASE/teagram-profile-ipc-failure"
		rm -rf "$IPC_DIRECTORY"
		case "$TEST_HOME/" in
		"$TEST_TMP_BASE/"*)
			;;
		*)
			mkdir -p "$TEST_TMP_BASE"
			cp -R "$TEST_HOME" "$TEST_TMP_BASE/teagram-profile-test-failure" \
				2>/dev/null || true
			;;
		esac
		printf 'Preserving failed synthetic profile state at %s\n' "$TEST_HOME" >&2
	fi
	return "$status"
}
trap cleanup EXIT

run_protected_profile_ipc_case() {
if (( SOCKET_PATH_BYTES > MAC_SOCKET_PATH_LIMIT )); then
	echo "socket path is $SOCKET_PATH_BYTES bytes; macOS sun_path limit is $MAC_SOCKET_PATH_LIMIT: $SOCKET_PATH" >&2
	exit 1
fi
printf 'socket_path_bytes=PASS %s/%s path=%s\n' \
	"$SOCKET_PATH_BYTES" "$MAC_SOCKET_PATH_LIMIT" "$SOCKET_PATH"

REFUSAL_LOCK_BEFORE="$(find_lock_path)"
set +e
REFUSAL_OUTPUT="$(env HOME="$HOSTILE_HOME" TMPDIR="$HOSTILE_HOME" \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$TEST_HOME" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 "$APP" -quit 2>&1)"
REFUSAL_STATUS=$?
set -e
REFUSAL_LOCK_AFTER="$(find_lock_path)"
printf 'protected_home_refusal_lock_baseline existed_before=%s exists_after=%s refusal_status=%s\n' \
	"$([[ -n "$REFUSAL_LOCK_BEFORE" ]] && printf 1 || printf 0)" \
	"$([[ -n "$REFUSAL_LOCK_AFTER" ]] && printf 1 || printf 0)" "$REFUSAL_STATUS"

if [[ "$REFUSAL_STATUS" -eq 0 ]]; then
	echo "hostile HOME unexpectedly started Teagram." >&2
	printf '%s\n' "$REFUSAL_OUTPUT" >&2
	exit 1
fi
if [[ -n "$REFUSAL_OUTPUT" ]]; then
	echo "home discovery failure emitted output before Seatbelt activation." >&2
	printf '%s\n' "$REFUSAL_OUTPUT" >&2
	exit 1
fi
if [[ -n "$REFUSAL_LOCK_BEFORE" || -n "$REFUSAL_LOCK_AFTER" ]]; then
	echo "profile refusal created or found an IPC lock." >&2
	exit 1
fi
if [[ -S "$SOCKET_PATH" ]]; then
	echo "profile refusal created an IPC socket." >&2
	exit 1
fi
if [[ -e "$PROFILE" || -e "$HOSTILE_HOME" || -e "$START_LOG" ]]; then
	echo "hostile profile initialization created a profile or protected path." >&2
	exit 1
fi
printf 'protected_home_refusal=PASS status=%s silent=1 profile_and_ipc_absent=1\n' \
	"$REFUSAL_STATUS"

COMPILE_FAILURE_HOME="$TEST_HOME/compile-failure-home"
COMPILE_FAILURE_PROFILE="$COMPILE_FAILURE_HOME/Library/Application Support/Teagram"
COMPILE_FAILURE_PROFILE_ARTIFACT="$COMPILE_FAILURE_HOME/generated-profile.sb"
mkdir -p "$COMPILE_FAILURE_HOME"
set +e
COMPILE_FAILURE_OUTPUT="$(env HOME="$COMPILE_FAILURE_HOME" TMPDIR="$TEST_TMP_BASE" \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$COMPILE_FAILURE_HOME" \
	TDESKTOP_MAC_PROFILE_TEST_DIAGNOSTICS=1 \
	TDESKTOP_MAC_PROFILE_TEST_PROFILE_ARTIFACT="$COMPILE_FAILURE_PROFILE_ARTIFACT" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	TDESKTOP_MAC_SEATBELT_FORCE_COMPILE_FAILURE=1 "$APP" -quit 2>&1)"
COMPILE_FAILURE_STATUS=$?
set -e
if [[ "$COMPILE_FAILURE_STATUS" -eq 0 ]]; then
	echo "forced Seatbelt compilation failure unexpectedly started Teagram." >&2
	printf '%s\n' "$COMPILE_FAILURE_OUTPUT" >&2
	exit 1
fi
if [[ "$COMPILE_FAILURE_OUTPUT" != *"stage=seatbelt"* \
	|| "$COMPILE_FAILURE_OUTPUT" != *"Mac profile artifact saved:"* ]]; then
	echo "Seatbelt compilation failure diagnostics or profile artifact status is missing." >&2
	printf '%s\n' "$COMPILE_FAILURE_OUTPUT" >&2
	exit 1
fi
if [[ ! -s "$COMPILE_FAILURE_PROFILE_ARTIFACT" ]] \
		|| [[ "$(tail -c 2 "$COMPILE_FAILURE_PROFILE_ARTIFACT" \
			| od -An -t x1 | tr -d '[:space:]')" != 280a ]]; then
	echo "Seatbelt compilation failure did not retain the generated profile text." >&2
	exit 1
fi
if [[ -e "$COMPILE_FAILURE_PROFILE" \
	|| -e "$COMPILE_FAILURE_PROFILE/log.txt" \
	|| -e "$COMPILE_FAILURE_HOME/refusal.log" ]]; then
	echo "Seatbelt compilation failure created a profile or log." >&2
	exit 1
fi
printf 'seatbelt_compile_failure=PASS status=%s profile_artifact=PASS profile_and_logs_absent=1\n' \
	"$COMPILE_FAILURE_STATUS"

SILENT_COMPILE_FAILURE_HOME="$TEST_HOME/silent-compile-failure-home"
SILENT_COMPILE_FAILURE_PROFILE="$SILENT_COMPILE_FAILURE_HOME/Library/Application Support/Teagram"
SILENT_COMPILE_FAILURE_ARTIFACT="$SILENT_COMPILE_FAILURE_HOME/generated-profile.sb"
mkdir -p "$SILENT_COMPILE_FAILURE_HOME"
set +e
SILENT_COMPILE_FAILURE_OUTPUT="$(env HOME="$SILENT_COMPILE_FAILURE_HOME" \
	TMPDIR="$TEST_TMP_BASE" \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$SILENT_COMPILE_FAILURE_HOME" \
	TDESKTOP_MAC_PROFILE_TEST_DIAGNOSTICS=0 \
	TDESKTOP_MAC_PROFILE_TEST_PROFILE_ARTIFACT="" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	TDESKTOP_MAC_SEATBELT_FORCE_COMPILE_FAILURE=1 "$APP" -quit 2>&1)"
SILENT_COMPILE_FAILURE_STATUS=$?
set -e
if [[ "$SILENT_COMPILE_FAILURE_STATUS" -eq 0 ]]; then
	echo "silent forced Seatbelt compilation failure unexpectedly started Teagram." >&2
	printf '%s\n' "$SILENT_COMPILE_FAILURE_OUTPUT" >&2
	exit 1
fi
if [[ -n "$SILENT_COMPILE_FAILURE_OUTPUT" ]]; then
	echo "production Seatbelt compilation failure emitted output." >&2
	printf '%s\n' "$SILENT_COMPILE_FAILURE_OUTPUT" >&2
	exit 1
fi
if [[ -e "$SILENT_COMPILE_FAILURE_PROFILE" \
	|| -e "$SILENT_COMPILE_FAILURE_PROFILE/log.txt" \
	|| -e "$SILENT_COMPILE_FAILURE_HOME/start.log" \
	|| -e "$SILENT_COMPILE_FAILURE_HOME/refusal.log" \
	|| -e "$SILENT_COMPILE_FAILURE_ARTIFACT" ]]; then
	echo "silent Seatbelt compilation failure created a profile, log, or artifact." >&2
	exit 1
fi
printf 'seatbelt_compile_failure_silent=PASS status=%s output_empty=1 profile_logs_and_artifacts_absent=1\n' \
	"$SILENT_COMPILE_FAILURE_STATUS"

ACCOUNT_STATE="$PROFILE/tdata/teagram-activation-account-state.fixture"
ENDPOINT_ENROLLMENT="$PROFILE/tdata/teagram-activation-endpoint-enrollment.fixture"
TELEGRAMD_PROFILE="$TEST_HOME/Library/Application Support/Telegramd"
mkdir -p "$PROFILE/tdata"
printf '%s' 'synthetic account state v1' > "$ACCOUNT_STATE"
printf '%s\n%s' 'https://telegramd.example:443' 'fingerprint=1234567890' \
	> "$ENDPOINT_ENROLLMENT"
ACCOUNT_STATE_HASH="$(shasum -a 256 "$ACCOUNT_STATE" | awk '{print $1}')"
ENDPOINT_ENROLLMENT_HASH="$(shasum -a 256 "$ENDPOINT_ENROLLMENT" | awk '{print $1}')"
printf 'account_state_sha256_before=%s endpoint_enrollment_sha256_before=%s\n' \
	"$ACCOUNT_STATE_HASH" "$ENDPOINT_ENROLLMENT_HASH"

SEATBELT_CANARY="$TEST_HOME/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/synthetic-canary"
mkdir -p "$(dirname "$SEATBELT_CANARY")"
printf '%s' 'synthetic protected canary' > "$SEATBELT_CANARY"
CANARY_HASH="$(shasum -a 256 "$SEATBELT_CANARY" | awk '{print $1}')"
set +e
CANARY_OUTPUT="$(env HOME="$TEST_HOME" TMPDIR="$TEST_TMP_BASE" LC_ALL=C \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$TEST_HOME" \
	TDESKTOP_MAC_PROFILE_TEST_DIAGNOSTICS=1 \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	"$APP" --mac-seatbelt-cat-probe "$SEATBELT_CANARY" 2>&1)"
CANARY_STATUS=$?
set -e
if [[ "$CANARY_STATUS" -ne 0 \
	|| "$CANARY_OUTPUT" != "Mac profile IPC selected: variant=non-store directory=$IPC_DIRECTORY" ]]; then
	echo "spawned /bin/cat denial probe did not return the expected EPERM result." >&2
	printf '%s\n' "$CANARY_OUTPUT" >&2
	printf '%s\n' "$CANARY_OUTPUT" | python3 \
		"$(dirname "$0")/diagnose_mac_seatbelt.py" \
		"$TEST_HOME" "$SEATBELT_CANARY" "$ACCOUNT_STATE" \
		"$PROFILE_ARTIFACT" \
		|| echo "Seatbelt diagnostic controls failed; see matrix above." >&2
	exit 1
fi
if [[ "$(shasum -a 256 "$SEATBELT_CANARY" | awk '{print $1}')" != "$CANARY_HASH" ]]; then
	echo "Seatbelt canary changed during the descendant denial probe." >&2
	exit 1
fi
printf 'seatbelt_descendant_denial=PASS mode=posix_spawn child=/bin/cat status=%s errno=EPERM\n' \
	"$CANARY_STATUS"
printf 'seatbelt_posix_spawn_cat_denial=PASS mode=posix_spawn child=/bin/cat errno=EPERM path=%s\n' \
	"$SEATBELT_CANARY"

SOUND_LOOKUP_OUTPUT="$(env HOME="$TEST_HOME" TMPDIR="$TEST_TMP_BASE" LC_ALL=C \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$TEST_HOME" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	"$APP" --mac-notification-sound-probe 2>&1)" || {
	echo "native notification sound lookup failed." >&2
	printf '%s\n' "$SOUND_LOOKUP_OUTPUT" >&2
	exit 1
}
printf 'notification_sound_lookup=PASS namespace=Teagram_ lookup=NSSound bytes=unchanged cleanup=PASS\n'

IGNORABLE_HOME="$TEST_HOME/teagram-ignorable-home"
IGNORABLE_HOME_ALIAS="$TEST_HOME/teagram-ignorable-$(printf '\342\200\213')home"
mkdir -p "$IGNORABLE_HOME" "$IGNORABLE_HOME_ALIAS"
run_home_spelling_probes \
	seatbelt_ignorable_home \
	"$IGNORABLE_HOME" "$IGNORABLE_HOME_ALIAS" \
	|| exit 1
run_home_spelling_probes \
	seatbelt_ignorable_profile_home \
	"$IGNORABLE_HOME_ALIAS" "$IGNORABLE_HOME" \
	|| exit 1

run_home_spelling_probes \
	seatbelt_case_spelling \
	"$TEST_HOME" "$TEST_HOME" \
	"lIBRARY/aPPLICATION sUPPORT/tElEgRaM dEsKtOp" \
	"lIBRARY/aPPLICATION sUPPORT/OtherApp/TeLeGrAm Desktop/tdata" \
	|| exit 1

CASE_SWAPPED_HOME="$(
python3 - "$TEST_HOME" <<'PY'
import os
import sys

parent, name = os.path.split(sys.argv[1])
print(os.path.join(parent, name.swapcase()))
PY
)"
if [[ "$CASE_SWAPPED_HOME" == "$TEST_HOME" || ! -d "$CASE_SWAPPED_HOME" ]]; then
	echo "case-swapped test home does not resolve to the same directory: $CASE_SWAPPED_HOME" >&2
	exit 1
fi
CASE_SWAPPED_HOME_ID="$(stat -f '%d:%i' "$CASE_SWAPPED_HOME")"
TEST_HOME_ID="$(stat -f '%d:%i' "$TEST_HOME")"
if [[ "$CASE_SWAPPED_HOME_ID" != "$TEST_HOME_ID" ]]; then
	echo "case-swapped test home resolved to a different directory: $CASE_SWAPPED_HOME" >&2
	exit 1
fi
printf 'seatbelt_case_swapped_home_alias=PASS profile_home=%s path_home=%s inode=%s\n' \
	"$TEST_HOME" "$CASE_SWAPPED_HOME" "$CASE_SWAPPED_HOME_ID"
run_home_spelling_probes \
	seatbelt_case_swapped_home \
	"$TEST_HOME" "$CASE_SWAPPED_HOME" \
	|| exit 1

KELVIN_WEBKIT_CANONICAL="$TEST_HOME/Library/WebKit"
KELVIN_WEBKIT_ALIAS="$TEST_HOME/Library/WebKit"
verify_casefold_directory_alias \
	seatbelt_kelvin_webkit_alias \
	"$KELVIN_WEBKIT_CANONICAL" "$KELVIN_WEBKIT_ALIAS" \
	|| exit 1
run_home_spelling_probes \
	seatbelt_kelvin_webkit \
	"$TEST_HOME" "$TEST_HOME" \
	"Library/WebKit/org.telegram.desktop" \
	|| exit 1

LONG_S_TELEGRAM_CANONICAL="$TEST_HOME/Library/Application Support/Telegram Desktop"
LONG_S_TELEGRAM_ALIAS="$TEST_HOME/Library/Application Support/Telegram Deſktop"
verify_casefold_directory_alias \
	seatbelt_long_s_telegram_desktop_alias \
	"$LONG_S_TELEGRAM_CANONICAL" "$LONG_S_TELEGRAM_ALIAS" \
	|| exit 1
run_home_spelling_probes \
	seatbelt_long_s_telegram_desktop \
	"$TEST_HOME" "$TEST_HOME" \
	"Library/Application Support/Telegram Deſktop" \
	|| exit 1

NON_ASCII_HOME_NFC_NAME="$(python3 -c 'import unicodedata; print(unicodedata.normalize("NFC", "teagram-café"))')"
NON_ASCII_HOME_NFD_NAME="$(python3 -c 'import unicodedata; print(unicodedata.normalize("NFD", "teagram-café"))')"
NON_ASCII_HOME_NFC="$TEST_HOME/$NON_ASCII_HOME_NFC_NAME"
NON_ASCII_HOME_NFD="$TEST_HOME/$NON_ASCII_HOME_NFD_NAME"
mkdir -p "$NON_ASCII_HOME_NFC"
run_home_spelling_probes \
	seatbelt_non_ascii_nfc_home \
	"$NON_ASCII_HOME_NFC" "$NON_ASCII_HOME_NFC" \
	|| exit 1
if [[ -d "$NON_ASCII_HOME_NFD" ]] \
	&& [[ "$(stat -f '%d:%i' "$NON_ASCII_HOME_NFC")" \
		== "$(stat -f '%d:%i' "$NON_ASCII_HOME_NFD")" ]]; then
	run_home_spelling_probes \
		seatbelt_nfd_home_alias \
		"$NON_ASCII_HOME_NFC" "$NON_ASCII_HOME_NFD" \
		|| exit 1
	run_home_spelling_probes \
		seatbelt_non_ascii_nfd_home \
		"$NON_ASCII_HOME_NFD" "$NON_ASCII_HOME_NFD" \
		|| exit 1
else
	mkdir -p "$NON_ASCII_HOME_NFD"
	run_home_spelling_probes \
		seatbelt_non_ascii_nfd_home \
		"$NON_ASCII_HOME_NFD" "$NON_ASCII_HOME_NFD" \
		|| exit 1
fi

while IFS='|' read -r UNICODE_CASE_NAME UNICODE_CANONICAL_NAME UNICODE_ALIAS_NAME; do
	UNICODE_CASE_CANONICAL="$TEST_HOME/$UNICODE_CANONICAL_NAME"
	UNICODE_CASE_ALIAS="$TEST_HOME/$UNICODE_ALIAS_NAME"
	verify_casefold_directory_alias \
		"seatbelt_unicode_home_${UNICODE_CASE_NAME}_alias" \
		"$UNICODE_CASE_CANONICAL" "$UNICODE_CASE_ALIAS" \
		|| exit 1
	run_home_spelling_probes \
		"seatbelt_unicode_home_${UNICODE_CASE_NAME}" \
		"$UNICODE_CASE_CANONICAL" "$UNICODE_CASE_ALIAS" \
		|| exit 1
	run_home_spelling_probes \
		"seatbelt_unicode_home_${UNICODE_CASE_NAME}_reverse" \
		"$UNICODE_CASE_ALIAS" "$UNICODE_CASE_CANONICAL" \
		|| exit 1
done <<'UNICODE_HOME_CASES'
accent|teagram-Élise|teagram-élise
stroke|teagram-Ølise|teagram-ølise
cyrillic|teagram-АЛИСА|teagram-алиса
sigma|teagram-Σ|teagram-ς
deseret|teagram-𐐀|teagram-𐐨
sharp-s|teagram-Maß|teagram-Mass
UNICODE_HOME_CASES

LONG_SHARP_S_HOME="$TEST_HOME/teagram-ssssssssss"
LONG_SHARP_S_HOME_ALIAS="$TEST_HOME/teagram-ßßßßß"
verify_casefold_directory_alias \
	seatbelt_long_sharp_s_home_alias \
	"$LONG_SHARP_S_HOME" "$LONG_SHARP_S_HOME_ALIAS" \
	|| exit 1
run_home_spelling_probes \
	seatbelt_long_sharp_s_home \
	"$LONG_SHARP_S_HOME" "$LONG_SHARP_S_HOME_ALIAS" \
	"Library/Application Support/Telegram Desktop" \
	"Library/Application Support/Teagram/tdata" \
	"Library/Application Support/OtherApp/ordinary-data" \
	|| exit 1

run_seatbelt_mode_probe \
	seatbelt_parent_open_denial \
	--mac-seatbelt-parent-open-probe "$TEST_HOME" "$SEATBELT_CANARY" \
	parent EPERM \
	|| exit 1
run_seatbelt_mode_probe \
	seatbelt_fork_open_denial \
	--mac-seatbelt-fork-open-probe "$TEST_HOME" "$SEATBELT_CANARY" \
	fork EPERM \
	|| exit 1
run_seatbelt_mode_probe \
	seatbelt_fork_exec_cat_denial \
	--mac-seatbelt-fork-exec-cat-probe "$TEST_HOME" "$SEATBELT_CANARY" \
	fork-exec EPERM \
	|| exit 1

DEFAULT_IGNORABLE="$(printf '\342\200\213')"
IGNORABLE_CHARACTERS=(
	"$(printf '\342\200\213')"
	"$(printf '\342\200\214')"
	"$(printf '\342\200\215')"
	"$(printf '\342\200\216')"
	"$(printf '\342\200\217')"
	"$(printf '\342\200\252')"
	"$(printf '\342\200\253')"
	"$(printf '\342\200\254')"
	"$(printf '\342\200\255')"
	"$(printf '\342\200\256')"
	"$(printf '\342\201\252')"
	"$(printf '\342\201\253')"
	"$(printf '\342\201\254')"
	"$(printf '\342\201\255')"
	"$(printf '\342\201\256')"
	"$(printf '\342\201\257')"
	"$(printf '\357\273\277')"
)
for index in "${!IGNORABLE_CHARACTERS[@]}"; do
	ignored="${IGNORABLE_CHARACTERS[$index]}"
	canary="$TEST_HOME/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Te${ignored}legram/synthetic-canary"
	mkdir -p "$(dirname "$canary")"
	printf '%s' 'synthetic protected canary with a default-ignorable character' \
		> "$canary"
	run_seatbelt_cat_probe \
		"seatbelt_default_ignorable_${index}_denial" \
		--mac-seatbelt-cat-probe "$TEST_HOME" "$canary" \
		|| exit 1
done

IGNORABLE_LIBRARY_CANARY="$TEST_HOME/Lib${DEFAULT_IGNORABLE}rary/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/synthetic-canary"
mkdir -p "$(dirname "$IGNORABLE_LIBRARY_CANARY")"
printf '%s' 'synthetic protected canary with an ignored Library character' \
	> "$IGNORABLE_LIBRARY_CANARY"
run_seatbelt_cat_probe \
	seatbelt_default_ignorable_library_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$IGNORABLE_LIBRARY_CANARY" \
	|| exit 1

FOLDED_GROUP_PARENT_CANARY="$TEST_HOME/Library/Group Con${DEFAULT_IGNORABLE}tainers/6N38VWS5BX.ru.keepcoder.Telegram/synthetic-canary"
mkdir -p "$(dirname "$FOLDED_GROUP_PARENT_CANARY")"
printf '%s' 'synthetic protected canary with a folded group parent' \
	> "$FOLDED_GROUP_PARENT_CANARY"
run_seatbelt_cat_probe \
	seatbelt_default_ignorable_group_parent_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$FOLDED_GROUP_PARENT_CANARY" \
	|| exit 1

FOLDED_PARENT_CANARY="$TEST_HOME/Library/Application Supp${DEFAULT_IGNORABLE}ort/Telegram Desktop/synthetic-canary"
mkdir -p "$(dirname "$FOLDED_PARENT_CANARY")"
printf '%s' 'synthetic protected canary with a folded parent component' \
	> "$FOLDED_PARENT_CANARY"
FOLDED_PARENT_CANARY_HASH="$(shasum -a 256 \
	"$FOLDED_PARENT_CANARY" | awk '{print $1}')"
run_seatbelt_cat_probe \
	seatbelt_default_ignorable_application_support_parent_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$FOLDED_PARENT_CANARY" \
	|| exit 1

FOLDED_APPLICATION_CANARY="$TEST_HOME/Library/Application Support/Telegram Des${DEFAULT_IGNORABLE}ktop/synthetic-canary"
mkdir -p "$(dirname "$FOLDED_APPLICATION_CANARY")"
printf '%s' 'synthetic protected canary with a folded application component' \
	> "$FOLDED_APPLICATION_CANARY"
run_seatbelt_cat_probe \
	seatbelt_default_ignorable_application_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$FOLDED_APPLICATION_CANARY" \
	|| exit 1

UNRELATED_CANARY="$TEST_HOME/Library/Application Support/OtherApp/telegram/cache${DEFAULT_IGNORABLE}"
mkdir -p "$(dirname "$UNRELATED_CANARY")"
printf '%s' 'synthetic unrelated application bytes' > "$UNRELATED_CANARY"
run_seatbelt_cat_probe \
	seatbelt_unrelated_telegram_ignorable_allowed \
	--mac-seatbelt-cat-allow-probe "$TEST_HOME" "$UNRELATED_CANARY" \
	|| exit 1

NARROW_NO_BREAK_SPACE="$(printf '\342\200\257')"
NEAR_MATCH_CANARY="$TEST_HOME/Library/Application Supp${NARROW_NO_BREAK_SPACE}ort/Telegram Desktop/synthetic-canary"
mkdir -p "$(dirname "$NEAR_MATCH_CANARY")"
printf '%s' 'synthetic non-ignorable near-match bytes' > "$NEAR_MATCH_CANARY"
# The ignored-character class accepts any non-ASCII character, so a
# non-ignorable one inside a protected name is denied as well.
run_seatbelt_cat_probe \
	seatbelt_non_ignorable_application_support_near_match_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$NEAR_MATCH_CANARY" \
	|| exit 1

ASCII_NEAR_MATCH_CANARY="$TEST_HOME/Library/Application Suppxort/Telegram Desktop/synthetic-allowed-file"
mkdir -p "$(dirname "$ASCII_NEAR_MATCH_CANARY")"
printf '%s' 'synthetic ASCII near-match bytes' > "$ASCII_NEAR_MATCH_CANARY"
run_seatbelt_cat_probe \
	seatbelt_ascii_application_support_near_match_allowed \
	--mac-seatbelt-cat-allow-probe "$TEST_HOME" "$ASCII_NEAR_MATCH_CANARY" \
	|| exit 1

OTHER_APP_TELEGRAM_CANARY="$TEST_HOME/Library/Application Support/OtherApp/Telegram Desktop/synthetic-allowed-file"
mkdir -p "$(dirname "$OTHER_APP_TELEGRAM_CANARY")"
printf '%s' 'synthetic nested Telegram name bytes' > "$OTHER_APP_TELEGRAM_CANARY"
run_seatbelt_cat_probe \
	seatbelt_nested_telegram_desktop_name_allowed \
	--mac-seatbelt-cat-allow-probe "$TEST_HOME" "$OTHER_APP_TELEGRAM_CANARY" \
	|| exit 1

CONTAINER_CANARY="$TEST_HOME/Library/Containers/org.telegram.desktop/synthetic-canary"
mkdir -p "$(dirname "$CONTAINER_CANARY")"
printf '%s' 'synthetic protected container bytes' > "$CONTAINER_CANARY"
CONTAINER_CANARY_HASH="$(shasum -a 256 "$CONTAINER_CANARY" | awk '{print $1}')"
run_seatbelt_cat_probe \
	seatbelt_container_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$CONTAINER_CANARY" \
	|| exit 1

FOLDED_CONTAINER_CANARY="$TEST_HOME/Library/Containers/${DEFAULT_IGNORABLE}org.telegram.desktop${DEFAULT_IGNORABLE}/synthetic-canary"
mkdir -p "$(dirname "$FOLDED_CONTAINER_CANARY")"
printf '%s' 'synthetic protected canary with a folded container bundle id' \
	> "$FOLDED_CONTAINER_CANARY"
run_seatbelt_cat_probe \
	seatbelt_default_ignorable_container_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$FOLDED_CONTAINER_CANARY" \
	|| exit 1

run_bundle_keyed_probe seatbelt_bundle_keyed_denial Preferences || exit 1
run_bundle_keyed_probe seatbelt_caches_denial Caches || exit 1
run_bundle_keyed_probe seatbelt_httpstorages_denial HTTPStorages || exit 1
run_bundle_keyed_probe seatbelt_webkit_denial WebKit || exit 1
run_bundle_keyed_probe \
	seatbelt_saved_application_state_denial \
	"Saved Application State" \
	|| exit 1

FOLDED_BUNDLE_CANARY="$TEST_HOME/Library/Preferences/org.telegram.des${DEFAULT_IGNORABLE}ktop.fixture/synthetic-canary"
mkdir -p "$(dirname "$FOLDED_BUNDLE_CANARY")"
printf '%s' 'synthetic protected canary with a folded bundle-keyed id' \
	> "$FOLDED_BUNDLE_CANARY"
run_seatbelt_cat_probe \
	seatbelt_default_ignorable_bundle_keyed_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$FOLDED_BUNDLE_CANARY" \
	|| exit 1

FOLDED_BUNDLE_PARENT_CANARY="$TEST_HOME/Library/Preference${DEFAULT_IGNORABLE}s/org.telegram.desktop/synthetic-canary"
mkdir -p "$(dirname "$FOLDED_BUNDLE_PARENT_CANARY")"
printf '%s' 'synthetic protected canary with a folded bundle-keyed parent' \
	> "$FOLDED_BUNDLE_PARENT_CANARY"
run_seatbelt_cat_probe \
	seatbelt_default_ignorable_bundle_parent_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$FOLDED_BUNDLE_PARENT_CANARY" \
	|| exit 1

APPLICATION_SUPPORT_CANARY="$PROFILE/../Telegram Desktop/tdata/synthetic-canary"
mkdir -p "$(dirname "$APPLICATION_SUPPORT_CANARY")"
printf '%s' 'synthetic protected application support bytes' \
	> "$APPLICATION_SUPPORT_CANARY"
APPLICATION_SUPPORT_CANARY_HASH="$(shasum -a 256 \
	"$APPLICATION_SUPPORT_CANARY" | awk '{print $1}')"
run_seatbelt_cat_probe \
	seatbelt_application_support_traversal_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$APPLICATION_SUPPORT_CANARY" \
	|| exit 1

REALPATH_HOME_ALIAS="$TEST_HOME/realpath-home"
ln -s "$TEST_HOME" "$REALPATH_HOME_ALIAS"
REALPATH_CANARY="$REALPATH_HOME_ALIAS/Library/Application Support/Teagram/../Telegram Desktop/tdata/synthetic-canary"
run_seatbelt_cat_probe \
	seatbelt_application_support_realpath_denial \
	--mac-seatbelt-cat-probe "$REALPATH_HOME_ALIAS" "$REALPATH_CANARY" \
	|| exit 1

FIRMLINK_HOME="/System/Volumes/Data$TEST_HOME"
if [[ ! -d "$FIRMLINK_HOME" ]] \
	|| [[ "$(stat -f '%d:%i' "$FIRMLINK_HOME")" \
		!= "$(stat -f '%d:%i' "$TEST_HOME")" ]]; then
	echo "synthetic home has no matching /System/Volumes/Data alias." >&2
	exit 1
fi
FIRMLINK_CANARY="$FIRMLINK_HOME/Library/Application Support/Teagram/../Telegram Desktop/tdata/synthetic-canary"
run_seatbelt_cat_probe \
	seatbelt_application_support_firmlink_denial \
	--mac-seatbelt-cat-probe "$TEST_HOME" "$FIRMLINK_CANARY" \
	|| exit 1

run_seatbelt_cat_probe \
	seatbelt_teagram_profile_allowed \
	--mac-seatbelt-cat-allow-probe "$TEST_HOME" "$ACCOUNT_STATE" \
	|| exit 1
run_seatbelt_mode_probe \
	seatbelt_parent_open_allowed \
	--mac-seatbelt-parent-open-allow-probe "$TEST_HOME" "$ACCOUNT_STATE" \
	parent 0 \
	|| exit 1
run_seatbelt_mode_probe \
	seatbelt_fork_open_allowed \
	--mac-seatbelt-fork-open-allow-probe "$TEST_HOME" "$ACCOUNT_STATE" \
	fork 0 \
	|| exit 1
run_seatbelt_mode_probe \
	seatbelt_fork_exec_cat_allowed \
	--mac-seatbelt-fork-exec-cat-allow-probe "$TEST_HOME" "$ACCOUNT_STATE" \
	fork-exec 0 \
	|| exit 1

if [[ "$(shasum -a 256 "$APPLICATION_SUPPORT_CANARY" | awk '{print $1}')" \
	!= "$APPLICATION_SUPPORT_CANARY_HASH" ]]; then
	echo "application support canary changed during the denial probes." >&2
	exit 1
fi
if [[ "$(shasum -a 256 "$CONTAINER_CANARY" | awk '{print $1}')" \
	!= "$CONTAINER_CANARY_HASH" ]]; then
	echo "container canary changed during the denial probes." >&2
	exit 1
fi
if [[ "$(shasum -a 256 "$FOLDED_PARENT_CANARY" | awk '{print $1}')" \
	!= "$FOLDED_PARENT_CANARY_HASH" ]]; then
	echo "folded application support parent canary changed during denial probes." >&2
	exit 1
fi
if [[ "$(shasum -a 256 "$ACCOUNT_STATE" | awk '{print $1}')" \
	!= "$ACCOUNT_STATE_HASH" ]]; then
	echo "allowed Teagram profile file changed during the access probe." >&2
	exit 1
fi

mkdir -p "$IPC_DIRECTORY"

env HOME="$TEST_HOME" TMPDIR="$TEST_TMP_BASE" TDESKTOP_MAC_PROFILE_TEST_HOME="$TEST_HOME" TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 "$APP" >"$START_LOG" 2>&1 &
FIRST_PID=$!

PROFILE_READY=false
IPC_SELECTION_READY=false
LOCK_READY=false
SOCKET_PATH_READY=false
SOCKET_LISTENER_READY=false
SOCKET_FILE_READY=false
FIRST_PROCESS_ALIVE=false
for ((attempt = 0; attempt < 150; ++attempt)); do
	if grep -F -q "Working dir: $PROFILE/" "$PROFILE/log.txt" 2>/dev/null; then
		PROFILE_READY=true
	fi
	if has_start_record "Mac profile IPC selected: variant=non-store directory=$IPC_DIRECTORY"; then
		IPC_SELECTION_READY=true
	fi
	if grep -F -q "Connecting local socket to $SOCKET_PATH" "$PROFILE/log.txt" 2>/dev/null; then
		SOCKET_PATH_READY=true
	fi
	if [[ -n "$(find_lock_path)" ]]; then
		LOCK_READY=true
	fi
	if has_start_record "Mac profile IPC ready: listening=1 full_server_name=$SOCKET_PATH"; then
		SOCKET_LISTENER_READY=true
	fi
	if [[ -S "$SOCKET_PATH" ]]; then
		SOCKET_FILE_READY=true
	fi
	if kill -0 "$FIRST_PID" 2>/dev/null; then
		FIRST_PROCESS_ALIVE=true
	fi
	if [[ "$PROFILE_READY" == true \
		&& "$IPC_SELECTION_READY" == true \
		&& "$LOCK_READY" == true \
		&& "$SOCKET_PATH_READY" == true \
		&& "$SOCKET_LISTENER_READY" == true \
		&& "$SOCKET_FILE_READY" == true \
		&& "$FIRST_PROCESS_ALIVE" == true ]]; then
		break
	fi
	if ! kill -0 "$FIRST_PID" 2>/dev/null; then
		break
	fi
	sleep 0.2
done

FAILURES=0
report_result profile "$PROFILE_READY" "path=$PROFILE" || FAILURES=$((FAILURES + 1))
report_result ipc_variant_and_directory "$IPC_SELECTION_READY" \
	"variant=non-store directory=$IPC_DIRECTORY" || FAILURES=$((FAILURES + 1))
report_result socket_path "$SOCKET_PATH_READY" \
	"path=$SOCKET_PATH bytes=$SOCKET_PATH_BYTES/$MAC_SOCKET_PATH_LIMIT" || FAILURES=$((FAILURES + 1))
LOCK_FOUND="$(find_lock_path)"
report_result lock "$LOCK_READY" \
	"expected=$LOCK_PATH found=${LOCK_FOUND:-missing}" || FAILURES=$((FAILURES + 1))
report_result bound_socket "$SOCKET_LISTENER_READY" \
	"listening=1 full_server_name=$SOCKET_PATH filesystem_socket=$SOCKET_FILE_READY" \
	|| FAILURES=$((FAILURES + 1))
report_result first_instance_pid "$FIRST_PROCESS_ALIVE" "pid=$FIRST_PID" \
	|| FAILURES=$((FAILURES + 1))
if (( FAILURES > 0 )); then
	cat "$START_LOG" >&2
	if [[ -f "$PROFILE/log.txt" ]]; then
		cat "$PROFILE/log.txt" >&2
	fi
	exit 1
fi

SOCKET_TARGET="$TEST_HOME/Library/Application Support/Telegram Desktop/tdata/socket-x"
mkdir -p "$(dirname "$SOCKET_TARGET")"

python3 - "$SOCKET_PATH" "$SOCKET_TARGET" <<'PY'
import pathlib
import socket
import sys
import time

socket_path, target = sys.argv[1:]


def request(command, deadline):
	remaining = deadline - time.monotonic()
	if remaining <= 0:
		raise TimeoutError("request deadline expired")
	with socket.socket(socket.AF_UNIX) as client:
		client.settimeout(remaining)
		client.connect(socket_path)
		client.sendall(command.encode())
		response = bytearray()
		while b";" not in response:
			chunk = client.recv(256)
			if not chunk:
				raise RuntimeError("socket closed before response")
			response.extend(chunk)
		if not response.startswith(b"RES:"):
			raise RuntimeError(f"unexpected socket response: {response!r}")
		return bytes(response)


deadline = time.monotonic() + 30
last_error = None
while time.monotonic() < deadline:
	try:
		request("CMD:show;", min(deadline, time.monotonic() + 2))
		break
	except (OSError, TimeoutError, RuntimeError) as error:
		last_error = error
		time.sleep(0.1)
else:
	raise RuntimeError(
		f"single-instance socket did not answer readiness probe: {last_error}")

request(
	"OPEN:" + pathlib.Path(target).as_uri() + ";",
	time.monotonic() + 10)
PY

for ((attempt = 0; attempt < 50; ++attempt)); do
	if has_start_record "class=application-support callsite=sandbox.open"; then
		break
	fi
	sleep 0.1
done
if ! has_start_record "class=application-support callsite=sandbox.open"; then
	echo "single-instance OPEN command was not refused before dispatch." >&2
	cat "$START_LOG" >&2
	if [[ -f "$PROFILE/log.txt" ]]; then
		cat "$PROFILE/log.txt" >&2
	fi
	exit 1
fi
if grep -F -q "$TEST_HOME/Library/Application Support/Telegram Desktop" "$START_LOG"; then
	echo "single-instance OPEN refusal disclosed the protected path." >&2
	cat "$START_LOG" >&2
	exit 1
fi
printf 'socket_open_protected_path_refusal=PASS class=application-support before-dispatch=1\n'

ARGV_HOME="$TEST_HOME/argv-home"
mkdir -p "$ARGV_HOME/uploads" \
	"$ARGV_HOME/Library/Application Support/Telegram Desktop/tdata"
ARGV_REFUSAL_LOG="$TEST_HOME/argv-refusal.log"
set +e
(cd "$ARGV_HOME" && env HOME="$ARGV_HOME" TMPDIR="$TEST_TMP_BASE" \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$ARGV_HOME" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	"$APP" -quit -- "./Library/Application Support/Telegram Desktop/tdata/x") >"$ARGV_REFUSAL_LOG" 2>&1
ARGV_REFUSAL_STATUS=$?
set -e
if [[ "$ARGV_REFUSAL_STATUS" -ne 0 ]] \
	|| ! grep -F -q "class=application-support callsite=launcher.argv" "$ARGV_REFUSAL_LOG"; then
	echo "protected command-line path was not refused before dispatch." >&2
	cat "$ARGV_REFUSAL_LOG" >&2
	exit 1
fi
if grep -F -q "$ARGV_HOME/Library/Application Support/Telegram Desktop" "$ARGV_REFUSAL_LOG"; then
	echo "command-line path refusal disclosed the protected path." >&2
	cat "$ARGV_REFUSAL_LOG" >&2
	exit 1
fi
printf 'argv_protected_path_refusal=PASS status=%s class=application-support before_dispatch=1\n' \
	"$ARGV_REFUSAL_STATUS"

ARGV_ALLOWED_LOG="$TEST_HOME/argv-allowed.log"
set +e
(cd "$ARGV_HOME" && env HOME="$ARGV_HOME" TMPDIR="$TEST_TMP_BASE" \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$ARGV_HOME" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	"$APP" -quit -- "./uploads/x") >"$ARGV_ALLOWED_LOG" 2>&1
ARGV_ALLOWED_STATUS=$?
set -e
if [[ "$ARGV_ALLOWED_STATUS" -ne 0 ]] \
	|| grep -F -q "callsite=launcher.argv" "$ARGV_ALLOWED_LOG"; then
	echo "safe command-line path was refused or failed to dispatch." >&2
	cat "$ARGV_ALLOWED_LOG" >&2
	exit 1
fi
printf 'argv_profile_relative_path_allowed=PASS status=%s\n' "$ARGV_ALLOWED_STATUS"

QUIT_LOG="$TEST_HOME/quit.log"
set +e
env HOME="$TEST_HOME" TMPDIR="$TEST_TMP_BASE" TDESKTOP_MAC_PROFILE_TEST_HOME="$TEST_HOME" TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 "$APP" -quit >"$QUIT_LOG" 2>&1
QUIT_STATUS=$?
set -e
report_result second_instance_exit "$([[ "$QUIT_STATUS" -eq 0 ]] && printf true || printf false)" \
	"status=$QUIT_STATUS" || FAILURES=$((FAILURES + 1))
QUIT_RESPONSE="$(grep -hF \
	"Show command response received, processId = $FIRST_PID, windowId = 0" \
	"$PROFILE"/log*.txt || true)"
if [[ -n "$QUIT_RESPONSE" ]]; then
	QUIT_HANDSHAKE_READY=true
else
	QUIT_HANDSHAKE_READY=false
fi
report_result second_instance_quit_handshake "$QUIT_HANDSHAKE_READY" \
	"response=$QUIT_RESPONSE expected_pid=$FIRST_PID" || FAILURES=$((FAILURES + 1))
if (( FAILURES > 0 )); then
	cat "$QUIT_LOG" >&2
	cat "$START_LOG" >&2
	exit 1
fi

for ((attempt = 0; attempt < 150; ++attempt)); do
	if ! kill -0 "$FIRST_PID" 2>/dev/null; then
		break
	fi
	sleep 0.2
done
if kill -0 "$FIRST_PID" 2>/dev/null; then
	report_result first_instance_exit false "pid=$FIRST_PID still_alive=1" || true
	echo "second-instance quit request did not stop the first instance." >&2
	exit 1
fi
FIRST_INSTANCE_PID="$FIRST_PID"
set +e
wait "$FIRST_PID"
FIRST_STATUS=$?
set -e
FIRST_PID=""
report_result first_instance_exit "$([[ "$FIRST_STATUS" -eq 0 ]] && printf true || printf false)" \
	"pid=$FIRST_INSTANCE_PID status=$FIRST_STATUS" || FAILURES=$((FAILURES + 1))
if (( FAILURES > 0 )); then
	cat "$START_LOG" >&2
	exit 1
fi

ACCOUNT_STATE_HASH_AFTER="$(shasum -a 256 "$ACCOUNT_STATE" | awk '{print $1}')"
ENDPOINT_ENROLLMENT_HASH_AFTER="$(shasum -a 256 "$ENDPOINT_ENROLLMENT" | awk '{print $1}')"
if [[ "$ACCOUNT_STATE_HASH_AFTER" != "$ACCOUNT_STATE_HASH" \
	|| "$ENDPOINT_ENROLLMENT_HASH_AFTER" != "$ENDPOINT_ENROLLMENT_HASH" \
	|| -e "$TELEGRAMD_PROFILE" ]]; then
	echo "startup changed the existing Teagram profile or created a replacement profile." >&2
	exit 1
fi
printf 'existing_profile_preserved=PASS account_state_sha256_before=%s account_state_sha256_after=%s endpoint_enrollment_sha256_before=%s endpoint_enrollment_sha256_after=%s replacement_profile_absent=1\n' \
	"$ACCOUNT_STATE_HASH" "$ACCOUNT_STATE_HASH_AFTER" \
	"$ENDPOINT_ENROLLMENT_HASH" "$ENDPOINT_ENROLLMENT_HASH_AFTER"

CACHE_TEXT="$PROFILE/tdata/emoji/spoiler/text"
CACHE_TEXT_FILES="$(find "$TEST_HOME" -type f -path '*/tdata/emoji/spoiler/text' -print)"
report_result spoiler_cache_location "$([[ -f "$CACHE_TEXT" && "$CACHE_TEXT_FILES" == "$CACHE_TEXT" ]] && printf true || printf false)" \
	"expected=$CACHE_TEXT found=${CACHE_TEXT_FILES:-missing}" || FAILURES=$((FAILURES + 1))
if (( FAILURES > 0 )); then
	exit 1
fi
}

prepare_authenticated_storage_fixtures() {
	local storage_fixture_root="$1"
	local fixture
	for fixture in \
		cache-root \
		media-cache-root \
		cache-leaf \
		media-cache-leaf \
		opened-cache \
		opened-media-cache \
		legacy-cleanup; do
		mkdir -p "$storage_fixture_root/$fixture"
		printf '%s' 'synthetic protected fixture' \
			> "$storage_fixture_root/$fixture/marker"
	done
	printf '%s' 'legacy bytes' \
		> "$storage_fixture_root/legacy-cleanup/unrecognized-legacy-file"
}

verify_storage_fixture() {
	local directory="$1"
	local expected="$(printf '%s\n' "$directory/marker")"
	local entries="$(find "$directory" -mindepth 1 -maxdepth 1 -print | LC_ALL=C sort)"
	[[ "$entries" == "$expected" ]] \
		&& [[ "$(cat "$directory/marker")" == 'synthetic protected fixture' ]]
}

verify_protected_storage_fixtures() {
	local storage_fixture_root="$1"
	local fixture
	for fixture in \
		cache-root \
		media-cache-root \
		cache-leaf \
		media-cache-leaf \
		opened-cache \
		opened-media-cache; do
		if ! verify_storage_fixture "$storage_fixture_root/$fixture"; then
			echo "protected_storage_fixture=FAIL fixture=$fixture"
			return 1
		fi
	done
	local cleanup="$storage_fixture_root/legacy-cleanup"
	local expected="$(printf '%s\n%s\n' \
		"$cleanup/marker" "$cleanup/unrecognized-legacy-file" | LC_ALL=C sort)"
	local entries="$(find "$cleanup" -mindepth 1 -maxdepth 1 -print | LC_ALL=C sort)"
	if [[ "$entries" != "$expected" ]] \
		|| [[ "$(cat "$cleanup/marker")" != 'synthetic protected fixture' ]] \
		|| [[ "$(cat "$cleanup/unrecognized-legacy-file")" != 'legacy bytes' ]]; then
		echo "protected_storage_fixture=FAIL fixture=legacy-cleanup"
		return 1
	fi
	echo "protected_storage_fixtures=PASS cache=6 legacy_cleanup=1"
}

run_authenticated_cache_regression() {
local AUTH_HOME="$TEST_HOME/authenticated-cache-regression"
local AUTH_LOG="$TEST_HOME/authenticated-cache-regression.log"
local AUTH_EVIDENCE_LOG="${TDESKTOP_MAC_PROFILE_EVIDENCE_LOG:-}"
local STORAGE_FIXTURE_ROOT="$AUTH_HOME/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/SyntheticStorageFixtures"
local PROFILE_LOG="$AUTH_HOME/Library/Application Support/Teagram/log.txt"
mkdir -p "$AUTH_HOME"
prepare_authenticated_storage_fixtures "$STORAGE_FIXTURE_ROOT"
append_ui_stall_diagnostics() {
	if [[ -f "$PROFILE_LOG" ]]; then
		grep -F 'UI stall ' "$PROFILE_LOG" >> "$AUTH_LOG" || true
	fi
}

if ! env HOME="$AUTH_HOME" TMPDIR="$TEST_TMP_BASE" \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$AUTH_HOME" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	TDESKTOP_AUTH_LIFECYCLE_REGRESSION=1 \
	TDESKTOP_AUTH_LIFECYCLE_REGRESSION_CACHE_ONLY=1 \
	TDESKTOP_UI_STALL_WATCHDOG_CACHE_REGRESSION=1 \
	"$APP" -noupdate -debug >"$AUTH_LOG" 2>&1; then
	append_ui_stall_diagnostics
	verify_protected_storage_fixtures "$STORAGE_FIXTURE_ROOT" || true
	cat "$AUTH_LOG" >&2
	echo "authenticated cache and cleanup regression failed." >&2
	exit 1
fi
append_ui_stall_diagnostics
verify_protected_storage_fixtures "$STORAGE_FIXTURE_ROOT"
for native_evidence in \
	'native_lifecycle_case=account-lifecycle-prerequisites result=PASS' \
	'native_lifecycle_case=legacy-cleanup-symlink result=PASS' \
	'native_lifecycle_case=cache-callback-timeout result=PASS' \
	'native_lifecycle_case=account-authorization-lifecycle result=PASS' \
	'native_lifecycle_case=chat-participants-prerequisites result=PASS' \
	'native_lifecycle_case=cache-concurrent-deletion result=PASS' \
	'native_lifecycle_case=authenticated-cache-file-symlinks result=PASS' \
	'native_lifecycle_case=cache-path-watchdog-regression result=PASS' \
	'native_lifecycle_case=post-open-cache result=PASS' \
	'native_lifecycle_case=post-open-media-cache result=PASS' \
	'native_lifecycle_case=logout-before-cache-initialization result=PASS' \
	'native_lifecycle_case=logout-before-media-cache-initialization result=PASS' \
	'native_lifecycle_case=failed-init-cache-access result=PASS' \
	'native_lifecycle_case=failed-init-media-cache-access result=PASS' \
	'native_lifecycle_case=cache-stats-initialization result=PASS' \
	'native_lifecycle_case=media-cache-stats-initialization result=PASS' \
	'native_lifecycle_case=cold-post-open-cache result=PASS' \
	'native_lifecycle_case=cold-post-open-media-cache result=PASS' \
	'native_lifecycle_case=cache-access-no-walk result=PASS' \
	'native_lifecycle_case=media-cache-access-no-walk result=PASS' \
	'native_lifecycle_case=protected-cache-cleanup result=PASS' \
	'native_lifecycle_cases_failed=none'; do
	if ! grep -F -q "$native_evidence" "$AUTH_LOG"; then
		cat "$AUTH_LOG" >&2
		printf 'native lifecycle case result missing: %s\n' "$native_evidence" >&2
		exit 1
	fi
done
local watchdog_evidence
watchdog_evidence="$(grep -E \
	'^Cache watchdog regression: seeded_directories=125 seeded_files=169 cache_accesses=292 duration_ns=[0-9]+ stalls=[0-9]+\.$' \
	"$AUTH_LOG" || true)"
if [[ -z "$watchdog_evidence" ]]; then
	cat "$AUTH_LOG" >&2
	echo "seeded cache watchdog regression evidence missing." >&2
	exit 1
fi
local watchdog_duration
watchdog_duration="$(sed -E \
	's/.* duration_ns=([0-9]+) stalls=.*/\1/' <<<"$watchdog_evidence")"
local watchdog_stalls
watchdog_stalls="$(sed -E \
	's/.* duration_ns=[0-9]+ stalls=([0-9]+)\./\1/' <<<"$watchdog_evidence")"
if (( watchdog_duration < 30000000 && watchdog_stalls == 0 )); then
	printf 'cache_watchdog_responsive_path=PASS %s\n' "$watchdog_evidence"
else
	cat "$AUTH_LOG" >&2
	cat "$PROFILE_LOG" >&2
	echo "seeded cache watchdog scenario exceeded the 30ms access budget or reported a UI stall." >&2
	exit 1
fi
if ! grep -F -q \
	"Authenticated cache regression passed: root, directory, and file symlinks refused for cache and media_cache." \
	"$AUTH_LOG"; then
	cat "$AUTH_LOG" >&2
	echo "authenticated cache regression did not report coverage." >&2
	exit 1
fi
for cache in cache media_cache; do
	if ! grep -E -q \
		"^Cache access latency regression: cache=$cache seeded_directories=125 seeded_files=169 cache_initialization_ns=[0-9]+ initialization_allowed=1 queued_ops_before_ready=1 queued_ops_passed=1 cache_usable_ns=[0-9]+ under_10s=1 max_main_thread_stall_ns=[0-9]+ under_100ms=1 first_access_ns=[0-9]+ under_50ms=1 root_swap_refused=1 direct_fallback_closed=1 root_restored=1 startup_walks=2 main_thread_walks=0\\.$" \
		"$AUTH_LOG"; then
		cat "$AUTH_LOG" >&2
		printf 'seeded cache access latency evidence missing for %s.\n' "$cache" >&2
		exit 1
	fi
done
if ! grep -E -q \
	'^First messages\.getDialogs handler latency regression: seeded_directories=125 seeded_files=169 requests=1 parsed=1 chats=2 dialogs=1 messages=73 documents=73 cache_walks=2 max_handler_ns=[0-9]+ under_50ms=1\.$' \
	"$AUTH_LOG"; then
	cat "$AUTH_LOG" >&2
	echo "first messages.getDialogs handler evidence missing or over 50 ms." >&2
	exit 1
fi
if ! grep -E -q \
	'^Cache chat switch latency regression: seeded_directories=125 seeded_files=169 reactions=73 hydrated_documents=292 chat_switches=73 max_hydration_ns=[0-9]+ max_switch_ns=[0-9]+ max_stall_ns=[0-9]+ under_50ms=1\.$' \
	"$AUTH_LOG"; then
	cat "$AUTH_LOG" >&2
	echo "cache chat switch latency evidence missing or over 50 ms." >&2
	exit 1
fi
if ! grep -F -q \
	"Authenticated refused download history regression passed: entry without a live message remained unpublished." \
	"$AUTH_LOG"; then
	cat "$AUTH_LOG" >&2
	echo "refused download-history regression did not report coverage." >&2
	exit 1
fi
if ! grep -F -q \
	"Mac protected path refusal: operation=read class=group-container callsite=download-history.resolve" \
	"$AUTH_LOG"; then
	cat "$AUTH_LOG" >&2
	echo "refused download-history fixture did not reach manager resolution." >&2
	exit 1
fi
if ! grep -F -q "Authenticated cache regression cases passed." "$AUTH_LOG"; then
	cat "$AUTH_LOG" >&2
	echo "authenticated cache regression did not report its passing result." >&2
	exit 1
fi
if [[ -n "$AUTH_EVIDENCE_LOG" ]]; then
	mkdir -p "$(dirname "$AUTH_EVIDENCE_LOG")"
	cp "$AUTH_LOG" "$AUTH_EVIDENCE_LOG"
	if ! grep -F -q "Authenticated cache regression cases passed." "$AUTH_EVIDENCE_LOG"; then
		echo "authenticated app log evidence missed the cache regression assertion." >&2
		exit 1
	fi
	printf 'authenticated_app_log_evidence=PASS path=%s cache_cases=PASS\n' \
		"$AUTH_EVIDENCE_LOG"
fi
printf 'authenticated_cache_regression=PASS roots=2 cache_directory_symlinks=2 cache_file_symlinks=2 cleanup_initial_symlink=1 cleanup_worker_symlink=1 download_history=PASS\n'
}

run_authenticated_lifecycle_regression() {
local AUTH_HOME="$TEST_HOME/authenticated-lifecycle-regression"
local AUTH_LOG="$TEST_HOME/authenticated-lifecycle-regression.log"
local STORAGE_FIXTURE_ROOT="$AUTH_HOME/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/SyntheticStorageFixtures"
mkdir -p "$AUTH_HOME"
prepare_authenticated_storage_fixtures "$STORAGE_FIXTURE_ROOT"

if ! env HOME="$AUTH_HOME" TMPDIR="$TEST_TMP_BASE" \
	TDESKTOP_MAC_PROFILE_TEST_HOME="$AUTH_HOME" \
	TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
	TDESKTOP_AUTH_LIFECYCLE_REGRESSION=1 \
	"$APP" -noupdate -debug >"$AUTH_LOG" 2>&1; then
	verify_protected_storage_fixtures "$STORAGE_FIXTURE_ROOT" || true
	cat "$AUTH_LOG" >&2
	echo "authenticated lifecycle regression failed." >&2
	exit 1
fi
verify_protected_storage_fixtures "$STORAGE_FIXTURE_ROOT"
if ! grep -E -q \
	'^Translation action routing: custom_provider=[0-3] custom_hidden=[01] custom_rich_hidden=1 local_failure=[01] stock_provider=[1-3] stock_hidden=0\.$' \
	"$AUTH_LOG"; then
	echo "translation action routing evidence missing or invalid." >&2
	cat "$AUTH_LOG" >&2
	exit 1
fi
for translation_evidence in \
	"Translation controls precondition: stock loaded=1 server_supported=1 network_allowed=0 creator=1 broadcast=1 auto_translation=1 translation_enabled=1; custom loaded=1 server_supported=0 network_allowed=0 creator=1 broadcast=1 auto_translation=1 translation_enabled=1." \
	"Translation controls custom requests: text=0 rich=0." \
	"Translation controls stock requests: text=1 rich=1." \
	"Translation controls counts unchanged after 1000 ms main-queue barrier: custom text=0 rich=0; stock text=1 rich=1; network_allowed stock=0 custom=0." \
	"Translation controls regression passed: custom action gates followed session provider availability, rich actions stayed hidden, stale actions failed locally, Language links skipped absent rows without writing preferences, automatic chat translation stayed off, and stock controls and RPCs remained enabled."; do
	if ! grep -F -q "$translation_evidence" "$AUTH_LOG"; then
		echo "translation controls regression evidence missing: $translation_evidence" >&2
		cat "$AUTH_LOG" >&2
		exit 1
	fi
done
if ! awk '
/Translation action routing: / && step == 0 { step = 1; next }
/Translation controls precondition: / && step == 1 { step = 2; next }
/Translation controls custom requests: / && step == 2 { step = 3; next }
/Translation controls stock requests: / && step == 3 { step = 4; next }
/Translation controls counts unchanged after 1000 ms main-queue barrier: / && step == 4 { step = 5; next }
/Translation controls regression passed: / && step == 5 { step = 6; next }
END { exit step != 6 }
' "$AUTH_LOG"; then
	echo "translation controls regression evidence was out of order." >&2
	exit 1
fi
for native_evidence in \
	'native_lifecycle_case=settings-search result=PASS' \
	'native_lifecycle_case=session-window-lifecycle result=PASS' \
	'native_lifecycle_case=translation-controls result=PASS' \
	'native_lifecycle_case=manage-controls result=PASS' \
	'native_lifecycle_case=passport-link-refusal result=PASS' \
	'native_lifecycle_case=mini-app-open result=PASS' \
	'native_lifecycle_case=chatlist-invite-check result=PASS' \
	'native_lifecycle_case=authenticated-cache-file-symlinks result=PASS' \
	'native_lifecycle_case=post-open-cache result=PASS' \
	'native_lifecycle_case=post-open-media-cache result=PASS' \
	'native_lifecycle_case=logout-before-cache-initialization result=PASS' \
	'native_lifecycle_case=logout-before-media-cache-initialization result=PASS' \
	'native_lifecycle_case=failed-init-cache-access result=PASS' \
	'native_lifecycle_case=failed-init-media-cache-access result=PASS' \
	'native_lifecycle_case=cache-stats-initialization result=PASS' \
	'native_lifecycle_case=media-cache-stats-initialization result=PASS' \
	'native_lifecycle_case=cold-post-open-cache result=PASS' \
	'native_lifecycle_case=cold-post-open-media-cache result=PASS' \
	'native_lifecycle_case=cache-access-no-walk result=SKIP reason=dedicated-cache-regression' \
	'native_lifecycle_case=media-cache-access-no-walk result=SKIP reason=dedicated-cache-regression' \
	'native_lifecycle_case=folder-crud result=PASS' \
	'native_lifecycle_case=stock-basic-group-creator result=PASS' \
	'native_lifecycle_case=pinned-basic-group-creator result=PASS' \
	'native_lifecycle_cases_failed=none'; do
	if ! grep -F -q "$native_evidence" "$AUTH_LOG"; then
		cat "$AUTH_LOG" >&2
		printf 'native lifecycle case result missing: %s\n' "$native_evidence" >&2
		exit 1
	fi
done
for cache_evidence in \
	"Authenticated cache regression passed: root, directory, and file symlinks refused for cache and media_cache." \
	"Authenticated refused download history regression passed: entry without a live message remained unpublished." \
	"Mac protected path refusal: operation=read class=group-container callsite=download-history.resolve" \
	"Chat participants regression passed." \
	"Settings deep link regression passed: paid and story routes refused in custom and blocked sessions;" \
	"Paid link regression passed: 16 paid links refused in custom and blocked sessions"; do
	if ! grep -F -q "$cache_evidence" "$AUTH_LOG"; then
		cat "$AUTH_LOG" >&2
		printf 'authenticated lifecycle evidence missing: %s\n' "$cache_evidence" >&2
		exit 1
	fi
done
printf 'authenticated_lifecycle_regression=PASS full_suite=PASS cache=PASS\n'
}

run_native_failure_continuation_case() {
	local case_home="$TEST_HOME/native-failure-continuation"
	local case_log="$case_home.log"
	local exit_status=0
	local fixture_failure_home="$TEST_HOME/native-identity-and-fixture-failure"
	local fixture_failure_log="$fixture_failure_home.log"
	local fixture_failure_status=0
	local promotion_failure_home="$TEST_HOME/native-promotion-failure"
	local promotion_failure_log="$promotion_failure_home.log"
	local promotion_failure_status=0
	mkdir -p "$case_home"
	if env HOME="$case_home" TMPDIR="$TEST_TMP_BASE" \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$case_home" \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		TDESKTOP_AUTH_LIFECYCLE_REGRESSION=1 \
		TDESKTOP_AUTH_LIFECYCLE_REGRESSION_FAIL_CASE=username-input,teagram-icon-persistence,legacy-cleanup-symlink,cache-callback-timeout,cache-concurrent-deletion,stock-incoming-call-answer,pinned-incoming-call-answer,settings-search,folder-crud,admin-session-identities \
		"$APP" -noupdate -debug >"$case_log" 2>&1; then
		exit_status=0
	else
		exit_status=$?
	fi
	if (( exit_status == 0 )); then
		cat "$case_log" >&2
		echo "native lifecycle failure injection unexpectedly passed." >&2
		return 1
	fi
	for evidence in \
		'native_lifecycle_case=username-input status=START' \
		'native_lifecycle_case=username-input result=FAIL' \
		'native_lifecycle_case=teagram-icon-persistence status=START' \
		'native_lifecycle_case=teagram-icon-persistence result=FAIL' \
		'native_lifecycle_case=account-lifecycle-prerequisites result=PASS' \
		'native_lifecycle_case=legacy-cleanup-symlink result=FAIL' \
		'native_lifecycle_case=cache-callback-timeout result=FAIL' \
		'native_lifecycle_case=account-authorization-lifecycle result=PASS' \
		'native_lifecycle_case=cache-concurrent-deletion status=START' \
		'native_lifecycle_case=cache-concurrent-deletion result=FAIL' \
		'native_lifecycle_case=settings-search status=START' \
		'native_lifecycle_case=settings-search result=FAIL' \
		'native_lifecycle_case=session-window-lifecycle status=START' \
		'native_lifecycle_case=session-window-lifecycle result=' \
		'native_lifecycle_case=translation-controls status=START' \
		'native_lifecycle_case=translation-controls result=' \
		'native_lifecycle_case=manage-controls status=START' \
		'native_lifecycle_case=manage-controls result=' \
		'native_lifecycle_case=passport-link-refusal status=START' \
		'native_lifecycle_case=passport-link-refusal result=PASS' \
		'native_lifecycle_case=authenticated-cache-file-symlinks status=START' \
		'native_lifecycle_case=authenticated-cache-file-symlinks result=' \
		'native_lifecycle_case=cache-access-no-walk status=START' \
		'native_lifecycle_case=cache-access-no-walk result=' \
		'native_lifecycle_case=media-cache-access-no-walk status=START' \
		'native_lifecycle_case=media-cache-access-no-walk result=' \
		'native_lifecycle_case=logout-before-cache-initialization result=PASS' \
		'native_lifecycle_case=logout-before-media-cache-initialization result=PASS' \
		'native_lifecycle_case=calls-instance-start result=PASS' \
		'native_lifecycle_case=stock-incoming-call-answer status=START' \
		'native_lifecycle_case=stock-incoming-call-answer result=FAIL' \
		'native_lifecycle_case=pinned-incoming-call-answer status=START' \
		'native_lifecycle_case=pinned-incoming-call-answer result=FAIL' \
		'native_lifecycle_case=call-session-fixtures result=PASS' \
		'native_lifecycle_case=folder-crud status=START' \
		'native_lifecycle_case=folder-crud result=FAIL' \
		'native_lifecycle_case=admin-session-identities status=START' \
		'native_lifecycle_case=admin-session-identities result=FAIL' \
		'native_lifecycle_case=stock-basic-group-creator status=START' \
		'native_lifecycle_case=stock-basic-group-creator result=PASS' \
		'native_lifecycle_case=pinned-basic-group-creator status=START' \
		'native_lifecycle_case=pinned-basic-group-creator result=PASS' \
		'native_lifecycle_case=stock-full-list-admin result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=stock-full-list-demotion result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=custom-full-list-admin result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=stock-live-admin-promotion result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=stock-live-admin-demotion result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=custom-live-admin-promotion result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=custom-live-admin-demotion result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=pinned-migration-fixture status=START' \
		'native_lifecycle_case=pinned-migration-fixture result=PASS' \
		'native_lifecycle_case=post-close-window-lifecycle result=' \
		'native_lifecycle_case=chat-migration result=PASS'; do
		if ! grep -F -q "$evidence" "$case_log"; then
			cat "$case_log" >&2
			printf 'native lifecycle failure injection missed: %s\n' \
				"$evidence" >&2
			return 1
		fi
	done
	if ! grep -E -q \
		'^native_lifecycle_cases_failed=.*username-input.*teagram-icon-persistence.*legacy-cleanup-symlink.*cache-callback-timeout.*cache-concurrent-deletion.*stock-incoming-call-answer.*pinned-incoming-call-answer.*settings-search.*folder-crud.*admin-session-identities' "$case_log"; then
		cat "$case_log" >&2
		echo "native lifecycle failure summary missed the injected failure cases." >&2
		return 1
	fi
	if ! awk '
/native_lifecycle_case=username-input status=START/ && step == 0 { step = 1; next }
/native_lifecycle_case=username-input result=FAIL/ && step == 1 { step = 2; next }
/native_lifecycle_case=teagram-icon-persistence status=START/ && step == 2 { step = 3; next }
/native_lifecycle_case=teagram-icon-persistence result=FAIL/ && step == 3 { step = 4; next }
/native_lifecycle_case=account-lifecycle-prerequisites result=PASS/ && step == 4 { step = 5; next }
/native_lifecycle_case=legacy-cleanup-symlink status=START/ && step == 5 { step = 6; next }
/native_lifecycle_case=legacy-cleanup-symlink result=FAIL/ && step == 6 { step = 7; next }
/native_lifecycle_case=cache-callback-timeout status=START/ && step == 7 { step = 8; next }
/native_lifecycle_case=cache-callback-timeout result=FAIL/ && step == 8 { step = 9; next }
/native_lifecycle_case=account-authorization-lifecycle status=START/ && step == 9 { step = 10; next }
/native_lifecycle_case=account-authorization-lifecycle result=PASS/ && step == 10 { step = 11; next }
/native_lifecycle_case=cache-concurrent-deletion status=START/ && step == 11 { step = 12; next }
/native_lifecycle_case=cache-concurrent-deletion result=FAIL/ && step == 12 { step = 13; next }
/native_lifecycle_case=stock-incoming-call-answer status=START/ && step == 13 { step = 14; next }
/native_lifecycle_case=stock-incoming-call-answer result=FAIL/ && step == 14 { step = 15; next }
/native_lifecycle_case=pinned-incoming-call-answer status=START/ && step == 15 { step = 16; next }
/native_lifecycle_case=pinned-incoming-call-answer result=FAIL/ && step == 16 { step = 17; next }
/native_lifecycle_case=settings-search status=START/ && step == 17 { step = 18; next }
/native_lifecycle_case=settings-search result=FAIL/ && step == 18 { step = 19; next }
/native_lifecycle_case=session-window-lifecycle status=START/ && step == 19 { step = 20; next }
/native_lifecycle_case=session-window-lifecycle result=/ && step == 20 { step = 21; next }
/native_lifecycle_case=translation-controls status=START/ && step == 21 { step = 22; next }
/native_lifecycle_case=translation-controls result=/ && step == 22 { step = 23; next }
/native_lifecycle_case=manage-controls status=START/ && step == 23 { step = 24; next }
/native_lifecycle_case=manage-controls result=/ && step == 24 { step = 25; next }
/native_lifecycle_case=passport-link-refusal status=START/ && step == 25 { step = 26; next }
/native_lifecycle_case=passport-link-refusal result=PASS/ && step == 26 { step = 27; next }
/native_lifecycle_case=authenticated-cache-file-symlinks status=START/ && step == 27 { step = 28; next }
/native_lifecycle_case=authenticated-cache-file-symlinks result=/ && step == 28 { step = 29; next }
/native_lifecycle_case=cache-access-no-walk status=START/ && step == 29 { step = 30; next }
/native_lifecycle_case=cache-access-no-walk result=/ && step == 30 { step = 31; next }
/native_lifecycle_case=media-cache-access-no-walk status=START/ && step == 31 { step = 32; next }
/native_lifecycle_case=media-cache-access-no-walk result=/ && step == 32 { step = 33; next }
/native_lifecycle_case=folder-crud status=START/ && step == 33 { step = 34; next }
/native_lifecycle_case=folder-crud result=FAIL/ && step == 34 { step = 35; next }
/native_lifecycle_case=admin-session-identities status=START/ && step == 35 { step = 36; next }
/native_lifecycle_case=admin-session-identities result=FAIL/ && step == 36 { step = 37; next }
/native_lifecycle_case=pinned-migration-fixture status=START/ && step == 37 { step = 38; next }
/native_lifecycle_case=pinned-migration-fixture result=PASS/ && step == 38 { step = 39; next }
/native_lifecycle_case=stock-basic-group-creator status=START/ && step == 39 { step = 40; next }
/native_lifecycle_case=stock-basic-group-creator result=PASS/ && step == 40 { step = 41; next }
/native_lifecycle_case=pinned-basic-group-creator status=START/ && step == 41 { step = 42; next }
/native_lifecycle_case=pinned-basic-group-creator result=PASS/ && step == 42 { step = 43; next }
/native_lifecycle_case=post-close-window-lifecycle result=/ && step == 43 { step = 44; next }
/native_lifecycle_case=chat-migration result=PASS/ && step == 44 { step = 45; next }
END { exit step != 45 }
' "$case_log"; then
		cat "$case_log" >&2
		echo "native lifecycle cases did not continue around the early and late failures." >&2
		return 1
	fi
	printf 'native_lifecycle_failure_continuation=PASS tested_revision=%s exit_status=%d failed_cases=username-input,teagram-icon-persistence,legacy-cleanup-symlink,cache-callback-timeout,cache-concurrent-deletion,stock-incoming-call-answer,pinned-incoming-call-answer,settings-search,folder-crud,admin-session-identities later_cases=translation-controls,manage-controls,authenticated-cache-file-symlinks,failed-init-cache-access,failed-init-media-cache-access,cold-post-open-cache,cold-post-open-media-cache,cache-access-no-walk,media-cache-access-no-walk,logout-before-cache-initialization,logout-before-media-cache-initialization,independent-creator-checks,admin-session-identities-skips,chat-migration-pass,post-close-window-lifecycle\n' \
		"$TESTED_REVISION" "$exit_status"
	mkdir -p "$fixture_failure_home"
	if env HOME="$fixture_failure_home" TMPDIR="$TEST_TMP_BASE" \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$fixture_failure_home" \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		TDESKTOP_AUTH_LIFECYCLE_REGRESSION=1 \
		TDESKTOP_AUTH_LIFECYCLE_REGRESSION_FAIL_CASE=admin-session-identities,pinned-migration-fixture \
		"$APP" -noupdate -debug >"$fixture_failure_log" 2>&1; then
		fixture_failure_status=0
	else
		fixture_failure_status=$?
	fi
	if (( fixture_failure_status == 0 )); then
		cat "$fixture_failure_log" >&2
		echo "combined identity and migration fixture failures unexpectedly passed." >&2
		return 1
	fi
	for evidence in \
		'native_lifecycle_case=admin-session-identities result=FAIL' \
		'native_lifecycle_case=stock-basic-group-creator result=PASS' \
		'native_lifecycle_case=pinned-migration-fixture result=FAIL' \
		'native_lifecycle_case=pinned-basic-group-creator result=SKIP reason=dependency-pinned-migration-fixture' \
		'native_lifecycle_case=stock-full-list-admin result=SKIP reason=dependency-admin-session-identities' \
		'native_lifecycle_case=chat-migration result=SKIP reason=dependency-pinned-migration-fixture'; do
		if ! grep -F -q "$evidence" "$fixture_failure_log"; then
			cat "$fixture_failure_log" >&2
			printf 'combined native failure evidence missing: %s\n' \
				"$evidence" >&2
			return 1
		fi
	done
	if grep -F -q \
		'native_lifecycle_case=chat-migration result=SKIP reason=dependency-admin-session-identities' \
		"$fixture_failure_log"; then
		cat "$fixture_failure_log" >&2
		echo "chat migration skip reported the wrong dependency." >&2
		return 1
	fi
	printf 'native_lifecycle_dependency_reporting=PASS combined_failures=admin-session-identities,pinned-migration-fixture stock_creator=PASS admin_cases=SKIP chat_migration=SKIP:dependency-pinned-migration-fixture\n'
	mkdir -p "$promotion_failure_home"
	if env HOME="$promotion_failure_home" TMPDIR="$TEST_TMP_BASE" \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$promotion_failure_home" \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		TDESKTOP_AUTH_LIFECYCLE_REGRESSION=1 \
		TDESKTOP_AUTH_LIFECYCLE_REGRESSION_FAIL_CASE=stock-live-admin-promotion,custom-live-admin-promotion \
		"$APP" -noupdate -debug >"$promotion_failure_log" 2>&1; then
		promotion_failure_status=0
	else
		promotion_failure_status=$?
	fi
	if (( promotion_failure_status == 0 )); then
		cat "$promotion_failure_log" >&2
		echo "live-admin promotion failure injection unexpectedly passed." >&2
		return 1
	fi
	for evidence in \
		'native_lifecycle_case=stock-live-admin-promotion result=FAIL' \
		'native_lifecycle_case=stock-live-admin-demotion result=SKIP reason=dependency-stock-live-admin-promotion' \
		'native_lifecycle_case=custom-live-admin-promotion result=FAIL' \
		'native_lifecycle_case=custom-live-admin-demotion result=SKIP reason=dependency-custom-live-admin-promotion' \
		'native_lifecycle_case=post-close-window-lifecycle result=' \
		'native_lifecycle_case=chat-migration result=PASS'; do
		if ! grep -F -q "$evidence" "$promotion_failure_log"; then
			cat "$promotion_failure_log" >&2
			printf 'live-admin promotion evidence missing: %s\n' \
				"$evidence" >&2
			return 1
		fi
	done
	if ! grep -E -q \
		'^native_lifecycle_cases_failed=.*stock-live-admin-promotion.*custom-live-admin-promotion' \
		"$promotion_failure_log"; then
		cat "$promotion_failure_log" >&2
		echo "live-admin promotion failures were omitted from the summary." >&2
		return 1
	fi
	printf 'native_lifecycle_promotion_dependencies=PASS failed_promotions=stock-live-admin-promotion,custom-live-admin-promotion later_cases=post-close-window-lifecycle,chat-migration\n'
}

run_authenticated_folder_fault_case() {
	local fault_mode="$1"
	local reason="$2"
	local other_present="$3"
	local case_home="$TEST_HOME/folder-fault-$fault_mode"
	local case_log="$case_home.log"
	local exit_status=0
	local diagnostic
	mkdir -p "$case_home"
	if env HOME="$case_home" TMPDIR="$TEST_TMP_BASE" \
		TDESKTOP_MAC_PROFILE_TEST_HOME="$case_home" \
		TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST=1 \
		TDESKTOP_AUTH_LIFECYCLE_REGRESSION=1 \
		TDESKTOP_FOLDER_CRUD_REGRESSION_FAULT="$fault_mode" \
		"$APP" -noupdate -debug >"$case_log" 2>&1; then
		exit_status=0
	else
		exit_status=$?
	fi
	if (( exit_status == 0 )); then
		cat "$case_log" >&2
		echo "folder fault case unexpectedly exited successfully: $fault_mode" >&2
		return 1
	fi
	if ! grep -E -q \
		"^Folder CRUD regression failed: reason=$reason side=stock other_side=pinned operation=create stage=after_local_apply synthetic_id=[1-9][0-9]* owner_present=1 other_present=$other_present$" \
		"$case_log"; then
		cat "$case_log" >&2
		echo "folder fault case missed its fail-closed diagnostic: $fault_mode" >&2
		return 1
	fi
	diagnostic="$(grep -E \
		"^Folder CRUD regression failed: reason=$reason side=stock other_side=pinned operation=create stage=after_local_apply synthetic_id=[1-9][0-9]* owner_present=1 other_present=$other_present$" \
		"$case_log")"
	printf 'folder_fault_%s=PASS tested_revision=%s exit_status=%d %s\n' \
		"$fault_mode" "$TESTED_REVISION" "$exit_status" "$diagnostic"
}

case "$CASE_NAME" in
all)
	bash "$REPO_ROOT/Telegram/build/run_mac_lifecycle_cases.sh" "$0" "$APP" \
		"$TEST_HOME/cases" \
		profile-ipc \
		spoiler-cache-emoji \
		spoiler-cache-spoiler \
		spoiler-cache-text \
		spoiler-cache-image \
		authenticated-lifecycle \
		authenticated-cache \
		native-failure-continuation \
		folder-fault-barrier-timeout \
		folder-fault-cross-session-presence
	;;
profile-ipc)
	run_protected_profile_ipc_case
	;;
spoiler-cache-emoji)
	run_spoiler_cache_symlink_case emoji-cache-symlink emoji
	;;
spoiler-cache-spoiler)
	run_spoiler_cache_symlink_case spoiler-cache-symlink spoiler
	;;
spoiler-cache-text)
	run_spoiler_cache_symlink_case text-cache-leaf-symlink text
	;;
spoiler-cache-image)
	run_spoiler_cache_symlink_case image-cache-leaf-symlink image
	;;
authenticated-cache)
	run_authenticated_cache_regression
	;;
authenticated-lifecycle)
	run_authenticated_lifecycle_regression
	;;
native-failure-continuation)
	run_native_failure_continuation_case
	;;
folder-fault-barrier-timeout)
	run_authenticated_folder_fault_case barrier_timeout barrier_timeout 0
	;;
folder-fault-cross-session-presence)
	run_authenticated_folder_fault_case cross_session_presence cross_session_presence 1
	;;
*)
	echo "unknown protected profile and IPC case: $CASE_NAME" >&2
	exit 2
	;;
esac
