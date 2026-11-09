#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
session_script="${scripts_dir}/qa-linux-desktop-session.sh"
temp_dir="$(mktemp -d)"
stub_bin="${temp_dir}/bin"
run_id="${BASHPID}-${RANDOM}-${RANDOM}"
remote_helper="/tmp/qa-linux-desktop-session-test-${run_id}.sh"
padding="$(head -c 8192 /dev/zero | /usr/bin/tr '\0' x)"
system_path="${PATH}"
declare -a session_dirs=()
declare -a remote_helpers=("$remote_helper")
hup_pid=
first_helper_pid=
second_helper_pid=

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

cleanup() {
	local proc pgrp marker child_pid helper
	for child_pid in "$hup_pid" "$first_helper_pid" "$second_helper_pid"; do
		if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
			kill -TERM "$child_pid" 2>/dev/null || true
		fi
	done
	for proc in /proc/[0-9]*; do
		[[ -r "$proc/environ" && -r "$proc/stat" ]] || continue
		/usr/bin/grep -qzFx -- "QA_SESSION_TEST_RUN_ID=$run_id" "$proc/environ" 2>/dev/null || continue
		marker="$(/usr/bin/tr '\0' '\n' < "$proc/environ" | /usr/bin/sed -n 's/^QA_SESSION_GROUP=//p')"
		[[ -n "$marker" && "$marker" != seed ]] || continue
		pgrp="$(/usr/bin/awk '{ print $5 }' "$proc/stat" 2>/dev/null || true)"
		[[ "$pgrp" =~ ^[0-9]+$ ]] && kill -TERM -- "-$pgrp" 2>/dev/null || true
	done
	for proc in "${session_dirs[@]}"; do
		rm -rf -- "$proc"
	done
	for helper in "${remote_helpers[@]}"; do
		rm -f -- "$helper"
	done
	rm -rf -- "$temp_dir"
}
trap cleanup EXIT

mkdir -p "$stub_bin"
cat > "$stub_bin/dbus-daemon" <<'STUB_DBUS_DAEMON'
#!/usr/bin/env python3
import ctypes
import sys
import time

libc = ctypes.CDLL(None, use_errno=True)
libc.prctl.argtypes = [
    ctypes.c_int,
    ctypes.c_char_p,
    ctypes.c_ulong,
    ctypes.c_ulong,
    ctypes.c_ulong,
]
libc.prctl.restype = ctypes.c_int
# Linux PR_SET_NAME keeps the stub compatible with the session identity check.
if libc.prctl(15, b"dbus-daemon", 0, 0, 0) != 0:
    raise OSError(ctypes.get_errno(), "prctl(PR_SET_NAME) failed")
time.sleep(int(sys.argv[1]))
STUB_DBUS_DAEMON

cat > "$stub_bin/id" <<'STUB_ID'
#!/usr/bin/env bash
if [[ "${1:-}" == -un ]]; then
	printf 'qa\n'
else
	exec /usr/bin/id "$@"
fi
STUB_ID

cat > "$stub_bin/xauth" <<'STUB_XAUTH'
#!/usr/bin/env bash
[[ "${1:-}" == -f && "${3:-}" == nmerge && "${4:-}" == - ]] || exit 64
cat > "$2"
STUB_XAUTH

