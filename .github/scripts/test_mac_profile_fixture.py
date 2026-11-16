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
if [[ -n "${TDESKTOP_SOCKET_ERROR_REGRESSION:-}" ]]; then
    printf 'Socket error regression: injected=1\\n'
    if [[ "$TDESKTOP_SOCKET_ERROR_REGRESSION" == persistent ]]; then
        if [[ "${FONT_TEST_MODE:-}" != socket-error-no-window ]]; then
            printf 'Socket error regression: failure_window=1\\n'
        fi
        exit 0
    fi
    if [[ "${FONT_TEST_MODE:-}" == socket-error-no-retry ]]; then exit 1; fi
fi
if [[ -n "${TDESKTOP_INSTANCE_RECHECK_HOLD_FILE:-}" ]]; then
    printf 'Single instance recheck regression: initial_failure=1 socket=%s\\n' "$TDESKTOP_INSTANCE_RECHECK_TEST_SOCKET"
    while [[ ! -e "$TDESKTOP_INSTANCE_RECHECK_HOLD_FILE" ]]; do sleep 0.02; done
    if [[ "${FONT_TEST_MODE:-}" == emoji-relaunch-skip-recheck ]]; then exit 1; fi
    python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"CMD:show;"); s.recv(128)' "$TDESKTOP_INSTANCE_RECHECK_TEST_SOCKET"
    printf 'Emoji cache relaunch: forwarded=1\\n'
    exit 0
fi
if [[ -n "${TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE:-}" ]]; then
    printf 'Emoji cache shutdown regression: drain_active=1\\n'
    if [[ "${TDESKTOP_EMOJI_RELAUNCH_COMPETING:-}" == 1 ]]; then
        for index in 1 2; do
            while [[ ! -e "$TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE.peer.$index" ]]; do sleep 0.02; done
            printf 'Emoji cache shutdown regression: peer_waiting=1\\n'
        done
    else
        while [[ ! -e "$TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE.peer" ]]; do sleep 0.02; done
        printf 'Emoji cache shutdown regression: peer_waiting=1\\n'
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
    if [[ "${FONT_TEST_MODE:-}" == emoji-relaunch-early ]]; then exit 0; fi
    while [[ ! -e "$TDESKTOP_EMOJI_RELAUNCH_TEST_FILE" ]]; do sleep 0.02; done
    if [[ -n "${TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE:-}" ]]; then
        if [[ "${FONT_TEST_MODE:-}" == emoji-relaunch-double-start ]] || mkdir "$TDESKTOP_EMOJI_RELAUNCH_TEST_FILE.winner" 2>/dev/null; then
            printf 'Emoji cache relaunch: application_started=1\\n'
            while [[ ! -e "$TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE" ]]; do sleep 0.02; done
        else
            printf 'Emoji cache relaunch: forwarded=1\\n'
        fi
        exit 0
    fi
    printf 'Font database warmup regression: ready=1 custom-fonts=1\\n'
    exit 0
fi
if [[ "${TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION:-}" == 1 ]]; then
    printf 'Emoji cache shutdown regression: worker_blocked=1\\n'
    printf 'Emoji cache shutdown regression: repeat_quit=1\\n'
    printf 'Emoji cache shutdown regression: gui_responsive=1 integration_alive=1 worker_returned=1 wait_timed_out=0\\n'
    exit 0
fi
if [[ "${TDESKTOP_FONT_STARTUP_REGRESSION:-}" == 1 ]]; then
    if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == thread-start ]]; then
        printf 'Font database warmup regression: started=0\\n'
        printf 'Font database warmup regression: ready=0\\n'
        printf 'StartFonts startup regression: wall_ns=150000000\\n'
        exit 0
    fi
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
    printf 'Font database metrics warmup regression: ready=1\\n'
    printf 'Font database warmup regression: ready=1 custom-fonts=1\\n'
    printf 'Font fallback regression: emoji=Apple Color Emoji arabic=Vazirmatn UI NL wall_ns=1000000 result=PASS\\n'
    exit 0
