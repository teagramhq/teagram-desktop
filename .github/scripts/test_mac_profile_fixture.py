#!/usr/bin/env python3

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


for preexisting_lock, create_lock in ((False, False), (False, True), (True, False)):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        executable = root / 'Teagram.app/Contents/MacOS/Teagram'
        executable.parent.mkdir(parents=True)
        bundle = executable.parents[2]
        lock_name = 'Teagram-lock-' + hashlib.md5(str(bundle).encode()).hexdigest()[:16]
        smoke_lock = Path('/tmp', lock_name + '-fixture')
        smoke_lock.write_text('synthetic smoke leftover\n')
        executable.write_text('''#!/bin/bash
if [[ "$HOME" == *'Group Containers'* ]]; then
    if [[ "$CREATE_REFUSAL_LOCK" == 1 ]]; then
        printf 'synthetic refused-process lock' > "$TDESKTOP_MAC_PROFILE_TEST_IPC/$TEST_LOCK_NAME"
    fi
    exit 1
fi
if [[ "${TDESKTOP_FONT_STARTUP_REGRESSION:-}" == 1 ]]; then
    count_file="$HOME/font-startup-count"
    if [[ -f "$count_file" ]]; then
        wall_ns=50000000
    else
        wall_ns=25000000
        printf 'started' > "$count_file"
    fi
    printf 'StartFonts startup regression: wall_ns=%s\\n' "$wall_ns"
    printf 'Sandbox startup to launchApplication regression: wall_ns=20000000\\n'
    printf 'Font startup window regression: visible=1 wall_ns=30000000\\n'
    printf 'Font database warmup regression: started=1\\n'
    printf 'Font database warmup regression: ready=1\\n'
    printf 'StartManager startup regression: wall_ns=20000000\\n'
    printf 'Font fallback regression: emoji=Apple Color Emoji arabic=Vazirmatn UI NL wall_ns=1000000 result=PASS\\n'
    exit 0
fi
if [[ "${TDESKTOP_MAC_SEATBELT_FORCE_COMPILE_FAILURE:-}" == 1 ]]; then
    exit 1
fi
echo 'stop after fixture checks' >&2
exit 1
''')
        executable.chmod(0o755)
        binaries = root / 'bin'
        binaries.mkdir()
        for name, body in {
            'uname': '#!/bin/bash\necho Darwin\n',
            'sw_vers': '#!/bin/bash\necho fixture\n',
            'mktemp': '''#!/bin/bash
directory="$(/usr/bin/mktemp "$@")" || exit "$?"
if [[ "$2" == /tmp/tg-ipc.* && "$PREEXISTING_LOCK" == 1 ]]; then
    printf 'preexisting fixture lock' > "$directory/$TEST_LOCK_NAME"
fi
printf '%s\n' "$directory"
''',
            'md5': '#!/usr/bin/env python3\nimport hashlib,sys\nprint(hashlib.md5(sys.stdin.buffer.read()).hexdigest())\n',
        }.items():
            stub = binaries / name
            stub.write_text(body)
            stub.chmod(0o755)
        environment = dict(os.environ, TDESKTOP_MAC_PROFILE_TEST_TMP_BASE=str(root),
                           CREATE_REFUSAL_LOCK=str(int(create_lock)), TEST_LOCK_NAME=lock_name,
                           PREEXISTING_LOCK=str(int(preexisting_lock)),
                           PATH=str(binaries) + ':' + os.environ['PATH'])
        try:
            result = subprocess.run(['bash', 'Telegram/build/test_mac_profile_guard.sh', str(executable)],
                                    env=environment, capture_output=True, text=True, timeout=15)
            output = result.stdout + result.stderr
            assert f'existed_before={int(preexisting_lock)}' in output, output
            assert f'exists_after={int(create_lock or preexisting_lock)}' in output, output
            if create_lock or preexisting_lock:
                assert 'profile refusal created or found an IPC lock.' in output, output
                assert 'protected_home_refusal=PASS' not in output, output
            else:
                assert 'protected_home_refusal=PASS' in output, output
            assert smoke_lock.read_text() == 'synthetic smoke leftover\n'
        finally:
            smoke_lock.unlink()

print('macOS profile fixture isolation and refusal lock check: PASS')