cat > "$stub_bin/Xvfb" <<'STUB_XVFB'
#!/usr/bin/env bash
auth_file=
while (($#)); do
	if [[ "$1" == -auth ]]; then
		auth_file="${2:-}"
		shift 2
	else
		shift
	fi
done
[[ -n "$auth_file" && -s "$auth_file" ]] || {
	echo 'Xvfb did not receive an Xauthority file.' >&2
	exit 1
}
[[ "$(stat -c '%a' "$auth_file")" == 600 ]] || {
	echo 'Xauthority file permissions are not private.' >&2
	exit 1
}
/usr/bin/grep -Eq '^ffff 0000 0000 0012 4d49542d4d414749432d434f4f4b49452d31 0010 [[:xdigit:]]{32}$' "$auth_file" || {
	echo 'Xauthority file does not contain a wildcard MIT cookie.' >&2
	exit 1
}
printf '%s\n' "${QA_TEST_DISPLAY_NUMBER:-99}" >&3
exec sleep 120
STUB_XVFB

for service in openbox tint2 dunst; do
	cat > "$stub_bin/$service" <<'STUB_SERVICE'
#!/usr/bin/env bash
if [[ "${QA_TEST_ORPHAN_OPENBOX:-0}" == 1 && "${0##*/}" == openbox ]]; then
	sleep 120 &
	exit 0
fi
exec sleep 120
STUB_SERVICE
	chmod +x "$stub_bin/$service"
done

cat > "$stub_bin/Telegram" <<'STUB_TELEGRAM'
#!/usr/bin/env bash
[[ -r "${XAUTHORITY:-}" ]] || {
	echo 'Telegram did not receive its Xauthority file.' >&2
	exit 43
}
if [[ "${QA_TEST_TELEGRAM_FAIL:-0}" == 1 ]]; then
	profile="${2:-}"
	mkdir -p "$profile/DebugLogs"
	printf 'simulated Telegram client log\n' > "$profile/log.txt"
	printf 'simulated Telegram debug log\n' > "$profile/DebugLogs/client.txt"
	printf 'simulated Telegram launch failure\n' >&2
	exit 42
fi
if [[ "${QA_TEST_SLOW_TELEGRAM_TERM:-0}" == 1 ]]; then
	trap 'sleep 1; exit 0' TERM
	while :; do
		sleep 5 &
		wait "$!"
	done
fi
exec sleep 120
STUB_TELEGRAM

cat > "$stub_bin/dbus-launch" <<'STUB_DBUS_LAUNCH'
#!/usr/bin/env bash
if [[ "${QA_TEST_DBUS_FAIL:-0}" == 1 ]]; then
	printf 'simulated dbus-launch failure\n' >&2
	exit 42
fi
dbus-daemon 120 &
pid=$!
printf "DBUS_SESSION_BUS_ADDRESS='unix:path=%s'; export DBUS_SESSION_BUS_ADDRESS;\n" "${QA_TEST_DBUS_ADDRESS:?}"
printf "DBUS_SESSION_BUS_PID='%s'; export DBUS_SESSION_BUS_PID;\n" "$pid"
STUB_DBUS_LAUNCH

cat > "$stub_bin/xdotool" <<'STUB_XDOTOOL'
#!/usr/bin/env bash
[[ -r "${XAUTHORITY:-}" ]] || exit 1
case "${1:-}" in
	search)
		if [[ "${QA_TEST_XDOTOOL_FAIL:-0}" == 1 ]]; then
			printf 'simulated xdotool display failure\n' >&2
			for ((index = 1; index <= $#; index++)); do
				if [[ "${!index}" == --pid ]]; then
					pid_index=$((index + 1))
					kill -TERM "${!pid_index}" 2>/dev/null || true
					break
				fi
			done
			exit 1
		fi
		printf '12345\n'
		;;
	getwindowgeometry)
		if [[ "${2:-}" == --shell ]]; then
			if [[ "${QA_TEST_INITIAL_SCREENSHOT_FAIL:-0}" == 1 ]]; then
				count=0
				[[ -r "${QA_TEST_XDOTOOL_STATE:-}" ]] && count="$(<"$QA_TEST_XDOTOOL_STATE")"
				count=$((count + 1))
				printf '%s\n' "$count" > "$QA_TEST_XDOTOOL_STATE"
				if (( count == 2 )); then
					printf 'simulated initial geometry failure\n' >&2
					exit 1
				fi
			fi
			printf 'WINDOW=12345\nX=11\nY=22\nWIDTH=800\nHEIGHT=600\nSCREEN=0\n'
		fi
		;;
	*)
		exit 1
		;;
esac
STUB_XDOTOOL

cat > "$stub_bin/import" <<'STUB_IMPORT'
#!/usr/bin/env bash
destination="${@: -1}"
printf 'stub screenshot\n' > "$destination"
if [[ "${QA_TEST_IMPORT_FAIL:-0}" == 1 ]]; then
	printf '%s\n' "$destination" > "${QA_TEST_IMPORT_PARTIAL_PATH:?}"
	printf 'simulated import failure after partial output\n' >&2
	exit 1
fi
STUB_IMPORT

# Delay output after a matching marker to reproduce grep -q closing the pipe early.
cat > "$stub_bin/tr" <<'STUB_TR'
#!/usr/bin/env bash
if [[ $# == 2 && "$1" == '\0' && "$2" == '\n' ]]; then
	exec python3 -c '
import sys
import time

records = [part for part in sys.stdin.buffer.read().split(b"\0") if part]
match_index = next(
    (index for index, record in enumerate(records)
     if record.startswith(b"QA_SESSION_GROUP=") and record != b"QA_SESSION_GROUP=seed"),
    None,
)
if match_index is not None:
    for record in records[:match_index + 1]:
        sys.stdout.buffer.write(record + b"\n")
    sys.stdout.buffer.flush()
    time.sleep(0.2)
    try:
        for record in records[match_index + 1:]:
            sys.stdout.buffer.write(record + b"\n")
        sys.stdout.buffer.flush()
    except BrokenPipeError:
        sys.stdout = None
        raise SystemExit(1)
else:
    for record in records:
        sys.stdout.buffer.write(record + b"\n")
'
else
	exec /usr/bin/tr "$@"
fi
STUB_TR

for tool in id Xvfb openbox tint2 dunst Telegram dbus-daemon \
	dbus-launch xdotool import tr xauth; do
	chmod +x "$stub_bin/$tool"
done

run_helper() {
	local mode="$1" output="$2" status
	local -a failure_vars=()
	cp "$session_script" "$remote_helper"
	chmod 700 "$remote_helper"
	case "$mode" in
		dbus-fail) failure_vars=(QA_TEST_DBUS_FAIL=1) ;;
		telegram-fail) failure_vars=(QA_TEST_TELEGRAM_FAIL=1) ;;
		xdotool-fail) failure_vars=(QA_TEST_XDOTOOL_FAIL=1) ;;
		initial-screenshot-fail) failure_vars=(QA_TEST_INITIAL_SCREENSHOT_FAIL=1) ;;
		import-fail) failure_vars=(QA_TEST_IMPORT_FAIL=1) ;;
		orphan-openbox) failure_vars=(QA_TEST_ORPHAN_OPENBOX=1 QA_SESSION_LEADER_ATTEMPTS=0) ;;
	esac
	if [[ "$mode" == stop ]]; then
		if printf 'stop\n' | env -i \
			QA_SESSION_GROUP=seed \
			QA_SESSION_PADDING="$padding" \
			QA_SESSION_TEST_RUN_ID="$run_id" \
			QA_SESSION_TELEGRAM_BIN="$stub_bin/Telegram" \
			QA_TEST_DISPLAY_NUMBER=99 \
			QA_TEST_XDOTOOL_STATE="${temp_dir}/xdotool-${mode}.state" \
			QA_TEST_IMPORT_PARTIAL_PATH="${temp_dir}/import-partial-path" \
			QA_TEST_DBUS_ADDRESS="unix:path=${temp_dir}/session-bus" \
			"${failure_vars[@]}" \
			PATH="$stub_bin:$system_path" \
			HOME="$temp_dir" \
			bash "$remote_helper" serve > "$output" 2>&1; then
			return 0
		else
			status=$?
			return "$status"
		fi
	fi
	if env -i \
		QA_SESSION_GROUP=seed \
		QA_SESSION_PADDING="$padding" \
		QA_SESSION_TEST_RUN_ID="$run_id" \
		QA_SESSION_TELEGRAM_BIN="$stub_bin/Telegram" \
		QA_TEST_DISPLAY_NUMBER=99 \
		QA_TEST_XDOTOOL_STATE="${temp_dir}/xdotool-${mode}.state" \
		QA_TEST_IMPORT_PARTIAL_PATH="${temp_dir}/import-partial-path" \
		QA_TEST_DBUS_ADDRESS="unix:path=${temp_dir}/session-bus" \
		"${failure_vars[@]}" \
		PATH="$stub_bin:$system_path" \
		HOME="$temp_dir" \
		bash "$remote_helper" serve < /dev/null > "$output" 2>&1; then
		return 0
	else
		status=$?
		return "$status"
	fi
}