fi
if [[ "${TDESKTOP_FONT_STARTUP_QUIT_REGRESSION:-}" == 1 ]]; then
    if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == signal-install ]]; then
        printf 'Font shutdown regression: probe=unavailable\\n'
        exit 1
    fi
    if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-timeout ]]; then
        printf 'Font shutdown regression: last_other_work_frames=worker <- pthread_start\\n'
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=0\\n'
        exit 0
    fi
    if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-timeout-retry ]]; then
        printf 'Font shutdown regression: retry_checkpoint=1\\n'
        printf 'Font shutdown regression: retry_timeout=1\\n'
        printf 'Font shutdown regression: retry_gui_event=1\\n'
        printf 'Font shutdown regression: retry_worker_done=1\\n'
        printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\\n'
        exit 0
    fi
    if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-missed-first ]]; then
        printf 'Font shutdown regression: enumeration_retry=1\\n'
    fi
    printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=1\\n'
    printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\\n'
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
    if [[ ( "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == signal-install \
        && "$FONT_SHUTDOWN_TEST_MODE" != probe-injection-ignored ) \
        || "$FONT_SHUTDOWN_TEST_MODE" == shutdown-probe-unavailable ]]; then
        printf 'Font shutdown regression: probe=unavailable\\n'
        exit 1
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-probe-kill ]]; then
        printf 'Font shutdown regression: probe_error=pthread_kill code=3\\n'
        exit 0
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-probe-ack ]]; then
        printf 'Font shutdown regression: probe_error=handler_ack_timeout\\n'
        exit 1
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-probe-release ]]; then
        printf 'Font shutdown regression: probe_error=handler_release_timeout\\n'
        exit 1
    elif [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-timeout \
        && "$FONT_SHUTDOWN_TEST_MODE" == timeout-no-frames ]]; then
        printf 'Font shutdown regression: last_other_work_frames=unavailable\\n'
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=0\\n'
        exit 0
    elif [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-timeout \
        && "$FONT_SHUTDOWN_TEST_MODE" != timeout-injection-ignored ]]; then
        printf 'Font shutdown regression: last_other_work_frames=worker <- pthread_start\\n'
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=0\\n'
        exit 0
    elif [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-timeout-retry ]]; then
        printf 'Font shutdown regression: retry_checkpoint=1\\n'
        printf 'Font shutdown regression: retry_timeout=1\\n'
        if [[ "$FONT_SHUTDOWN_TEST_MODE" == retry-timeout-bad-order ]]; then
            printf 'Font shutdown regression: retry_worker_done=1\\n'
            printf 'Font shutdown regression: retry_gui_event=1\\n'
        else
            printf 'Font shutdown regression: retry_gui_event=1\\n'
            printf 'Font shutdown regression: retry_worker_done=1\\n'
        fi
        printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\\n'
        exit 0
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-not-in-flight ]]; then
        printf 'Font shutdown regression: quit_observed=0 font_work=active enumeration_overlapped=0\\n'
        printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\\n'
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-no-overlap ]]; then
        printf 'Font shutdown regression: last_other_work_frames=frame0 <- frame1 <- frame2 <- frame3 <- frame4 <- frame5 <- frame6 <- frame7\\n'
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=0\\n'
        printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\\n'
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-order ]]; then
        printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\\n'
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=1\\n'
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-application-created ]]; then
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=1\\n'
        printf 'Font shutdown regression: worker_joined=1 application_created=1 teardown=complete\\n'
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-missing-teardown ]]; then
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=1\\n'
    elif [[ "$FONT_SHUTDOWN_TEST_MODE" == shutdown-crash ]]; then
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=1\\n'
        exit 139
    else
        if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == probe-missed-first \\
            && "$FONT_SHUTDOWN_TEST_MODE" != retry-injection-ignored ]]; then
            printf 'Font shutdown regression: enumeration_retry=1\\n'
        fi
        printf 'Font shutdown regression: quit_observed=1 font_work=active enumeration_overlapped=1\\n'
        printf 'Font shutdown regression: worker_joined=1 application_created=0 teardown=complete\\n'
    fi
    exit 0
fi
if [[ -n "${TDESKTOP_SOCKET_ERROR_REGRESSION:-}" ]]; then
    printf 'Socket error regression: injected=1\\n'
    if [[ "$TDESKTOP_SOCKET_ERROR_REGRESSION" == persistent ]]; then
        if [[ "${FONT_TEST_MODE:-}" != socket-error-no-window ]]; then
            printf 'Socket error regression: failure_window=1\\n'
        fi
        exit 0
    fi
    if [[ "${FONT_TEST_MODE:-}" == socket-error-no-retry ]]; then exit 1; fi
