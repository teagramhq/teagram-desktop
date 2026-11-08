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

fail() {
	printf '%s\n' "$*" >&2
	exit 1
}

cleanup() {
	local proc pgrp marker
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

cat > "$stub_bin/Xvfb" <<'STUB_XVFB'
#!/usr/bin/env bash
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

for tool in id Xvfb openbox tint2 dunst Telegram dbus-launch xdotool import tr; do
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
	local proc
	for proc in /proc/[0-9]*; do
		[[ -r "$proc/environ" ]] || continue
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
[[ -n "$failure_dir" ]] && session_dirs+=("$failure_dir")
dbus_log="$(sed -n 's/^dbus-launch failed; log retained at //p' "$failure_output" | head -n 1)"
[[ -n "$failure_dir" && -n "$dbus_log" && -f "$dbus_log" ]] || {
	cat "$failure_output" >&2
	fail 'A failed dbus-launch should name a retained diagnostic log.'
}
grep -Fq 'simulated dbus-launch failure' "$dbus_log" || fail 'The retained D-Bus log should include the launch error.'
[[ -d "$failure_dir" ]] || fail 'The session directory should remain after failed startup.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after failed startup.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after failed startup cleanup.'

telegram_failure_output="${temp_dir}/telegram-failure.log"
if run_helper telegram-fail "$telegram_failure_output"; then
	cat "$telegram_failure_output" >&2
	fail 'A failing Telegram stub should make startup fail.'
fi
telegram_log="$(sed -n 's/^Could not start telegram; see //p' "$telegram_failure_output" | head -n 1)"
[[ -n "$telegram_log" && -f "$telegram_log" ]] || {
	cat "$telegram_failure_output" >&2
	fail 'A failed Telegram launch should name a retained diagnostic log.'
}
telegram_dir="${telegram_log%/*}"
session_dirs+=("$telegram_dir")
grep -Fq 'simulated Telegram launch failure' "$telegram_log" || fail 'The retained Telegram log should include the launch error.'
[[ -d "$telegram_dir" ]] || fail 'The session directory should remain after Telegram startup failure.'
[[ ! -e "$remote_helper" ]] || fail 'The uploaded helper remains after failed Telegram startup.'
[[ -z "$(tagged_processes)" ]] || fail 'A stubbed QA process remains after Telegram startup failure cleanup.'

printf 'QA desktop session lifecycle tests passed.\n'