tagged_processes() {
	local proc state
	for proc in /proc/[0-9]*; do
		[[ -r "$proc/environ" ]] || continue
		state="$(awk '{ print $3 }' "$proc/stat" 2>/dev/null || true)"
		[[ "$state" != Z && "$state" != X ]] || continue
		if /usr/bin/grep -qzFx -- "QA_SESSION_TEST_RUN_ID=$run_id" "$proc/environ" 2>/dev/null; then
			printf '%s\n' "${proc##*/}"
		fi
	done
}

wait_for_ready() {
	local pid="$1" output="$2"
	for _ in {1..200}; do
		grep -Fq 'Ready for steps:' "$output" && return 0
		if ! kill -0 "$pid" 2>/dev/null; then
			cat "$output" >&2
			return 1
		fi
		sleep 0.05
	done
	cat "$output" >&2
	return 1
}

is_running() {
	local state
	[[ -r "/proc/$1/stat" ]] || return 1
	state="$(awk '{ print $3 }' "/proc/$1/stat" 2>/dev/null || true)"
	[[ -n "$state" && "$state" != Z && "$state" != X ]]
}

normal_output="${temp_dir}/normal.log"
run_helper stop "$normal_output" || {
	cat "$normal_output" >&2
	fail 'A stubbed QA session should start and stop successfully with a large environment.'
}
normal_dir="$(/usr/bin/grep -oE '/tmp/teagram-qa-session\.[[:alnum:]]{8}' "$normal_output" | head -n 1 || true)"
[[ -n "$normal_dir" ]] && session_dirs+=("$normal_dir")
telegram_pid="$(sed -n 's/.* TELEGRAM_PID=\([0-9][0-9]*\) .*/\1/p' "$normal_output" | head -n 1)"
[[ -n "$normal_dir" && -n "$telegram_pid" ]] || {
	cat "$normal_output" >&2
	fail 'The started session should report its directory and Telegram PID.'
}
grep -Fq "CLEANUP_COMPLETE=${normal_dir##*/}" "$normal_output" || {
	cat "$normal_output" >&2
	fail 'A stopped session should report successful cleanup.'
}
[[ ! -e "$normal_dir" ]] || fail "The stopped session directory remains: $normal_dir"
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after stop.'
for _ in {1..20}; do
	kill -0 "$telegram_pid" 2>/dev/null || break
	sleep 0.05
