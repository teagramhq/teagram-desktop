#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

usage() {
	echo 'Usage: qa-linux-desktop-session.sh serve' >&2
	exit 64
}

[[ "${1:-}" == serve && $# == 1 ]] || usage
[[ "$(id -un)" == qa ]] || {
	echo 'Run this session as the unprivileged qa user.' >&2
	exit 77
}
[[ "$0" == /tmp/qa-linux-desktop-session-*.sh ]] || {
	echo 'Upload this helper to a unique /tmp/qa-linux-desktop-session-*.sh path before running it.' >&2
	exit 64
}

for tool in Xvfb dbus-launch openbox tint2 dunst xdotool import setsid xauth; do
	command -v "$tool" >/dev/null || {
		echo "Required QA desktop tool is missing: $tool" >&2
		exit 69
	}
done

BIN="${QA_SESSION_TELEGRAM_BIN:-/opt/teagram/Telegram}"
if [[ -z "${QA_SESSION_TELEGRAM_BIN:-}" && -x /home/qa/build/Telegram ]]; then
	BIN=/home/qa/build/Telegram
fi
[[ -x "$BIN" ]] || {
	echo "No executable Teagram build found at /home/qa/build/Telegram or /opt/teagram/Telegram." >&2
	exit 69
}

BUILD_SHA=unknown
BUILD_FILE="$(dirname "$BIN")/BUILD"
[[ -r "$BUILD_FILE" ]] && BUILD_SHA="$(<"$BUILD_FILE")"

SESSION_DIR="$(mktemp -d /tmp/teagram-qa-session.XXXXXXXX)"
SESSION_ID="${SESSION_DIR##*/}"
REMOTE_HELPER="$0"
PROFILE="$SESSION_DIR/profile"
RUNTIME_DIR="$SESSION_DIR/runtime"
DISPLAY_FILE="$SESSION_DIR/display"
XAUTHORITY="$SESSION_DIR/Xauthority"
export XAUTHORITY
STEP_NUMBER=0
DBUS_PID=
DBUS_START_TICKS=
DBUS_SESSION_BUS_ADDRESS=
DISPLAY_NUMBER=
DISPLAY_VALUE=
WINDOW_ID=
WINDOW_X=
WINDOW_Y=
CLEANUP_STARTED=0
declare -A GROUP_MARKERS=()
declare -A GROUP_PIDS=()

cleanup_preflight() {
	rm -rf -- "$SESSION_DIR"
	if [[ -f "$REMOTE_HELPER" ]]; then
		rm -f -- "$REMOTE_HELPER"
	fi
}
trap cleanup_preflight EXIT

mkdir -p "$PROFILE" "$RUNTIME_DIR" \
	"$SESSION_DIR/xdg-config" "$SESSION_DIR/xdg-data" "$SESSION_DIR/xdg-cache"
chmod 700 "$SESSION_DIR" "$PROFILE" "$RUNTIME_DIR" \
	"$SESSION_DIR/xdg-config" "$SESSION_DIR/xdg-data" "$SESSION_DIR/xdg-cache"

proc_fields() {
	awk '{ print $5, $6, $22 }' "/proc/$1/stat" 2>/dev/null
}

has_group_marker() {
	local pid="$1" marker="$2"
	[[ -r "/proc/$pid/environ" ]] &&
		grep -qzFx -- "QA_SESSION_GROUP=$marker" "/proc/$pid/environ"
}

prepare_xauthority() {
	local cookie
	: > "$XAUTHORITY"
	chmod 600 "$XAUTHORITY"
	cookie="$(od -An -N16 -tx1 /dev/urandom | tr -d '[:space:]')"
	[[ "$cookie" =~ ^[[:xdigit:]]{32}$ ]] || {
		echo 'Could not generate an Xauthority cookie.' >&2
		return 1
	}
	printf 'ffff 0000 0000 0012 4d49542d4d414749432d434f4f4b49452d31 0010 %s\n' "$cookie" |
		xauth -f "$XAUTHORITY" nmerge -
	chmod 600 "$XAUTHORITY"
}

find_group_leader() {
	local marker="$1" proc pid pgrp session _start_ticks
	for proc in /proc/[0-9]*; do
		pid="${proc##*/}"
		[[ -r "$proc/stat" && -r "$proc/environ" ]] || continue
		read -r pgrp session _start_ticks < <(proc_fields "$pid") || continue
		[[ "$pid" == "$pgrp" && "$pid" == "$session" ]] || continue
		if has_group_marker "$pid" "$marker"; then
			printf '%s\n' "$pid"
			return 0
		fi
	done
	return 1
}

find_marked_process() {
	local marker="$1" proc pid
	for proc in /proc/[0-9]*; do
		pid="${proc##*/}"
		[[ -r "$proc/environ" ]] || continue
		if has_group_marker "$pid" "$marker"; then
			printf '%s\n' "$pid"
			return 0
		fi
	done
	return 1
}

group_is_alive() {
	local pgid="$1" marker="$2" proc pid pgrp session _start_ticks
	for proc in /proc/[0-9]*; do
		pid="${proc##*/}"
		[[ -r "$proc/stat" && -r "$proc/environ" ]] || continue
		read -r pgrp session _start_ticks < <(proc_fields "$pid") || continue
		[[ "$pgrp" == "$pgid" ]] || continue
		if has_group_marker "$pid" "$marker"; then
			return 0
		fi
	done
	return 1
}

start_group() {
	local name="$1" log="$2" marker leader_pid untracked_pid
	local leader_attempts="${QA_SESSION_LEADER_ATTEMPTS:-100}" attempt
	shift 2
	[[ "$leader_attempts" =~ ^[0-9]+$ ]] || {
		echo 'QA_SESSION_LEADER_ATTEMPTS must be a non-negative integer.' >&2
		return 64
	}
	marker="$SESSION_ID-$name-$$"
	GROUP_MARKERS["$name"]="$marker"
	setsid env \
		"QA_SESSION_GROUP=$marker" \
		"DISPLAY=$DISPLAY_VALUE" \
		"XAUTHORITY=$XAUTHORITY" \
		"DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS" \
		"XDG_RUNTIME_DIR=$RUNTIME_DIR" \
		"XDG_CONFIG_HOME=$SESSION_DIR/xdg-config" \
		"XDG_DATA_HOME=$SESSION_DIR/xdg-data" \
		"XDG_CACHE_HOME=$SESSION_DIR/xdg-cache" \
		"$@" > "$SESSION_DIR/$log" 2>&1 &
	for ((attempt = 0; attempt < leader_attempts; attempt++)); do
		if leader_pid="$(find_group_leader "$marker")"; then
			GROUP_PIDS["$name"]="$leader_pid"
			return 0
		fi
		sleep 0.05
	done
	for ((attempt = 0; attempt < 100; attempt++)); do
		if untracked_pid="$(find_marked_process "$marker")"; then
			echo "Could not discover the $name process group leader; marked process $untracked_pid remains." >&2
			return 1
		fi
		sleep 0.05
	done
	echo "Could not start $name; captured output will be emitted before cleanup." >&2
	return 1
}

stop_group() {
	local name="$1" pgid="${GROUP_PIDS[$1]:-}" marker="${GROUP_MARKERS[$1]:-}" untracked_pid
	[[ -n "$marker" ]] || return 0
	if [[ -z "$pgid" ]]; then
		if untracked_pid="$(find_marked_process "$marker")"; then
			echo "Found an untracked $name process $untracked_pid with its QA session marker." >&2
			return 1
		fi
		return 0
	fi
	group_is_alive "$pgid" "$marker" || return 0
	if ! kill -TERM -- "-$pgid" 2>/dev/null && group_is_alive "$pgid" "$marker"; then
		echo "Could not send TERM to the $name process group $pgid." >&2
		return 1
	fi
	for _ in {1..50}; do
		group_is_alive "$pgid" "$marker" || return 0
		sleep 0.1
	done
	if group_is_alive "$pgid" "$marker"; then
		if ! kill -KILL -- "-$pgid" 2>/dev/null && group_is_alive "$pgid" "$marker"; then
			echo "Could not stop the $name process group $pgid." >&2
			return 1
		fi
		for _ in {1..20}; do
			group_is_alive "$pgid" "$marker" || return 0
			sleep 0.1
		done
	fi
	if group_is_alive "$pgid" "$marker"; then
		echo "The $name process group $pgid is still running." >&2
		return 1
	fi
}

stop_dbus() {
	[[ -n "$DBUS_PID" && -n "$DBUS_START_TICKS" ]] || return 0
	[[ -r "/proc/$DBUS_PID/stat" ]] || return 0
	local current_pgrp current_session current_start current_name
	read -r current_pgrp current_session current_start < <(proc_fields "$DBUS_PID") || return 0
	[[ "$current_start" == "$DBUS_START_TICKS" ]] || return 0
	current_name="$(<"/proc/$DBUS_PID/comm")"
	[[ "$current_name" == dbus-daemon ]] || {
		echo "Refusing to stop unexpected process $DBUS_PID ($current_name)." >&2
		return 1
	}
	if ! kill -TERM "$DBUS_PID" 2>/dev/null && [[ -r "/proc/$DBUS_PID/stat" ]]; then
		read -r current_pgrp current_session current_start < <(proc_fields "$DBUS_PID") || return 0
		[[ "$current_start" == "$DBUS_START_TICKS" ]] || return 0
		echo "Could not stop the session D-Bus process $DBUS_PID." >&2
		return 1
	fi
	for _ in {1..50}; do
		[[ -r "/proc/$DBUS_PID/stat" ]] || return 0
		read -r current_pgrp current_session current_start < <(proc_fields "$DBUS_PID") || return 0
		[[ "$current_start" == "$DBUS_START_TICKS" ]] || return 0
		sleep 0.1
	done
	if [[ -r "/proc/$DBUS_PID/stat" ]]; then
		read -r current_pgrp current_session current_start < <(proc_fields "$DBUS_PID") || return 0
		if [[ "$current_start" == "$DBUS_START_TICKS" ]]; then
			if ! kill -KILL "$DBUS_PID" 2>/dev/null; then
				echo "Could not force-stop the session D-Bus process $DBUS_PID." >&2
				return 1
			fi
		fi
	fi
}

cleanup() {
	(( CLEANUP_STARTED == 0 )) || return 0
	CLEANUP_STARTED=1
	local failed=0 name
	for name in telegram dunst tint2 openbox xvfb; do
		stop_group "$name" || failed=1
	done
	stop_dbus || failed=1
	if (( failed == 0 )); then
		if ! rm -rf -- "$SESSION_DIR"; then
			echo "Could not remove QA session directory $SESSION_DIR." >&2
			failed=1
		elif [[ -e "$SESSION_DIR" ]]; then
			echo "Could not remove QA session directory $SESSION_DIR." >&2
			failed=1
		fi
	else
		echo "QA session directory retained for cleanup: $SESSION_DIR" >&2
	fi
	if [[ -n "$REMOTE_HELPER" && -f "$REMOTE_HELPER" ]]; then
		if ! rm -f -- "$REMOTE_HELPER"; then
			echo "Could not remove the uploaded helper $REMOTE_HELPER." >&2
			failed=1
		fi
	fi
	if (( failed != 0 )); then
		return 1
	fi
	return 0
}

emit_session_diagnostics() {
	local log
	printf 'QA session diagnostics from %s follow (last 200 lines per log):\n' "$SESSION_DIR" >&2
	for log in "$SESSION_DIR"/*.log "$PROFILE"/log*.txt "$PROFILE"/DebugLogs/*.txt; do
		[[ -f "$log" ]] || continue
		printf '\n--- %s ---\n' "${log##*/}" >&2
		tail -n 200 -- "$log" >&2 || true
	done
}

on_exit() {
	local status=$?
	trap - EXIT
	trap '' HUP INT TERM
	if (( status != 0 )); then
		printf 'QA session exited with status %s.\n' "$status" >&2
		emit_session_diagnostics
	fi
	if cleanup; then
		(( status != 0 )) || printf 'CLEANUP_COMPLETE=%s\n' "$SESSION_ID"
	else
		(( status != 0 )) || emit_session_diagnostics
		status=1
	fi
	exit "$status"
}
trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

prepare_xauthority

setsid env "QA_SESSION_GROUP=$SESSION_ID-xvfb-$$" \
	Xvfb -displayfd 3 -screen 0 1440x900x24 -nolisten tcp -auth "$XAUTHORITY" \
	3>"$DISPLAY_FILE" > "$SESSION_DIR/xvfb.log" 2>&1 &
GROUP_MARKERS[xvfb]="$SESSION_ID-xvfb-$$"
for _ in {1..100}; do
	if XVFB_PID="$(find_group_leader "${GROUP_MARKERS[xvfb]}")"; then
		GROUP_PIDS[xvfb]="$XVFB_PID"
		break
	fi
	sleep 0.05
done
[[ -n "${GROUP_PIDS[xvfb]:-}" ]] || {
	echo 'Could not start Xvfb; captured output will be emitted before cleanup.' >&2
	exit 1
}
for _ in {1..100}; do
	[[ -s "$DISPLAY_FILE" ]] && break
	group_is_alive "${GROUP_PIDS[xvfb]}" "${GROUP_MARKERS[xvfb]}" || {
		echo 'Xvfb exited before it selected a display; captured output will be emitted before cleanup.' >&2
		exit 1
	}
	sleep 0.05
done
[[ -s "$DISPLAY_FILE" ]] || {
	echo 'Xvfb did not publish a display number; captured output will be emitted before cleanup.' >&2
	exit 1
}
DISPLAY_NUMBER="$(<"$DISPLAY_FILE")"
[[ "$DISPLAY_NUMBER" =~ ^[0-9]+$ ]] || {
	echo "Xvfb published an invalid display number." >&2
	exit 1
}
DISPLAY_VALUE=":$DISPLAY_NUMBER"

if ! XDG_RUNTIME_DIR="$RUNTIME_DIR" DISPLAY="$DISPLAY_VALUE" \
	dbus-launch --sh-syntax > "$SESSION_DIR/dbus.env" 2> "$SESSION_DIR/dbus-launch.log"; then
	echo 'dbus-launch failed; captured output will be emitted before cleanup.' >&2
	exit 1
fi
# dbus-launch emits shell assignments generated by this local executable.
. "$SESSION_DIR/dbus.env"
[[ "${DBUS_SESSION_BUS_PID:-}" =~ ^[0-9]+$ ]] || {
	echo 'dbus-launch did not provide a session bus PID.' >&2
	exit 1
}
DBUS_PID="$DBUS_SESSION_BUS_PID"
for _ in {1..50}; do
	if [[ -r "/proc/$DBUS_PID/stat" ]]; then
		DBUS_START_TICKS="$(awk '{ print $22 }' "/proc/$DBUS_PID/stat")"
		break
	fi
	sleep 0.05
done
[[ -n "$DBUS_START_TICKS" ]] || {
	echo "Session D-Bus process $DBUS_PID did not start." >&2
	exit 1
}

start_group openbox openbox.log openbox
start_group tint2 tint2.log tint2
start_group dunst dunst.log dunst
start_group telegram teagram.log "$BIN" -workdir "$PROFILE"
group_is_alive "${GROUP_PIDS[telegram]}" "${GROUP_MARKERS[telegram]}" || {
	echo 'Telegram exited during startup; captured output will be emitted before cleanup.' >&2
	exit 1
}

find_main_window() {
	local candidate geometry width height
	local -a candidates=()
	mapfile -t candidates < <(
		DISPLAY="$DISPLAY_VALUE" xdotool search --onlyvisible --pid "${GROUP_PIDS[telegram]}" 2>>"$SESSION_DIR/xdotool.log" || true
	)
	for candidate in "${candidates[@]}"; do
		geometry="$(DISPLAY="$DISPLAY_VALUE" xdotool getwindowgeometry --shell "$candidate" 2>>"$SESSION_DIR/xdotool.log")" || continue
		width="$(awk -F= '$1 == "WIDTH" { print $2 }' <<<"$geometry")"
		height="$(awk -F= '$1 == "HEIGHT" { print $2 }' <<<"$geometry")"
		if [[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ ]] && (( width >= 398 && height >= 522 )); then
			printf '%s\n' "$candidate"
			return 0
		fi
	done
	return 1
}

# Concurrent client starts can take longer than the legacy eight-second delay.
for _ in {1..60}; do
	if WINDOW_ID="$(find_main_window)"; then
		break
	fi
	group_is_alive "${GROUP_PIDS[telegram]}" "${GROUP_MARKERS[telegram]}" || {
		echo 'Telegram exited before showing its window; captured output will be emitted before cleanup.' >&2
		exit 1
	}
	sleep 1
done
[[ -n "$WINDOW_ID" ]] || {
	echo 'Telegram did not show its window within 60 seconds; captured output will be emitted before cleanup.' >&2
	exit 1
}
sleep 2

capture_screenshot() {
	local destination="$1" geometry
	if ! DISPLAY="$DISPLAY_VALUE" xdotool getwindowgeometry "$WINDOW_ID" >/dev/null 2>>"$SESSION_DIR/xdotool.log"; then
		WINDOW_ID="$(find_main_window)" || return 1
	fi
	geometry="$(DISPLAY="$DISPLAY_VALUE" xdotool getwindowgeometry --shell "$WINDOW_ID" 2>>"$SESSION_DIR/xdotool.log")" || return 1
	WINDOW_X="$(awk -F= '$1 == "X" { print $2 }' <<<"$geometry")"
	WINDOW_Y="$(awk -F= '$1 == "Y" { print $2 }' <<<"$geometry")"
	[[ "$WINDOW_X" =~ ^-?[0-9]+$ && "$WINDOW_Y" =~ ^-?[0-9]+$ ]] || return 1
	DISPLAY="$DISPLAY_VALUE" import -window "$WINDOW_ID" "$destination" || return 1
	chmod 600 "$destination"
}

INITIAL_SCREENSHOT="$SESSION_DIR/screenshot-000.png"
capture_screenshot "$INITIAL_SCREENSHOT" || {
	echo 'Could not capture the initial Teagram window.' >&2
	exit 1
}
printf 'SESSION=%s DISPLAY=%s PROFILE=%s TELEGRAM_PID=%s WINDOW_ID=%s WINDOW_X=%s WINDOW_Y=%s BUILD_SHA=%s SCREENSHOT=%s\n' \
	"$SESSION_ID" "$DISPLAY_VALUE" "$PROFILE" "${GROUP_PIDS[telegram]}" "$WINDOW_ID" "$WINDOW_X" "$WINDOW_Y" "$BUILD_SHA" "$INITIAL_SCREENSHOT"
printf 'Ready for steps: send `step <settle-seconds> <base64-command>`; send `stop` when finished.\n'

while read -r line; do
	case "$line" in
	stop)
		printf 'Stopping session %s.\n' "$SESSION_ID"
		exit 0
		;;
	status)
		printf 'SESSION=%s DISPLAY=%s PROFILE=%s TELEGRAM_PID=%s WINDOW_ID=%s WINDOW_X=%s WINDOW_Y=%s BUILD_SHA=%s\n' \
			"$SESSION_ID" "$DISPLAY_VALUE" "$PROFILE" "${GROUP_PIDS[telegram]}" "$WINDOW_ID" "$WINDOW_X" "$WINDOW_Y" "$BUILD_SHA"
		;;
	step\ *)
		step_args="${line#step }"
		settle="${step_args%% *}"
		encoded_command="${step_args#* }"
		[[ "$settle" =~ ^[0-9]+$ && "$settle" -le 60 && "$encoded_command" != "$step_args" ]] || {
			echo 'Invalid step. Use: step <settle-seconds: 0-60> <base64-command>.' >&2
			continue
		}
		if ! printf '%s' "$encoded_command" | base64 --decode > "$SESSION_DIR/step.sh"; then
			echo 'Step command was not valid base64.' >&2
			continue
		fi
		chmod 600 "$SESSION_DIR/step.sh"
		if env \
			"QA_SESSION_ID=$SESSION_ID" \
			"DISPLAY=$DISPLAY_VALUE" \
			"XAUTHORITY=$XAUTHORITY" \
			"QA_WINDOW_ID=$WINDOW_ID" \
			"QA_WINDOW_X=$WINDOW_X" \
			"QA_WINDOW_Y=$WINDOW_Y" \
			"DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS" \
			"XDG_RUNTIME_DIR=$RUNTIME_DIR" \
			"XDG_CONFIG_HOME=$SESSION_DIR/xdg-config" \
			"XDG_DATA_HOME=$SESSION_DIR/xdg-data" \
			"XDG_CACHE_HOME=$SESSION_DIR/xdg-cache" \
			bash "$SESSION_DIR/step.sh"; then
			STEP_STATUS=0
		else
			STEP_STATUS=$?
		fi
		sleep "$settle"
		STEP_NUMBER=$((STEP_NUMBER + 1))
		STEP_SCREENSHOT="$SESSION_DIR/screenshot-$(printf '%03d' "$STEP_NUMBER").png"
		if capture_screenshot "$STEP_SCREENSHOT"; then
			printf 'STEP=%s EXIT=%s SCREENSHOT=%s\n' "$STEP_NUMBER" "$STEP_STATUS" "$STEP_SCREENSHOT"
		else
			echo "Could not capture screenshot for step $STEP_NUMBER." >&2
		fi
		;;
	'')
		;;
	*)
		echo 'Unknown command. Use `step <settle-seconds> <base64-command>`, `status`, or `stop`.' >&2
		;;
	esac
done

printf 'Connection closed; cleaning session %s.\n' "$SESSION_ID" >&2
