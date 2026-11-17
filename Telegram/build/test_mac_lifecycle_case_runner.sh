#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
RUNNER="$ROOT/Telegram/build/run_mac_lifecycle_cases.sh"
TEMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEMP_ROOT"' EXIT

FAKE_CASES="$TEMP_ROOT/fake_cases.sh"
cat > "$FAKE_CASES" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "$2" in
pass)
	printf 'case body passed\n'
	;;
fail)
	printf 'case body failed\n'
	exit 7
	;;
hang)
	printf 'case entered hang\n'
	sleep 5
	;;
after-failure)
	printf 'later case ran\n'
	;;
*)
	exit 2
	;;
esac
EOF

if bash "$RUNNER" "$FAKE_CASES" fake-app "$TEMP_ROOT/logs" \
	pass fail after-failure > "$TEMP_ROOT/output.txt" 2>&1; then
	echo "case runner should fail when any case fails." >&2
	exit 1
fi

for evidence in \
	'lifecycle_case=pass result=PASS' \
	'case body failed' \
	'lifecycle_case=fail result=FAIL exit_status=7' \
	'later case ran' \
	'lifecycle_case=after-failure result=PASS' \
	'Lifecycle cases failed: fail'; do
	if ! grep -F -q "$evidence" "$TEMP_ROOT/output.txt"; then
		cat "$TEMP_ROOT/output.txt" >&2
		printf 'case runner output missed: %s\n' "$evidence" >&2
		exit 1
	fi
done

if TDESKTOP_MAC_LIFECYCLE_CASE_TIMEOUT_SECONDS=1 \
	bash "$RUNNER" "$FAKE_CASES" fake-app "$TEMP_ROOT/timeout-logs" \
		hang after-failure > "$TEMP_ROOT/timeout-output.txt" 2>&1; then
	echo "case runner should fail when a case exceeds its timeout." >&2
	exit 1
fi

for evidence in \
	'lifecycle_case=hang result=START' \
	'lifecycle_case_timeout=hang timeout_seconds=1' \
	'lifecycle_case=hang result=FAIL exit_status=124' \
	'lifecycle_case=after-failure result=SKIPPED dependency=hang case_timeout=1' \
	'Lifecycle cases failed: hang'; do
	if ! grep -F -q "$evidence" "$TEMP_ROOT/timeout-output.txt"; then
		cat "$TEMP_ROOT/timeout-output.txt" >&2
		printf 'timed-out case output missed: %s\n' "$evidence" >&2
		exit 1
	fi
done
if grep -F -q 'later case ran' "$TEMP_ROOT/timeout-output.txt"; then
	cat "$TEMP_ROOT/timeout-output.txt" >&2
	echo "case runner should stop after a timed-out lifecycle case." >&2
	exit 1
fi
for evidence in \
	'case entered hang' \
	'lifecycle_case_timeout=hang timeout_seconds=1 process_group_stopped=1'; do
	if ! grep -F -q "$evidence" "$TEMP_ROOT/timeout-logs/hang.log"; then
		cat "$TEMP_ROOT/timeout-logs/hang.log" >&2
		printf 'timed-out case diagnostics missed: %s\n' "$evidence" >&2
		exit 1
	fi
done

CLOCK_BIN="$TEMP_ROOT/fixed-clock-bin"
mkdir -p "$CLOCK_BIN"
cat > "$CLOCK_BIN/date" <<'EOF'
#!/usr/bin/env bash
printf '1000\n'
EOF
chmod +x "$CLOCK_BIN/date"

if TDESKTOP_MAC_LIFECYCLE_STEP_DEADLINE_EPOCH=1100 \
	PATH="$CLOCK_BIN:$PATH" \
	bash "$RUNNER" "$FAKE_CASES" fake-app "$TEMP_ROOT/deadline-limited-logs" \
		pass > "$TEMP_ROOT/deadline-limited-output.txt" 2>&1; then
	:
else
	cat "$TEMP_ROOT/deadline-limited-output.txt" >&2
	echo "case runner should run a case with the time left before the step deadline." >&2
	exit 1
fi
grep -Fq 'lifecycle_case=pass result=START timeout_seconds=90' \
	"$TEMP_ROOT/deadline-limited-output.txt" || {
	cat "$TEMP_ROOT/deadline-limited-output.txt" >&2
	echo "case runner should cap the case timeout at deadline minus cleanup headroom." >&2
	exit 1
}

if TDESKTOP_MAC_LIFECYCLE_STEP_DEADLINE_EPOCH=1700 \
	PATH="$CLOCK_BIN:$PATH" \
	bash "$RUNNER" "$FAKE_CASES" fake-app "$TEMP_ROOT/deadline-capped-logs" \
		pass > "$TEMP_ROOT/deadline-capped-output.txt" 2>&1; then
	:
