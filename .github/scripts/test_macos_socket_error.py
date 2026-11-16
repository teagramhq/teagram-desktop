#!/usr/bin/env python3

import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    executable, home_arg, temp_arg = sys.argv[1:]
    for mode in ('transient', 'unknown', 'persistent', 'lock-error', 'lock-refused'):
        home = Path(home_arg) / mode
        home.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, HOME=str(home), TMPDIR=temp_arg,
                   TDESKTOP_MAC_PROFILE_TEST_HOME=str(home),
                   TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST='1',
                   TDESKTOP_FONT_STARTUP_REGRESSION='1',
                   TDESKTOP_SOCKET_ERROR_REGRESSION=mode)
        env.pop('TDESKTOP_FONT_STARTUP_TEST_FAILURE', None)
        env.pop('TDESKTOP_PROFILE_LOCK_REGRESSION', None)
        env.pop('TDESKTOP_PROFILE_LOCK_REFUSAL_REGRESSION', None)
        if mode == 'lock-error':
            env.pop('TDESKTOP_SOCKET_ERROR_REGRESSION', None)
            env['TDESKTOP_PROFILE_LOCK_REGRESSION'] = '1'
        elif mode == 'lock-refused':
            env.pop('TDESKTOP_SOCKET_ERROR_REGRESSION', None)
            env['TDESKTOP_PROFILE_LOCK_REFUSAL_REGRESSION'] = '1'
        try:
            result = subprocess.run([executable, '-noupdate', '-debug'],
                                    env=env, capture_output=True, text=True,
                                    timeout=30, check=False)
        except subprocess.TimeoutExpired:
            print(f'socket error regression hung: mode={mode}', file=sys.stderr)
            return 1
        output = result.stdout + result.stderr
        expected = ('Font database warmup regression: ready=1 custom-fonts=1'
                    if mode != 'persistent' else
                    'Socket error regression: failure_window=1')
        if mode == 'lock-error':
            passed = re.search(r'Profile lock regression: failure_window=1 '
                               r'window_contains_error=1 reason=.+', output)
        elif mode == 'lock-refused':
            protected = home / 'Library/Group Containers/6N38VWS5BX.ru.keepcoder.Telegram/refused-profile.lock'
            policy = re.search(
                r'Mac protected path refusal: operation=(\w+) '
                r'class=group-container callsite=[^\n]*Sandbox::start', output)
            window = re.search(
                r'Profile lock refusal regression: failure_window=1 '
                r'window_contains_error=1 operation=(\w+) path=([^\n]+)', output)
            passed = (policy and window and policy.group(1) == 'stat'
                      and window.group(1) == policy.group(1)
                      and window.group(2) == str(protected)
                      and not protected.exists())
        else:
            passed = ('Socket error regression: injected=1' in output
                      and expected in output)
        expected_status = 1 if mode == 'lock-refused' else 0
        if result.returncode != expected_status or not passed:
            print(output, file=sys.stderr)
            print(f'socket error regression failed: mode={mode}', file=sys.stderr)
            return 1
        print(f'Socket error recovery=PASS mode={mode}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