def run_font_startup_fixture(
        cold_ns,
        warm_ns,
        expected_status,
        mode='pass',
        fallback_wall_ns=1_000_000,
        launch_cold_ns=20_000_000,
        launch_warm_ns=20_000_000,
        window_cold_ns=30_000_000,
        window_warm_ns=30_000_000,
        manager_cold_ns=20_000_000,
        manager_warm_ns=20_000_000):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        executable = root / 'Teagram.app/Contents/MacOS/Teagram'
        executable.parent.mkdir(parents=True)
        executable.write_text('''#!/bin/bash
count_file="$HOME/font-startup-count"
mode="$FONT_TEST_MODE"
if [[ -f "$count_file" ]]; then
    wall_ns="$FONT_WARM_NS"
    launch_wall_ns="$FONT_LAUNCH_WARM_NS"
    window_wall_ns="$FONT_WINDOW_WARM_NS"
    manager_wall_ns="$FONT_MANAGER_WARM_NS"
    sample='warm'
else
    wall_ns="$FONT_COLD_NS"
    launch_wall_ns="$FONT_LAUNCH_COLD_NS"
    window_wall_ns="$FONT_WINDOW_COLD_NS"
    manager_wall_ns="$FONT_MANAGER_COLD_NS"
    sample='cold'
    printf 'started' > "$count_file"
fi
if [[ "$mode" == stall ]]; then
    printf 'UI stall synthetic main-thread delay\\n'
fi
if [[ "$mode" != missing-launch-timing ]]; then
    printf 'Sandbox startup to launchApplication regression: wall_ns=%s\\n' \\
        "$launch_wall_ns"
fi
if [[ "$mode" == warmup-before-window ]]; then
    printf 'Font database warmup regression: started=1\\n'
fi
if [[ "$mode" != missing-window ]]; then
    if [[ "$mode" == window-not-shown ]]; then
        printf 'Font startup window regression: visible=0 wall_ns=%s\\n' \\
            "$window_wall_ns"
    else
        printf 'Font startup window regression: visible=1 wall_ns=%s\\n' \\
            "$window_wall_ns"
    fi
fi
if [[ "$mode" != warmup-before-window ]]; then
    printf 'Font database warmup regression: started=1\\n'
fi
if [[ "$mode" != missing-timing ]]; then
    printf 'StartFonts startup regression: wall_ns=%s\\n' "$wall_ns"
fi
if [[ "$mode" == warmup-not-ready ]]; then
    printf 'Font database warmup regression: ready=0\\n'
else
    printf 'Font database warmup regression: ready=1\\n'
fi
if [[ "$mode" != missing-manager-timing \
    && ( "$mode" != missing-manager-warm-timing || "$sample" == cold ) ]]; then
    printf 'StartManager startup regression: wall_ns=%s\\n' \\
        "$manager_wall_ns"
fi
arabic_family='Vazirmatn UI NL'
if [[ "$mode" == fallback-mismatch ]]; then
    arabic_family='Unexpected Family'
fi
printf 'Font fallback regression: emoji=Apple Color Emoji arabic=%s wall_ns=%s result=PASS\\n' \\
    "$arabic_family" "$FONT_FALLBACK_WALL_NS"
''')
        executable.chmod(0o755)
        binaries = root / 'bin'
        binaries.mkdir()
        for name, body in {
            'uname': '#!/bin/bash\necho Darwin\n',
            'sw_vers': '#!/bin/bash\necho fixture\n',
            'mktemp': '''#!/bin/bash
directory="$(/usr/bin/mktemp "$@")" || exit "$?"
printf '%s\\n' "$directory"
''',
            'md5': '#!/usr/bin/env python3\nimport hashlib,sys\nprint(hashlib.md5(sys.stdin.buffer.read()).hexdigest())\n',
        }.items():
            stub = binaries / name
            stub.write_text(body)
            stub.chmod(0o755)
        environment = dict(
            os.environ,
            TDESKTOP_MAC_PROFILE_TEST_TMP_BASE=str(root),
            FONT_COLD_NS=str(cold_ns),
            FONT_WARM_NS=str(warm_ns),
            FONT_LAUNCH_COLD_NS=str(launch_cold_ns),
            FONT_LAUNCH_WARM_NS=str(launch_warm_ns),
            FONT_WINDOW_COLD_NS=str(window_cold_ns),
            FONT_WINDOW_WARM_NS=str(window_warm_ns),
            FONT_MANAGER_COLD_NS=str(manager_cold_ns),
            FONT_MANAGER_WARM_NS=str(manager_warm_ns),
            FONT_TEST_MODE=mode,
            FONT_FALLBACK_WALL_NS=str(fallback_wall_ns),
            PATH=str(binaries) + ':' + os.environ['PATH'],
        )
        result = subprocess.run(
            ['bash', 'Telegram/build/test_mac_profile_guard.sh',
             str(executable), 'font-startup'],
            env=environment, capture_output=True, text=True, timeout=15)
        output = result.stdout + result.stderr
        assert result.returncode == expected_status, output
        return output


font_startup_output = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, fallback_wall_ns=150_000_000,
    manager_cold_ns=40_000_000, manager_warm_ns=49_999_999)
