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

printf 'mac lifecycle case runner self-test passed.\n'