done
kill -0 "$telegram_pid" 2>/dev/null && fail "Telegram process $telegram_pid remains after stop."
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after normal cleanup.'

failure_output="${temp_dir}/dbus-failure.log"
if run_helper dbus-fail "$failure_output"; then
	cat "$failure_output" >&2
	fail 'A failing dbus-launch stub should make startup fail.'
fi
failure_dir="$(/usr/bin/grep -oE '/tmp/teagram-qa-session\.[[:alnum:]]{8}' "$failure_output" | head -n 1 || true)"

[[ -n "$failure_dir" ]] || {
	cat "$failure_output" >&2
	fail 'A failed dbus-launch should identify its session diagnostics.'
}
grep -Fq -- '--- dbus-launch.log ---' "$failure_output" || fail 'The failed D-Bus log should be emitted before cleanup.'
grep -Fq 'simulated dbus-launch failure' "$failure_output" || fail 'The D-Bus startup error should be emitted before cleanup.'
[[ ! -e "$failure_dir" ]] || fail 'Per-run data should be removed after failed-start cleanup.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after failed startup.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after failed startup cleanup.'

telegram_failure_output="${temp_dir}/telegram-failure.log"
if run_helper telegram-fail "$telegram_failure_output"; then
	cat "$telegram_failure_output" >&2
	fail 'A failing Telegram stub should make startup fail.'
fi
telegram_dir="$(/usr/bin/grep -oE '/tmp/teagram-qa-session\.[[:alnum:]]{8}' "$telegram_failure_output" | head -n 1 || true)"
[[ -n "$telegram_dir" ]] || {
	cat "$telegram_failure_output" >&2
	fail 'A failed Telegram launch should identify its session diagnostics.'
}
grep -Fq -- '--- teagram.log ---' "$telegram_failure_output" || fail 'The failed Telegram log should be emitted before cleanup.'
grep -Fq 'simulated Telegram launch failure' "$telegram_failure_output" || fail 'The Telegram startup error should be emitted before cleanup.'
grep -Fq -- '--- log.txt ---' "$telegram_failure_output" || fail 'The Telegram client log should be emitted before cleanup.'
grep -Fq 'simulated Telegram client log' "$telegram_failure_output" || fail 'The Telegram client log content should be emitted before cleanup.'
grep -Fq -- '--- client.txt ---' "$telegram_failure_output" || fail 'The Telegram debug log should be emitted before cleanup.'
grep -Fq 'simulated Telegram debug log' "$telegram_failure_output" || fail 'The Telegram debug log content should be emitted before cleanup.'
[[ ! -e "$telegram_dir" ]] || fail 'Per-run data should be removed after failed Telegram cleanup.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after failed Telegram startup.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after Telegram startup failure cleanup.'