else
	cat "$TEMP_ROOT/deadline-capped-output.txt" >&2
	echo "case runner should complete a case below its normal timeout cap." >&2
	exit 1
fi
grep -Fq 'lifecycle_case=pass result=START timeout_seconds=600' \
	"$TEMP_ROOT/deadline-capped-output.txt" || {
	cat "$TEMP_ROOT/deadline-capped-output.txt" >&2
	echo "case runner should cap the timeout at 600 seconds when ample time remains." >&2
	exit 1
}

if TDESKTOP_MAC_LIFECYCLE_STEP_DEADLINE_EPOCH=1009 \
	PATH="$CLOCK_BIN:$PATH" \
	bash "$RUNNER" "$FAKE_CASES" fake-app "$TEMP_ROOT/deadline-exhausted-logs" \
		pass after-failure > "$TEMP_ROOT/deadline-exhausted-output.txt" 2>&1; then
	echo "case runner should fail when the step deadline has no case time left." >&2
	exit 1
fi
for evidence in \
	'lifecycle_case=pass result=FAIL reason=step-deadline-exhausted' \
	'lifecycle_case=after-failure result=SKIPPED dependency=pass step_deadline_exhausted=1'; do
	if ! grep -F -q "$evidence" "$TEMP_ROOT/deadline-exhausted-output.txt"; then
		cat "$TEMP_ROOT/deadline-exhausted-output.txt" >&2
		printf 'deadline exhausted output missed: %s\n' "$evidence" >&2
		exit 1
	fi
done
if grep -F -q 'case body passed' "$TEMP_ROOT/deadline-exhausted-output.txt"; then
	cat "$TEMP_ROOT/deadline-exhausted-output.txt" >&2
	echo "case runner should not start a lifecycle case after the step deadline." >&2
	exit 1
fi
grep -Fq 'lifecycle_case_deadline_exhausted=pass deadline_epoch=1009 remaining_seconds=-1 cleanup_headroom_seconds=10' \
	"$TEMP_ROOT/deadline-exhausted-logs/pass.log" || {
	cat "$TEMP_ROOT/deadline-exhausted-logs/pass.log" >&2
	echo "case runner should retain deadline exhaustion diagnostics for the skipped case." >&2
	exit 1
}

if bash "$RUNNER" "$FAKE_CASES" fake-app "$TEMP_ROOT/invalid-logs" \
	../outside > "$TEMP_ROOT/invalid-output.txt" 2>&1; then
	echo "case runner should reject case names that can escape its log directory." >&2
	exit 1
fi
grep -Fq 'case name must contain lowercase letters, digits, and single hyphens' \
	"$TEMP_ROOT/invalid-output.txt" || {
	cat "$TEMP_ROOT/invalid-output.txt" >&2
	echo "case runner did not explain its case name validation failure." >&2
	exit 1
}

FAKE_BIN="$TEMP_ROOT/fake-bin"
FAKE_APP="$TEMP_ROOT/fake-app/Contents/MacOS/Teagram"
PROFILE_GUARD="$ROOT/Telegram/build/test_mac_profile_guard.sh"
mkdir -p "$FAKE_BIN" "$(dirname "$FAKE_APP")"
cat > "$FAKE_BIN/uname" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == -s ]]; then
	printf 'Darwin\n'
else
	exec /usr/bin/uname "$@"
fi
EOF
cat > "$FAKE_BIN/md5" <<'EOF'
#!/usr/bin/env bash
python3 -c 'import hashlib, sys; print(hashlib.md5(sys.stdin.buffer.read()).hexdigest())'
EOF
cat > "$FAKE_BIN/sw_vers" <<'EOF'
#!/usr/bin/env bash
printf '15.0\n'
EOF
cat > "$FAKE_APP" <<'EOF'
#!/usr/bin/env bash
if [[ "${TDESKTOP_FONT_STARTUP_QUIT_REGRESSION:-}" == 1 ]]; then
	if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == signal-install ]]; then
		printf 'Font shutdown regression: probe=unavailable\n'
		exit 1
	fi
	if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-timeout ]]; then
		printf 'Font shutdown regression: last_other_work_frames=worker <- pthread_start\n'
		printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=0\n'
		exit 0
	fi
	if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-timeout-retry ]]; then
		printf 'Font shutdown regression: retry_checkpoint=1\n'
		printf 'Font shutdown regression: retry_timeout=1\n'
		printf 'Font shutdown regression: retry_gui_event=1\n'
		printf 'Font shutdown regression: retry_worker_done=1\n'
		printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\n'
		exit 0
	fi
	if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == shutdown-remove-pending ]]; then
		printf 'Font shutdown regression: quit_observed=1 font_work=active remove_pending_at_quit=1 removal_completed=1\n'
		printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\n'
		exit 0
	fi
	if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-missed-first ]]; then
		printf 'Font shutdown regression: enumeration_retry=1\n'
	fi
	printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=1\n'
	printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\n'
	exit 0