assert 'StartFonts cold=PASS wall_ns=50000000 under_100ms=1' in font_startup_output
assert 'StartFonts warm=PASS wall_ns=50000000 under_100ms=1' in font_startup_output
assert 'Sandbox to launchApplication cold=PASS wall_ns=20000000 under_100ms=1' \
    in font_startup_output
assert 'Sandbox to launchApplication warm=PASS wall_ns=20000000 under_100ms=1' \
    in font_startup_output
assert 'Font startup window cold=PASS wall_ns=30000000 under_100ms=1' \
    in font_startup_output
assert 'Font startup window warm=PASS wall_ns=30000000 under_100ms=1' \
    in font_startup_output
assert 'Font database warmup order=PASS sample=cold' in font_startup_output
assert 'Font database warmup order=PASS sample=warm' in font_startup_output
assert 'Font startup through first resolve cold=PASS wall_ns=90000000 under_100ms=1' \
    in font_startup_output
assert 'Font startup through first resolve warm=PASS wall_ns=99999999 under_100ms=1' \
    in font_startup_output
assert 'Font database warmup before app run=PASS' in font_startup_output
assert 'fallback=PASS start_fonts_plus_start_manager_under_100ms=1' \
    in font_startup_output
font_startup_failure = run_font_startup_fixture(50_000_000, 100_000_000, 1)
assert 'StartFonts warm exceeded the 100ms budget' in font_startup_failure
font_startup_cold_failure = run_font_startup_fixture(100_000_000, 50_000_000, 1)
assert 'StartFonts cold exceeded the 100ms budget' in font_startup_cold_failure
font_startup_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='stall')
assert 'StartFonts cold launch produced a main-thread stall' in font_startup_stall
font_startup_slow_launch = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, launch_cold_ns=100_000_000)
assert 'Sandbox to launchApplication cold exceeded the 100ms budget' \
    in font_startup_slow_launch
font_startup_slow_warm_launch = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, launch_warm_ns=100_000_000)
assert 'Sandbox to launchApplication warm exceeded the 100ms budget' \
    in font_startup_slow_warm_launch
font_startup_slow_window = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, window_cold_ns=100_000_000)
assert 'Font startup window cold exceeded the 100ms budget' \
    in font_startup_slow_window
font_startup_slow_warm_window = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, window_warm_ns=100_000_000)
assert 'Font startup window warm exceeded the 100ms budget' \
    in font_startup_slow_warm_window
font_startup_missing_launch_timing = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-launch-timing')
assert 'Sandbox to launchApplication cold exceeded the 100ms budget or had no timing' \
    in font_startup_missing_launch_timing
font_startup_missing_window = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-window')
assert 'Font startup window evidence missing or changed' \
    in font_startup_missing_window
font_startup_window_not_shown = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='window-not-shown')
assert 'Font startup window evidence missing or changed' \
    in font_startup_window_not_shown
font_startup_warmup_before_window = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='warmup-before-window')
assert 'font database warmup started before the startup window was shown' \
    in font_startup_warmup_before_window
font_startup_missing_timing = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-timing')
assert 'StartFonts cold exceeded the 100ms budget or had no timing' \
    in font_startup_missing_timing
font_startup_fallback_mismatch = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='fallback-mismatch')
assert 'font fallback regression evidence missing or changed.' \
    in font_startup_fallback_mismatch
font_startup_manager_failure = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, manager_cold_ns=100_000_000)
assert 'StartManager cold exceeded the 100ms budget or had no timing' \
    in font_startup_manager_failure
font_startup_warm_manager_failure = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, manager_warm_ns=100_000_000)
assert 'StartManager warm exceeded the 100ms budget or had no timing' \
    in font_startup_warm_manager_failure
font_startup_total_failure = run_font_startup_fixture(
    60_000_000, 50_000_000, 1, manager_cold_ns=50_000_000)
assert 'Font startup through first resolve cold exceeded the 100ms budget' \
    in font_startup_total_failure
font_startup_warm_total_failure = run_font_startup_fixture(
    50_000_000, 60_000_000, 1, manager_warm_ns=50_000_000)
assert 'Font startup through first resolve warm exceeded the 100ms budget' \
    in font_startup_warm_total_failure
font_startup_missing_manager_timing = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-manager-timing')
assert 'StartManager cold exceeded the 100ms budget or had no timing' \
    in font_startup_missing_manager_timing
font_startup_missing_warm_manager_timing = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-manager-warm-timing')
assert 'StartManager warm exceeded the 100ms budget or had no timing' \
    in font_startup_missing_warm_manager_timing
font_startup_warmup_not_ready = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='warmup-not-ready')
assert 'font database warmup did not finish before app initialization' \
    in font_startup_warmup_not_ready
print('macOS font startup timing and failure guards: PASS')
