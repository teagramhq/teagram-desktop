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
    if [[ "${TDESKTOP_FONT_STARTUP_REGRESSION_SKIP_EXPOSE_CALLBACK:-}" == 1 ]]; then
        printf 'Font startup exposure timeout regression: wall_ns=500000000\\n'
    else
        printf 'Font startup splash regression: visible=1 wall_ns=30000000\\n'
    fi
    printf 'Font database warmup regression: started=1\\n'
    printf 'Font database warmup regression: ready=1 custom-fonts=1\\n'
    printf 'Font fallback regression: emoji=Apple Color Emoji arabic=Vazirmatn UI NL wall_ns=1000000 result=PASS\\n'
    exit 0
fi
if [[ "${TDESKTOP_FONT_STARTUP_QUIT_REGRESSION:-}" == 1 ]]; then
    printf 'Font shutdown regression: teardown=deferred\\n'
    printf 'Font shutdown regression: worker_started=1 quit_observed=1 resolver_cancelled=1 result=PASS\\n'
    printf 'Font shutdown regression: teardown=complete\\n'
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
        splash_cold_ns=30_000_000,
        splash_warm_ns=30_000_000,
        exposure_timeout_ns=500_000_000):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        executable = root / 'Teagram.app/Contents/MacOS/Teagram'
        executable.parent.mkdir(parents=True)
        executable.write_text('''#!/bin/bash
count_file="$HOME/font-startup-count"
mode="$FONT_TEST_MODE"
if [[ "${TDESKTOP_FONT_STARTUP_QUIT_REGRESSION:-}" == 1 ]]; then
    if [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-missing-worker ]]; then
        printf 'Font shutdown regression: teardown=deferred\\n'
        printf 'Font shutdown regression: worker_started=0 quit_observed=1 resolver_cancelled=1 result=FAIL\\n'
        printf 'Font shutdown regression: teardown=complete\\n'
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-order ]]; then
        printf 'Font shutdown regression: teardown=complete\\n'
        printf 'Font shutdown regression: teardown=deferred\\n'
        printf 'Font shutdown regression: worker_started=1 quit_observed=1 resolver_cancelled=1 result=PASS\\n'
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-missing-teardown ]]; then
        printf 'Font shutdown regression: teardown=deferred\\n'
        printf 'Font shutdown regression: worker_started=1 quit_observed=1 resolver_cancelled=1 result=PASS\\n'
    else
        printf 'Font shutdown regression: teardown=deferred\\n'
        printf 'Font shutdown regression: worker_started=1 quit_observed=1 resolver_cancelled=1 result=PASS\\n'
        printf 'Font shutdown regression: teardown=complete\\n'
    fi
    exit 0
fi
if [[ -f "$count_file" ]]; then
    wall_ns="$FONT_WARM_NS"
    splash_wall_ns="$FONT_SPLASH_WARM_NS"
    sample='warm'
else
    wall_ns="$FONT_COLD_NS"
    splash_wall_ns="$FONT_SPLASH_COLD_NS"
    sample='cold'
    printf 'started' > "$count_file"
fi
if [[ "$mode" == font-stall ]]; then
    printf 'UI stall 197 ms (started 2026-11-13T00:00:00): QFontDatabase::addApplicationFont <- CTFontManagerCopyAvailableFontFamilyNames\\n'
elif [[ "$mode" == harfbuzz-stall ]]; then
    printf 'UI stall 182 ms (started 2026-11-13T00:00:00): QTextEngine::shape <- QFontMetricsF::horizontalAdvance\\n'
elif [[ "$mode" == qrawfont-stall ]]; then
    printf 'UI stall 120 ms (started 2026-11-13T00:00:00): QRawFont::fromFont <- glyphIndexesForString\\n'
elif [[ "$mode" == media-stall ]]; then
    printf 'UI stall 155 ms (started 2026-11-13T00:00:00): Core::Application::mediaDevices() <- AVCapture defaults\\n'
fi
if [[ "$mode" == warmup-before-splash ]]; then
    printf 'Font database warmup regression: started=1\\n'
fi
if [[ "$mode" == exposure-timeout \
    || "${TDESKTOP_FONT_STARTUP_REGRESSION_SKIP_EXPOSE_CALLBACK:-}" == 1 ]]; then
    printf 'Font startup exposure timeout regression: wall_ns=%s\\n' \\
        "$FONT_EXPOSURE_TIMEOUT_NS"
fi
if [[ "$mode" != missing-splash && "$mode" != exposure-timeout \
    && "${TDESKTOP_FONT_STARTUP_REGRESSION_SKIP_EXPOSE_CALLBACK:-}" != 1 ]]; then
    if [[ "$mode" == splash-not-shown ]]; then
        printf 'Font startup splash regression: visible=0 wall_ns=%s\\n' \\
            "$splash_wall_ns"
    else
        printf 'Font startup splash regression: visible=1 wall_ns=%s\\n' \\
            "$splash_wall_ns"
    fi
fi
if [[ "$mode" != warmup-before-splash ]]; then
    printf 'Font database warmup regression: started=1\\n'
fi
if [[ "$mode" != missing-timing ]]; then
    printf 'StartFonts startup regression: wall_ns=%s\\n' "$wall_ns"
fi
if [[ "$mode" == warmup-not-ready ]]; then
    printf 'Font database warmup regression: ready=0\\n'
elif [[ "$mode" == missing-custom-fonts ]]; then
    printf 'Font database warmup regression: ready=1 custom-fonts=0\\n'
else
    printf 'Font database warmup regression: ready=1 custom-fonts=1\\n'
fi
emoji_family='Apple Color Emoji'
arabic_family='Vazirmatn UI NL'
if [[ "$mode" == fallback-mismatch ]]; then
    arabic_family='Unexpected Family'
elif [[ "$mode" == apple-emoji-ui ]]; then
    emoji_family='.Apple Color Emoji UI'
fi
printf 'Font fallback regression: emoji=%s arabic=%s wall_ns=%s result=PASS\\n' \\
    "$emoji_family" "$arabic_family" "$FONT_FALLBACK_WALL_NS"
if [[ "$mode" == launch-exit-failure ]]; then
    exit 139
fi
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
            FONT_SPLASH_COLD_NS=str(splash_cold_ns),
            FONT_SPLASH_WARM_NS=str(splash_warm_ns),
            FONT_EXPOSURE_TIMEOUT_NS=str(exposure_timeout_ns),
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
    splash_cold_ns=30_000_000, splash_warm_ns=30_000_000)
assert 'StartFonts cold=PASS wall_ns=50000000 under_100ms=1' in font_startup_output
assert 'StartFonts warm=PASS wall_ns=50000000 under_100ms=1' in font_startup_output
assert 'Font startup splash=PASS sample=cold visible=1 wall_ns=30000000' \
    in font_startup_output
assert 'Font startup splash=PASS sample=warm visible=1 wall_ns=30000000' \
    in font_startup_output
assert 'Font database warmup order=PASS sample=cold' in font_startup_output
assert 'Font database warmup order=PASS sample=warm' in font_startup_output
assert 'Font database warmup before app run=PASS sample=cold' in font_startup_output
assert 'Font database warmup before app run=PASS sample=warm' in font_startup_output
assert 'Font-code startup stall gate=PASS sample=cold' in font_startup_output
assert 'Font-code startup stall gate=PASS sample=warm' in font_startup_output
assert 'fallback=PASS font_stall_gate=PASS' \
    in font_startup_output
assert 'Font shutdown teardown=PASS worker_active=1 order=PASS' \
    in font_startup_output
font_startup_failure = run_font_startup_fixture(50_000_000, 100_000_000, 1)
assert 'StartFonts warm exceeded the 100ms budget' in font_startup_failure
font_startup_cold_failure = run_font_startup_fixture(100_000_000, 50_000_000, 1)
assert 'StartFonts cold exceeded the 100ms budget' in font_startup_cold_failure
font_startup_font_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='font-stall')
assert 'Font-code startup cold produced a main-thread stall' in font_startup_font_stall
font_startup_harfbuzz_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='harfbuzz-stall')
assert 'Font-code startup cold produced a main-thread stall' \
    in font_startup_harfbuzz_stall
font_startup_qrawfont_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='qrawfont-stall')
assert 'Font-code startup cold produced a main-thread stall' \
    in font_startup_qrawfont_stall
font_startup_media_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, mode='media-stall')
assert 'Font-code startup stall gate=PASS sample=cold' in font_startup_media_stall
font_startup_unbudgeted_splash = run_font_startup_fixture(
    50_000_000, 50_000_000, 0,
    splash_cold_ns=876_000_000, splash_warm_ns=150_000_000)
assert 'Font startup splash=PASS sample=cold visible=1 wall_ns=876000000' \
    in font_startup_unbudgeted_splash
assert 'Font startup splash=PASS sample=warm visible=1 wall_ns=150000000' \
    in font_startup_unbudgeted_splash
font_startup_missing_splash = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-splash')
assert 'Font startup splash evidence missing or exposure fallback exceeded 2s' \
    in font_startup_missing_splash
font_startup_splash_not_shown = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='splash-not-shown')
assert 'Font startup splash evidence missing or exposure fallback exceeded 2s' \
    in font_startup_splash_not_shown
font_startup_exposure_timeout = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, mode='exposure-timeout')
assert 'Font database warmup fallback=PASS sample=cold wall_ns=500000000' \
    in font_startup_exposure_timeout
font_startup_warmup_before_splash = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='warmup-before-splash')
assert 'font database warmup started before splash exposure or timeout' \
    in font_startup_warmup_before_splash
font_startup_missing_timing = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-timing')
assert 'StartFonts cold exceeded the 100ms budget or had no timing' \
    in font_startup_missing_timing
font_startup_fallback_mismatch = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='fallback-mismatch')
assert 'font fallback regression evidence missing or changed.' \
    in font_startup_fallback_mismatch
font_startup_launch_failure = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='launch-exit-failure')
assert 'font startup regression launch failed: sample=cold exit_status=139' \
    in font_startup_launch_failure
font_startup_apple_emoji_ui = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, mode='apple-emoji-ui')
assert 'StartFonts cold=PASS wall_ns=50000000 under_100ms=1' \
    in font_startup_apple_emoji_ui
font_startup_warmup_not_ready = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='warmup-not-ready')
assert 'font database warmup did not finish before app initialization' \
    in font_startup_warmup_not_ready
font_startup_missing_custom_fonts = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-custom-fonts')
assert 'bundled fonts were not preloaded before app initialization' \
    in font_startup_missing_custom_fonts
font_startup_shutdown_missing_worker = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-missing-worker')
assert 'font shutdown regression did not observe an active worker and quit' \
    in font_startup_shutdown_missing_worker
font_startup_shutdown_order = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-order')
assert 'font shutdown teardown was not deferred until after resolver unwind' \
    in font_startup_shutdown_order
font_startup_shutdown_missing_teardown = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-missing-teardown')
assert 'font shutdown teardown was not deferred until after resolver unwind' \
    in font_startup_shutdown_missing_teardown
print('macOS font startup timing and failure guards: PASS')