fi
if [[ -n "${TDESKTOP_PROFILE_LOCK_REFUSAL_REGRESSION:-}" ]]; then
    refusal_window=1
    refusal_path="$HOME/Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/refused-profile.lock"
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == lock-refusal-no-window ]]; then refusal_window=0; fi
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == lock-refusal-no-path ]]; then refusal_path='missing'; fi
    policy_operation=stat
    reported_operation=stat
    window_contains_error=1
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == lock-refusal-wrong-policy ]]; then policy_operation=write; fi
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == lock-refusal-wrong-reported ]]; then reported_operation=write; fi
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == lock-refusal-wrong-window ]]; then window_contains_error=0; fi
    printf 'Mac protected path refusal: operation=%s class=group-container callsite=int Core::Sandbox::start()\n' "$policy_operation"
    printf 'Profile lock refusal regression: failure_window=%s window_contains_error=%s operation=%s path=%s\n' "$refusal_window" "$window_contains_error" "$reported_operation" "$refusal_path"
    exit 1
fi
if [[ -n "${TDESKTOP_PROFILE_LOCK_REGRESSION:-}" ]]; then
    lock_window=1
    lock_reason='open failed'
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == lock-error-no-window ]]; then lock_window=0; fi
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == lock-error-no-reason ]]; then lock_reason=''; fi
    printf 'Profile lock regression: failure_window=%s window_contains_error=1 reason=%s\n' "$lock_window" "$lock_reason"
    exit 0
fi
if [[ -n "${TDESKTOP_SOCKET_ERROR_REGRESSION:-}" ]]; then
    printf 'Socket error regression: injected=1\n'
    if [[ "$TDESKTOP_SOCKET_ERROR_REGRESSION" == persistent ]]; then
        if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" != socket-error-no-window ]]; then
            printf 'Socket error regression: failure_window=1\n'
        fi
        exit 0
    fi
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == socket-error-no-retry ]]; then exit 1; fi
fi
if [[ -n "${TDESKTOP_INSTANCE_RECHECK_HOLD_FILE:-}" ]]; then
    printf 'Single instance recheck regression: initial_failure=1 socket=%s\n' "$TDESKTOP_INSTANCE_RECHECK_TEST_SOCKET"
    while [[ ! -e "$TDESKTOP_INSTANCE_RECHECK_HOLD_FILE" ]]; do sleep 0.02; done
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == emoji-relaunch-skip-recheck ]]; then exit 1; fi
    python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"CMD:show;"); s.recv(128)' "$TDESKTOP_INSTANCE_RECHECK_TEST_SOCKET"
    printf 'Emoji cache relaunch: forwarded=1\n'
    exit 0
fi
if [[ -n "${TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE:-}" ]]; then
    printf 'Emoji cache shutdown regression: drain_active=1\n'
    if [[ "${TDESKTOP_EMOJI_RELAUNCH_COMPETING:-}" == 1 ]]; then
        for index in 1 2; do
            while [[ ! -e "$TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE.peer.$index" ]]; do sleep 0.02; done
            printf 'Emoji cache shutdown regression: peer_waiting=1\n'
        done
    else
        while [[ ! -e "$TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE.peer" ]]; do sleep 0.02; done
        printf 'Emoji cache shutdown regression: peer_waiting=1\n'
    fi
    while [[ ! -e "$TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE" ]]; do sleep 0.02; done
    exit 0