fi
if [[ -n "${TDESKTOP_INSTANCE_RECHECK_HOLD_FILE:-}" ]]; then
    printf 'Single instance recheck regression: initial_failure=1 socket=%s\\n' "$TDESKTOP_INSTANCE_RECHECK_TEST_SOCKET"
    while [[ ! -e "$TDESKTOP_INSTANCE_RECHECK_HOLD_FILE" ]]; do sleep 0.02; done
    if [[ "${FONT_TEST_MODE:-}" == emoji-relaunch-skip-recheck ]]; then exit 1; fi
    python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(b"CMD:show;"); s.recv(128)' "$TDESKTOP_INSTANCE_RECHECK_TEST_SOCKET"
    printf 'Emoji cache relaunch: forwarded=1\\n'
    exit 0
fi
if [[ -n "${TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE:-}" ]]; then
    printf 'Emoji cache shutdown regression: drain_active=1\\n'
    if [[ "${TDESKTOP_EMOJI_RELAUNCH_COMPETING:-}" == 1 ]]; then
        for index in 1 2; do
            while [[ ! -e "$TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE.peer.$index" ]]; do sleep 0.02; done
            printf 'Emoji cache shutdown regression: peer_waiting=1\\n'
        done
    else
        while [[ ! -e "$TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE.peer" ]]; do sleep 0.02; done
        printf 'Emoji cache shutdown regression: peer_waiting=1\\n'
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
    if [[ "${FONT_TEST_MODE:-}" == emoji-relaunch-early ]]; then exit 0; fi
    while [[ ! -e "$TDESKTOP_EMOJI_RELAUNCH_TEST_FILE" ]]; do sleep 0.02; done
    if [[ -n "${TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE:-}" ]]; then
        if [[ "${FONT_TEST_MODE:-}" == emoji-relaunch-double-start ]] || mkdir "$TDESKTOP_EMOJI_RELAUNCH_TEST_FILE.winner" 2>/dev/null; then
            printf 'Emoji cache relaunch: application_started=1\\n'
            while [[ ! -e "$TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE" ]]; do sleep 0.02; done
        else
            printf 'Emoji cache relaunch: forwarded=1\\n'
        fi
        exit 0
    fi
    printf 'Font database warmup regression: ready=1 custom-fonts=1\\n'
    exit 0
fi
if [[ "${TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION:-}" == 1 ]]; then
    printf 'Emoji cache shutdown regression: worker_blocked=1\\n'
    if [[ "$mode" != emoji-shutdown-no-repeat ]]; then
        printf 'Emoji cache shutdown regression: repeat_quit=1\\n'
    fi
    if [[ "$mode" == emoji-shutdown-early ]]; then
        printf 'Emoji cache shutdown regression: gui_responsive=0 integration_alive=0 worker_returned=0 wait_timed_out=0\\n'
    else
        printf 'Emoji cache shutdown regression: gui_responsive=1 integration_alive=1 worker_returned=1 wait_timed_out=0\\n'
    fi
    exit 0
fi
if [[ "${TDESKTOP_FONT_STARTUP_TEST_FAILURE:-}" == thread-start \
    && "$mode" != warmup-injection-ignored ]]; then
    printf 'Font database warmup regression: started=0\\n'
    printf 'Font database warmup regression: ready=0\\n'
    printf 'StartFonts startup regression: wall_ns=150000000\\n'
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
if [[ "$mode" == emoji-shutdown-invalid ]]; then
    printf 'Mac protected path refusal: operation=read class=invalid callsite=emojiCacheFolder\\n'
fi
if [[ "$mode" == font-stall ]]; then
    printf 'UI stall 197 ms (started 2026-11-13T00:00:00): QFontDatabase::addApplicationFont <- CTFontManagerCopyAvailableFontFamilyNames\\n'
elif [[ "$mode" == descriptor-stall ]]; then
    printf 'UI stall 197 ms (started 2026-11-13T00:00:00): populateFromDescriptor\\n'
elif [[ "$mode" == populate-database-stall ]]; then
    printf 'UI stall 197 ms (started 2026-11-13T00:00:00): populateFontDatabase\\n'