xdotool_failure_output="${temp_dir}/xdotool-failure.log"
if run_helper xdotool-fail "$xdotool_failure_output"; then
	cat "$xdotool_failure_output" >&2
	fail 'A failing xdotool stub should make startup fail.'
fi
grep -Fq 'simulated xdotool display failure' "$xdotool_failure_output" || {
	cat "$xdotool_failure_output" >&2
	fail 'The xdotool error should be emitted before cleanup.'
}
grep -Fq 'QA session exited with status 1.' "$xdotool_failure_output" || fail 'The failing session should report its exit status.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after xdotool startup failure.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after xdotool startup failure cleanup.'

initial_screenshot_output="${temp_dir}/initial-screenshot-failure.log"
if run_helper initial-screenshot-fail "$initial_screenshot_output"; then
	cat "$initial_screenshot_output" >&2
	fail 'A failing initial screenshot stub should make startup fail.'
fi
grep -Fq 'Could not capture the initial Teagram window.' "$initial_screenshot_output" || {
	cat "$initial_screenshot_output" >&2
	fail 'Initial screenshot failure should have a specific diagnostic.'
}
grep -Fq 'simulated initial geometry failure' "$initial_screenshot_output" || fail 'The xdotool geometry error should be emitted before cleanup.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after initial screenshot failure.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after initial screenshot failure cleanup.'

import_failure_output="${temp_dir}/import-failure.log"
if run_helper import-fail "$import_failure_output"; then
	cat "$import_failure_output" >&2
	fail 'A failing import stub should make initial screenshot capture fail.'
fi
grep -Fq 'simulated import failure after partial output' "$import_failure_output" || {
	cat "$import_failure_output" >&2
	fail 'The import failure should remain visible in startup diagnostics.'
}
grep -Fq 'Could not capture the initial Teagram window.' "$import_failure_output" || fail 'A failed initial import should report screenshot capture failure.'
partial_screenshot="$(<"${temp_dir}/import-partial-path")"
[[ -n "$partial_screenshot" && ! -e "$partial_screenshot" ]] || fail 'The partial screenshot should not survive failed session cleanup.'
grep -Fq 'Ready for steps:' "$import_failure_output" && fail 'A failed initial screenshot must not report the session as ready.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after import failure.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after initial import failure cleanup.'

first_helper="/tmp/qa-linux-desktop-session-test-${run_id}-first.sh"
second_helper="/tmp/qa-linux-desktop-session-test-${run_id}-second.sh"
first_output="${temp_dir}/first-session.log"
second_output="${temp_dir}/second-session.log"
first_fifo="${temp_dir}/first-session.in"
second_fifo="${temp_dir}/second-session.in"
remote_helpers+=("$first_helper" "$second_helper")
mkfifo "$first_fifo" "$second_fifo"
cp "$session_script" "$first_helper"
cp "$session_script" "$second_helper"
chmod 700 "$first_helper" "$second_helper"
env -i \
	QA_SESSION_GROUP=seed \
	QA_SESSION_PADDING="$padding" \
	QA_SESSION_TEST_RUN_ID="$run_id" \
	QA_SESSION_TELEGRAM_BIN="$stub_bin/Telegram" \
	QA_TEST_DISPLAY_NUMBER=101 \
	QA_TEST_XDOTOOL_STATE="${temp_dir}/xdotool-first.state" \
	QA_TEST_DBUS_ADDRESS="unix:path=${temp_dir}/first-session-bus" \
	PATH="$stub_bin:$system_path" \
	HOME="$temp_dir" \
	bash "$first_helper" serve < "$first_fifo" > "$first_output" 2>&1 &