fi
if [[ "${TDESKTOP_EMOJI_RELAUNCH_SECONDARY_TEST:-}" == 1 ]]; then
    if [[ -n "${TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE:-}" ]]; then
        touch "$TDESKTOP_EMOJI_RELAUNCH_TEST_FILE.peer.$TDESKTOP_EMOJI_RELAUNCH_INDEX"
    else
        touch "$TDESKTOP_EMOJI_RELAUNCH_TEST_FILE.peer"
    fi
    if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == emoji-relaunch-early ]]; then exit 0; fi
    while [[ ! -e "$TDESKTOP_EMOJI_RELAUNCH_TEST_FILE" ]]; do sleep 0.02; done
    if [[ -n "${TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE:-}" ]]; then
        if [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == emoji-relaunch-double-start ]] \
            || [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" == emoji-relaunch-second-wins && "$TDESKTOP_EMOJI_RELAUNCH_INDEX" == 2 ]] \
            || [[ "${TDESKTOP_FONT_TEST_MODE:-${FONT_TEST_MODE:-}}" != emoji-relaunch-second-wins && "$TDESKTOP_EMOJI_RELAUNCH_INDEX" == 1 ]]; then
            printf 'Emoji cache relaunch: application_started=1\n'
            while [[ ! -e "$TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE" ]]; do sleep 0.02; done
        else
            printf 'Emoji cache relaunch: forwarded=1\n'
        fi
        exit 0
    fi
    printf 'Font database warmup regression: ready=1 custom-fonts=1\n'
    exit 0
fi
if [[ "${TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION:-}" == 1 ]]; then
	printf 'Emoji cache shutdown regression: worker_blocked=1\n'
	printf 'Emoji cache shutdown regression: repeat_quit=1\n'
	printf 'Emoji cache shutdown regression: gui_responsive=1 integration_alive=1 worker_returned=1 wait_timed_out=0\n'
	exit 0
fi
if [[ "${TDESKTOP_FONT_STARTUP_REGRESSION:-}" == 1 ]]; then
	if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == thread-start ]]; then
		printf 'Font database warmup regression: started=0\n'
		printf 'Font database warmup regression: ready=0\n'
		printf 'StartFonts startup regression: wall_ns=150000000\n'
		exit 0
	fi
	count_file="$HOME/font-startup-count"
	if [[ -f "$count_file" ]]; then
		wall_ns=50000000
	else
		wall_ns=25000000
		printf 'started' > "$count_file"
	fi
	printf 'StartFonts startup regression: wall_ns=%s\n' "$wall_ns"
	if [[ "${TDESKTOP_FONT_STARTUP_REGRESSION_SKIP_EXPOSE_CALLBACK:-}" == 1 ]]; then
		printf 'Font startup exposure timeout regression: wall_ns=500000000\n'
	else
		printf 'Font startup splash regression: visible=1 wall_ns=30000000\n'
	fi
	printf 'Font database warmup regression: started=1\n'
	printf 'Font database metrics warmup regression: ready=1\n'
	printf 'Font database warmup regression: ready=1 custom-fonts=1\n'
	printf 'Font fallback regression: emoji=Apple Color Emoji arabic=Vazirmatn UI NL wall_ns=1000000 result=PASS\n'
	exit 0
fi
exit 0
EOF
chmod +x "$FAKE_BIN/uname" "$FAKE_BIN/md5" "$FAKE_BIN/sw_vers" "$FAKE_APP"
mkdir -p "$TEMP_ROOT/profile-harness"

if TDESKTOP_FONT_TEST_MODE=pass \
	TDESKTOP_MAC_PROFILE_TEST_TMP_BASE="$TEMP_ROOT/profile-harness" \
	PATH="$FAKE_BIN:$PATH" bash "$PROFILE_GUARD" "$FAKE_APP" \
	> "$TEMP_ROOT/profile-entrypoint-output.txt" 2>&1; then
	echo "profile guard should report failures from the fake app cases." >&2
	exit 1
fi

for case_name in \
	profile-ipc \
	start-in-tray \
	spoiler-cache-emoji \
	spoiler-cache-spoiler \
	spoiler-cache-text \
	spoiler-cache-image \
	authenticated-lifecycle \
	authenticated-cache \
	native-failure-continuation \
	folder-fault-barrier-timeout \
	folder-fault-cross-session-presence; do
	if ! grep -F -q "lifecycle_case=$case_name result=START" \
		"$TEMP_ROOT/profile-entrypoint-output.txt"; then
		cat "$TEMP_ROOT/profile-entrypoint-output.txt" >&2
		printf 'default profile guard invocation missed case: %s\n' "$case_name" >&2
		exit 1
	fi
done

font_start_line="$(grep -n -m 1 '^StartFonts cold=PASS' \
	"$TEMP_ROOT/profile-entrypoint-output.txt" || true)"
profile_ipc_line="$(grep -n -m 1 '^lifecycle_case=profile-ipc result=START' \
	"$TEMP_ROOT/profile-entrypoint-output.txt" || true)"
if [[ -z "$font_start_line" || -z "$profile_ipc_line" \
	|| "${font_start_line%%:*}" -ge "${profile_ipc_line%%:*}" ]]; then
	cat "$TEMP_ROOT/profile-entrypoint-output.txt" >&2
	echo "default profile guard must run cold font startup before profile-ipc." >&2
	exit 1
fi

printf 'mac lifecycle case runner self-test passed.\n'