elif [[ "$mode" == qt-families-stall ]]; then
    printf 'UI stall 197 ms (started 2026-11-13T00:00:00): QFontDatabase::families\\n'
elif [[ "$mode" == harfbuzz-stall ]]; then
    printf 'UI stall 182 ms (started 2026-11-13T00:00:00): QTextEngine::shape <- QFontMetricsF::horizontalAdvance\\n'
elif [[ "$mode" == qrawfont-stall ]]; then
    printf 'UI stall 120 ms (started 2026-11-13T00:00:00): QRawFont::fromFont <- glyphIndexesForString\\n'
elif [[ "$mode" == system-menu-stall ]]; then
    printf 'UI stall 229 ms (started 2026-11-13T00:00:00): TOpenTypeMorph::AddShapingGlyphs <- __CTFont <- NSMenuBarRepresentation::layoutMenuBarIfNeeded\\n'
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
if [[ "$mode" == warmup-start-failure ]]; then
    printf 'Font database warmup regression: started=0 error=simulated\\n'
elif [[ "$mode" != warmup-before-splash ]]; then
    printf 'Font database warmup regression: started=1\\n'
fi
if [[ "$mode" != warmup-not-ready && "$mode" != missing-metrics ]]; then
    printf 'Font database metrics warmup regression: ready=1\\n'
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
assert 'Font shutdown teardown=PASS font_work_active=1 application_created=0' \
    in font_startup_output
assert 'Font shutdown retry after missed enumeration=PASS' in font_startup_output
assert 'Font shutdown probe installation failure=PASS' in font_startup_output
assert 'Font shutdown probe timeout diagnostics=PASS' in font_startup_output
assert 'Font shutdown retry timeout ordering=PASS' in font_startup_output
assert 'Font database warmup thread failure fallback=PASS' in font_startup_output
assert 'Emoji cache shutdown lifetime and GUI responsiveness=PASS' in font_startup_output
assert 'Emoji cache shutdown relaunch=normal PASS waited=1 new_application=1' in font_startup_output
assert 'Emoji cache shutdown relaunch=crash PASS waited=1 new_application=1' in font_startup_output
assert 'Emoji cache shutdown relaunch=competing PASS waited=1 new_application=1' in font_startup_output
assert 'Single instance server recheck=PASS forwarded=1' in font_startup_output
assert 'Socket error recovery=PASS mode=transient' in font_startup_output
assert 'Socket error recovery=PASS mode=unknown' in font_startup_output
assert 'Socket error recovery=PASS mode=persistent' in font_startup_output
font_startup_socket_no_retry = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='socket-error-no-retry')
assert 'socket error regression failed: mode=transient' in font_startup_socket_no_retry
font_startup_socket_no_window = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='socket-error-no-window')
assert 'socket error regression failed: mode=persistent' in font_startup_socket_no_window
font_startup_no_recheck = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='emoji-relaunch-skip-recheck')
assert 'relaunch exited without rechecking the server' in font_startup_no_recheck
font_startup_double_relaunch = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='emoji-relaunch-double-start')
assert 'competing launches chose multiple winners' in font_startup_double_relaunch
font_startup_early_relaunch = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='emoji-relaunch-early')
assert 'emoji cache shutdown relaunch failed' in font_startup_early_relaunch
font_startup_failure = run_font_startup_fixture(50_000_000, 100_000_000, 1)
assert 'StartFonts warm exceeded the 100ms budget' in font_startup_failure
font_startup_cold_failure = run_font_startup_fixture(100_000_000, 50_000_000, 1)
assert 'StartFonts cold exceeded the 100ms budget' in font_startup_cold_failure
font_startup_emoji_shutdown = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='emoji-shutdown-invalid')
assert 'emoji cache accessed an invalid profile during shutdown' \
    in font_startup_emoji_shutdown
font_startup_emoji_missing_repeat = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='emoji-shutdown-no-repeat')
assert 'emoji cache shutdown ordering was not observed' \
    in font_startup_emoji_missing_repeat
font_startup_emoji_early_shutdown = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='emoji-shutdown-early')
assert 'emoji cache shutdown ordering was not observed' \
    in font_startup_emoji_early_shutdown
