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
hup_pid=

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

cleanup() {
	local proc pgrp marker
	if [[ -n "$hup_pid" ]] && kill -0 "$hup_pid" 2>/dev/null; then
		kill -TERM "$hup_pid" 2>/dev/null || true
	fi
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
	rm -f -- "$remote_helper"
	rm -rf -- "$temp_dir"
}
trap cleanup EXIT

mkdir -p "$stub_bin"
cp "$(command -v sleep)" "$stub_bin/dbus-daemon"

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
printf '99\n' >&3
exec sleep 120
STUB_XVFB

for service in openbox tint2 dunst; do
	cat > "$stub_bin/$service" <<'STUB_SERVICE'
#!/usr/bin/env bash
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
	printf 'simulated Telegram launch failure\n' >&2
	exit 42
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
		printf '12345\n'
		;;
	getwindowgeometry)
		if [[ "${2:-}" == --shell ]]; then
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
printf 'stub screenshot\n' > "${@: -1}"
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

for tool in id Xvfb openbox tint2 dunst Telegram dbus-launch xdotool import tr xauth; do
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
	esac
	if [[ "$mode" == stop ]]; then
		if printf 'stop\n' | env -i \
			QA_SESSION_GROUP=seed \
			QA_SESSION_PADDING="$padding" \
			QA_SESSION_TEST_RUN_ID="$run_id" \
			QA_SESSION_TELEGRAM_BIN="$stub_bin/Telegram" \
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
[[ ! -e "$telegram_dir" ]] || fail 'Per-run data should be removed after failed Telegram cleanup.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after failed Telegram startup.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after Telegram startup failure cleanup.'

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
	QA_TEST_DBUS_ADDRESS="unix:path=${temp_dir}/session-bus" \
	PATH="$stub_bin:$system_path" \
	HOME="$temp_dir" \
	bash "$remote_helper" serve < "$hup_fifo" > "$hup_output" 2>&1 &
hup_pid=$!
exec 9>"$hup_fifo"
for _ in {1..200}; do
	grep -Fq 'Ready for steps:' "$hup_output" && break
	if ! kill -0 "$hup_pid" 2>/dev/null; then
		cat "$hup_output" >&2
		fail 'The session should remain ready before the HUP test.'
	fi
	sleep 0.05
done
grep -Fq 'Ready for steps:' "$hup_output" || {
	cat "$hup_output" >&2
	fail 'The HUP test session did not become ready.'
}
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
if wait "$hup_pid"; then
	fail 'The HUP test helper should exit with a non-zero status.'
else
	hup_status=$?
fi
is_running() {
	local state
	[[ -r "/proc/$1/stat" ]] || return 1
	state="$(awk '{ print $3 }' "/proc/$1/stat" 2>/dev/null || true)"
	[[ -n "$state" && "$state" != Z && "$state" != X ]]
}
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

printf 'QA desktop session lifecycle tests passed.\n'
