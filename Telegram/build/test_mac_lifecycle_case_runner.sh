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
	'later case ran' \
	'lifecycle_case=after-failure result=PASS' \
	'Lifecycle cases failed: hang'; do
	if ! grep -F -q "$evidence" "$TEMP_ROOT/timeout-output.txt"; then
		cat "$TEMP_ROOT/timeout-output.txt" >&2
		printf 'timed-out case output missed: %s\n' "$evidence" >&2
		exit 1
	fi
done

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
if [[ "${TDESKTOP_FONT_STARTUP_REGRESSION:-}" == 1 ]]; then
	count_file="$HOME/font-startup-count"
	if [[ -f "$count_file" ]]; then
		wall_ns=50000000
	else
		wall_ns=25000000
		printf 'started' > "$count_file"
	fi
	printf 'StartFonts startup regression: wall_ns=%s\n' "$wall_ns"
	printf 'Font database warmup regression: ready=1\n'
	printf 'StartManager startup regression: wall_ns=20000000\n'
	printf 'Font fallback regression: emoji=Apple Color Emoji arabic=Vazirmatn UI NL wall_ns=1000000 result=PASS\n'
	exit 0
fi
exit 0
EOF
chmod +x "$FAKE_BIN/uname" "$FAKE_BIN/md5" "$FAKE_BIN/sw_vers" "$FAKE_APP"
mkdir -p "$TEMP_ROOT/profile-harness"

if TDESKTOP_MAC_PROFILE_TEST_TMP_BASE="$TEMP_ROOT/profile-harness" \
	PATH="$FAKE_BIN:$PATH" bash "$PROFILE_GUARD" "$FAKE_APP" \
	> "$TEMP_ROOT/profile-entrypoint-output.txt" 2>&1; then
	echo "profile guard should report failures from the fake app cases." >&2
	exit 1
fi

for case_name in \
	profile-ipc \
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