font_startup_font_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='font-stall')
assert 'Font-code startup cold produced a main-thread stall' in font_startup_font_stall
for mode in ('populate-database-stall', 'qt-families-stall'):
    output = run_font_startup_fixture(50_000_000, 50_000_000, 1, mode=mode)
    assert 'Font-code startup cold produced a main-thread stall' in output
font_startup_descriptor_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, mode='descriptor-stall')
assert 'Font-code startup stall gate=PASS sample=cold' in font_startup_descriptor_stall
assert 'reported (not gated) stalls:' in font_startup_descriptor_stall
assert 'populateFromDescriptor' in font_startup_descriptor_stall
font_startup_harfbuzz_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, mode='harfbuzz-stall')
assert 'Font-code startup stall gate=PASS sample=cold' \
    in font_startup_harfbuzz_stall
assert 'Font-code startup cold reported (not gated) stalls' \
    in font_startup_harfbuzz_stall
font_startup_qrawfont_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, mode='qrawfont-stall')
assert 'Font-code startup stall gate=PASS sample=cold' \
    in font_startup_qrawfont_stall
assert 'Font-code startup cold reported (not gated) stalls' \
    in font_startup_qrawfont_stall
font_startup_system_menu_stall = run_font_startup_fixture(
    50_000_000, 50_000_000, 0, mode='system-menu-stall')
assert 'Font-code startup stall gate=PASS sample=cold' \
    in font_startup_system_menu_stall
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
font_startup_warmup_start_failure = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='warmup-start-failure')
assert 'font database warmup worker failed to start; startup falls back to the main thread' \
    in font_startup_warmup_start_failure
assert 'started=0 error=simulated' in font_startup_warmup_start_failure
font_warmup_injection_ignored = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='warmup-injection-ignored')
assert 'font warmup thread failure fallback was not detected' in font_warmup_injection_ignored
font_startup_missing_metrics = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-metrics')
assert 'font metrics were not prewarmed off the UI thread: cold' \
    in font_startup_missing_metrics
font_startup_missing_custom_fonts = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='missing-custom-fonts')
assert 'bundled fonts were not preloaded before app initialization' \
    in font_startup_missing_custom_fonts
font_startup_shutdown_not_in_flight = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-not-in-flight')
assert 'font shutdown regression did not quit during an active system font enumeration' \
    in font_startup_shutdown_not_in_flight
font_startup_shutdown_no_overlap = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-no-overlap')
assert 'font shutdown regression did not quit during an active system font enumeration' \
    in font_startup_shutdown_no_overlap
assert 'last_other_work_frames=frame0 <- frame1 <- frame2 <- frame3 <- frame4 <- frame5 <- frame6 <- frame7' \
    in font_startup_shutdown_no_overlap
font_startup_shutdown_order = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-order')
assert 'font shutdown did not join the worker and tear down without starting the application' \
    in font_startup_shutdown_order
font_startup_shutdown_application_created = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-application-created')
assert 'font shutdown did not join the worker and tear down without starting the application' \
    in font_startup_shutdown_application_created
font_startup_shutdown_missing_teardown = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-missing-teardown')
assert 'font shutdown did not join the worker and tear down without starting the application' \
    in font_startup_shutdown_missing_teardown
font_startup_shutdown_crash = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='shutdown-crash')
assert 'font shutdown regression did not exit cleanly: exit_status=139' \
    in font_startup_shutdown_crash
for probe_mode, detail in (
        ('shutdown-probe-unavailable', 'probe=unavailable'),
        ('shutdown-probe-kill', 'probe_error=pthread_kill code=3'),
        ('shutdown-probe-ack', 'probe_error=handler_ack_timeout'),
        ('shutdown-probe-release', 'probe_error=handler_release_timeout')):
    output = run_font_startup_fixture(50_000_000, 50_000_000, 1, mode=probe_mode)
    assert detail in output
    assert 'font shutdown regression probe failed' in output
font_probe_injection_ignored = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='probe-injection-ignored')
assert 'font shutdown regression accepted a failed probe installation' \
    in font_probe_injection_ignored
font_retry_injection_ignored = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='retry-injection-ignored')
assert 'font shutdown enumeration retry was not observed' \
    in font_retry_injection_ignored