first_helper_pid=$!
exec 7>"$first_fifo"
wait_for_ready "$first_helper_pid" "$first_output" || fail 'The first concurrent session did not become ready.'
env -i \
	QA_SESSION_GROUP=seed \
	QA_SESSION_PADDING="$padding" \
	QA_SESSION_TEST_RUN_ID="$run_id" \
	QA_SESSION_TELEGRAM_BIN="$stub_bin/Telegram" \
	QA_TEST_DISPLAY_NUMBER=102 \
	QA_TEST_XDOTOOL_STATE="${temp_dir}/xdotool-second.state" \
	QA_TEST_DBUS_ADDRESS="unix:path=${temp_dir}/second-session-bus" \
	PATH="$stub_bin:$system_path" \
	HOME="$temp_dir" \
	bash "$second_helper" serve < "$second_fifo" > "$second_output" 2>&1 &
second_helper_pid=$!
exec 8>"$second_fifo"
wait_for_ready "$second_helper_pid" "$second_output" || fail 'The second concurrent session did not become ready.'
first_session="$(sed -n 's/^SESSION=\([^ ]*\).*/\1/p' "$first_output" | head -n 1)"
first_display="$(sed -n 's/^SESSION=[^ ]* DISPLAY=\([^ ]*\).*/\1/p' "$first_output" | head -n 1)"
first_profile="$(sed -n 's/^SESSION=[^ ]* DISPLAY=[^ ]* PROFILE=\([^ ]*\).*/\1/p' "$first_output" | head -n 1)"
first_telegram_pid="$(sed -n 's/.* TELEGRAM_PID=\([0-9][0-9]*\) .*/\1/p' "$first_output" | head -n 1)"
second_session="$(sed -n 's/^SESSION=\([^ ]*\).*/\1/p' "$second_output" | head -n 1)"
second_display="$(sed -n 's/^SESSION=[^ ]* DISPLAY=\([^ ]*\).*/\1/p' "$second_output" | head -n 1)"
second_profile="$(sed -n 's/^SESSION=[^ ]* DISPLAY=[^ ]* PROFILE=\([^ ]*\).*/\1/p' "$second_output" | head -n 1)"
second_telegram_pid="$(sed -n 's/.* TELEGRAM_PID=\([0-9][0-9]*\) .*/\1/p' "$second_output" | head -n 1)"
first_session_dir="/tmp/$first_session"
second_session_dir="/tmp/$second_session"
session_dirs+=("$first_session_dir" "$second_session_dir")
first_xauthority="$first_session_dir/Xauthority"
second_xauthority="$second_session_dir/Xauthority"
[[ "$first_display" == :101 && "$second_display" == :102 && "$first_display" != "$second_display" ]] || fail 'Concurrent sessions should receive distinct displays.'
[[ -n "$first_profile" && -n "$second_profile" && "$first_profile" != "$second_profile" ]] || fail 'Concurrent sessions should receive distinct profiles.'
[[ -f "$first_xauthority" && -f "$second_xauthority" && "$first_xauthority" != "$second_xauthority" ]] || fail 'Concurrent sessions should receive distinct Xauthority files.'
[[ "$(stat -c '%a' "$first_xauthority")" == 600 && "$(stat -c '%a' "$second_xauthority")" == 600 ]] || fail 'Concurrent session Xauthority files should be private.'
[[ -n "$first_telegram_pid" && -n "$second_telegram_pid" ]] || fail 'Concurrent sessions should report their Telegram PIDs.'
is_running "$first_telegram_pid" && is_running "$second_telegram_pid" || fail 'Both concurrent Telegram stubs should be running.'
printf 'stop\n' >&7
if wait "$first_helper_pid"; then
	first_helper_pid=
else
	cat "$first_output" >&2
	fail 'The first session should stop successfully.'
fi
exec 7>&-
grep -Fq "CLEANUP_COMPLETE=$first_session" "$first_output" || fail 'The first session should report successful cleanup.'
[[ ! -e "$first_session_dir" ]] || fail 'The first session tree should be removed after stop.'
is_running "$first_telegram_pid" && fail 'The first Telegram process should stop with its session.'
is_running "$second_telegram_pid" || fail 'Stopping the first session should leave the second Telegram process alive.'
printf 'stop\n' >&8
if wait "$second_helper_pid"; then
	second_helper_pid=
else
	cat "$second_output" >&2
	fail 'The second session should stop successfully.'
fi
exec 8>&-
grep -Fq "CLEANUP_COMPLETE=$second_session" "$second_output" || fail 'The second session should report successful cleanup.'
[[ ! -e "$second_session_dir" ]] || fail 'The second session tree should be removed after stop.'
is_running "$second_telegram_pid" && fail 'The second Telegram process should stop with its session.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after concurrent session cleanup.'

