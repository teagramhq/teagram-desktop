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


def run_font_startup_fixture(cold_ns, warm_ns, expected_status):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        executable = root / 'Teagram.app/Contents/MacOS/Teagram'
        executable.parent.mkdir(parents=True)
        executable.write_text('''#!/bin/bash
count_file="$HOME/font-startup-count"
if [[ -f "$count_file" ]]; then
    wall_ns="$FONT_WARM_NS"
else
    wall_ns="$FONT_COLD_NS"
    printf 'started' > "$count_file"
fi
printf 'StartFonts startup regression: wall_ns=%s\\n' "$wall_ns"
printf 'Font fallback regression: emoji=Apple Color Emoji arabic=Vazirmatn UI NL wall_ns=1000000 result=PASS\\n'
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
            PATH=str(binaries) + ':' + os.environ['PATH'],
        )
        result = subprocess.run(
            ['bash', 'Telegram/build/test_mac_profile_guard.sh',
             str(executable), 'font-startup'],
            env=environment, capture_output=True, text=True, timeout=15)
        output = result.stdout + result.stderr
        assert result.returncode == expected_status, output
        return output


font_startup_output = run_font_startup_fixture(50_000_000, 99_999_999, 0)
assert 'StartFonts cold=PASS wall_ns=50000000 under_100ms=1' in font_startup_output
assert 'StartFonts warm=PASS wall_ns=99999999 under_100ms=1' in font_startup_output
font_startup_failure = run_font_startup_fixture(50_000_000, 100_000_000, 1)
assert 'StartFonts warm exceeded the 100ms budget' in font_startup_failure
print('macOS font startup timing guard: PASS')