font_timeout_injection_ignored = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='timeout-injection-ignored')
assert 'font shutdown regression accepted a probe timeout' in font_timeout_injection_ignored
font_timeout_no_frames = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='timeout-no-frames')
assert 'font shutdown probe timeout did not report the sampled frames' \
    in font_timeout_no_frames
font_retry_timeout_bad_order = run_font_startup_fixture(
    50_000_000, 50_000_000, 1, mode='retry-timeout-bad-order')
assert 'font shutdown retry timeout ordering was not observed' \
    in font_retry_timeout_bad_order
print('macOS font startup timing and failure guards: PASS')


def run_start_in_tray_fixture(mode, expected_status):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        executable = root / 'Teagram.app/Contents/MacOS/Teagram'
        executable.parent.mkdir(parents=True)
        executable.write_text('''#!/bin/bash
if [[ " $* " == *" -quit "* ]]; then
    kill -TERM "$(cat "$HOME/fixture-pid")"
    exit 0
fi
if [[ " $* " != *" -startintray "* \
    || "${TDESKTOP_MAC_START_IN_TRAY_REGRESSION:-}" != 1 ]]; then
    echo 'missing tray launch argument or regression flag' >&2
    exit 1
fi
printf '%s\\n' "$$" > "$HOME/fixture-pid"
trap 'exit 0' TERM
case "$TRAY_TEST_MODE" in
    exit-before-startup) exit 0 ;;
    visible)
        printf 'Mac start-in-tray regression: startup=complete start_intray=1 window_and_tray=1 hidden=0 splash_visible=1 splash_destroyed=1\\n'
        for ((i = 0; i < 20; ++i)); do sleep 0.1 & wait "$!" || :; done
        exit 0
        ;;
    wrong-mode)
        printf 'Mac start-in-tray regression: startup=complete start_intray=1 window_and_tray=0 hidden=1 splash_visible=1 splash_destroyed=1\\n'
        for ((i = 0; i < 20; ++i)); do sleep 0.1 & wait "$!" || :; done
        exit 0
        ;;
    *)
        printf 'Mac start-in-tray regression: startup=complete start_intray=1 window_and_tray=1 hidden=1 splash_visible=1 splash_destroyed=1\\n'
        ;;
esac
if [[ "$TRAY_TEST_MODE" == exit-after-startup ]]; then
    sleep 0.1
    exit 0
fi
while :; do sleep 0.1 & wait "$!" || :; done
''')
        executable.chmod(0o755)
        binaries = root / 'bin'
        binaries.mkdir()
        for name, body in {
            'uname': '#!/bin/bash\necho Darwin\n',
            'sw_vers': '#!/bin/bash\necho fixture\n',
            'md5': '#!/usr/bin/env python3\nimport hashlib,sys\nprint(hashlib.md5(sys.stdin.buffer.read()).hexdigest())\n',
        }.items():
            stub = binaries / name
            stub.write_text(body)
            stub.chmod(0o755)
        environment = dict(
            os.environ,
            TDESKTOP_MAC_PROFILE_TEST_TMP_BASE=str(root),
            TRAY_TEST_MODE=mode,
            PATH=str(binaries) + ':' + os.environ['PATH'],
        )
        result = subprocess.run(
            ['bash', 'Telegram/build/test_mac_profile_guard.sh',
             str(executable), 'start-in-tray'],
            env=environment, capture_output=True, text=True, timeout=15)
        output = result.stdout + result.stderr
        assert result.returncode == expected_status, output
        return output


tray_output = run_start_in_tray_fixture('pass', 0)
assert 'start_in_tray_regression=PASS hidden=1 alive_after_startup=1' in tray_output
tray_early_exit = run_start_in_tray_fixture('exit-before-startup', 1)
assert 'start-in-tray process exited before startup completed' in tray_early_exit
tray_visible = run_start_in_tray_fixture('visible', 1)
assert 'start-in-tray process exited before startup completed' in tray_visible
tray_wrong_mode = run_start_in_tray_fixture('wrong-mode', 1)
assert 'start-in-tray process exited before startup completed' in tray_wrong_mode
tray_late_exit = run_start_in_tray_fixture('exit-after-startup', 1)
assert 'start-in-tray process exited after startup' in tray_late_exit
print('macOS start-in-tray lifecycle guard: PASS')