hup_output="${temp_dir}/hup.log"
hup_fifo="${temp_dir}/hup.in"
mkfifo "$hup_fifo"
cp "$session_script" "$remote_helper"
chmod 700 "$remote_helper"
env -i \
	QA_SESSION_GROUP=seed \
	QA_SESSION_PADDING="$padding" \
	QA_SESSION_TEST_RUN_ID="$run_id" \
	QA_SESSION_TELEGRAM_BIN="$stub_bin/Telegram" \
	QA_TEST_SLOW_TELEGRAM_TERM=1 \
	QA_TEST_DBUS_ADDRESS="unix:path=${temp_dir}/session-bus" \
	PATH="$stub_bin:$system_path" \
	HOME="$temp_dir" \
	bash "$remote_helper" serve < "$hup_fifo" > "$hup_output" 2>&1 &
hup_pid=$!
exec 9>"$hup_fifo"
wait_for_ready "$hup_pid" "$hup_output" || fail 'The session should remain ready before the HUP test.'
hup_dir="$(sed -n 's/^SESSION=\([^ ]*\).*/\1/p' "$hup_output" | head -n 1)"
hup_telegram_pid="$(sed -n 's/.* TELEGRAM_PID=\([0-9][0-9]*\) .*/\1/p' "$hup_output" | head -n 1)"
[[ -n "$hup_dir" ]] || {
	cat "$hup_output" >&2
	fail 'The HUP test session should report its directory.'
}
hup_session_dir="/tmp/$hup_dir"
session_dirs+=("$hup_session_dir")
[[ -n "$hup_telegram_pid" ]] || {
	cat "$hup_output" >&2
	fail 'The HUP test session should report its Telegram PID.'
}
kill -HUP "$hup_pid"
for _ in {1..100}; do
	grep -Fq 'QA session exited with status 129.' "$hup_output" && break
	sleep 0.01
done
grep -Fq 'QA session exited with status 129.' "$hup_output" || {
	cat "$hup_output" >&2
	fail 'The HUP teardown should report its non-zero exit status.'
}
kill -TERM "$hup_pid" 2>/dev/null || true
if wait "$hup_pid"; then
	fail 'The HUP test helper should exit with a non-zero status.'
else
	hup_status=$?
fi
hup_pid=
exec 9>&-
[[ "$hup_status" == 129 ]] || {
	cat "$hup_output" >&2
	fail "The HUP handler should preserve exit status 129, got $hup_status."
}
[[ ! -e "$hup_session_dir" ]] || fail 'Successful HUP teardown should remove the session tree.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after HUP cleanup.'
for _ in {1..20}; do
	is_running "$hup_telegram_pid" || break
	sleep 0.05
done
if is_running "$hup_telegram_pid"; then
	ps -o pid,ppid,pgid,state,comm -p "$hup_telegram_pid" >&2 || true
	cat "$hup_output" >&2
	fail "Telegram process $hup_telegram_pid remains after HUP cleanup."
fi
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after HUP cleanup.'

orphan_output="${temp_dir}/orphan-openbox.log"
if run_helper orphan-openbox "$orphan_output"; then
	cat "$orphan_output" >&2
	fail 'A process group without a discovered leader should make cleanup fail.'
fi
orphan_session_dir="$(/usr/bin/grep -oE '/tmp/teagram-qa-session\.[[:alnum:]]{8}' "$orphan_output" | head -n 1 || true)"
[[ -n "$orphan_session_dir" ]] || {
	cat "$orphan_output" >&2
	fail 'The failed untracked group should identify its retained session directory.'
}
session_dirs+=("$orphan_session_dir")
grep -Fq 'Could not discover the openbox process group leader' "$orphan_output" || fail 'The orphan test should skip leader discovery deterministically.'
grep -Fq 'Found an untracked openbox process' "$orphan_output" || fail 'Cleanup should report an undiscovered marked process.'
[[ -d "$orphan_session_dir" ]] || fail 'Cleanup should retain session data when a marked process is untracked.'
! grep -Fq 'CLEANUP_COMPLETE=' "$orphan_output" || fail 'Cleanup must not report success with an untracked process.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after untracked-group failure.'

printf 'QA desktop session lifecycle tests passed.\n'
