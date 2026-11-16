#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys
import time


def wait_for_text(path, text, process, timeout=15, peer=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and text in path.read_text(errors='replace'):
            return
        if process.poll() is not None or (peer and peer.poll() is not None):
            raise RuntimeError(f'process exited before {text!r}')
        time.sleep(0.02)
    raise RuntimeError(f'timed out waiting for {text!r}')


def main():
    executable, home_arg, temp_arg, mode = sys.argv[1:]
    home = Path(home_arg)
    home.mkdir(parents=True, exist_ok=True)
    release = home / 'release-emoji-worker'
    primary_log = home / 'primary.log'
    secondary_log = home / 'secondary.log'
    env = dict(os.environ, HOME=str(home), TMPDIR=temp_arg,
               TDESKTOP_MAC_PROFILE_TEST_HOME=str(home),
               TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST='1',
               TDESKTOP_FONT_STARTUP_REGRESSION='1')
    env.pop('TDESKTOP_FONT_STARTUP_TEST_FAILURE', None)
    primary_env = dict(env, TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION='1',
                       TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE=str(release))
    secondary_env = dict(env, TDESKTOP_EMOJI_RELAUNCH_SECONDARY_TEST='1',
                         TDESKTOP_EMOJI_RELAUNCH_TEST_FILE=str(release))
    secondary_env.pop('TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION', None)
    secondary_env.pop('TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE', None)
    processes = []
    try:
        with primary_log.open('w') as primary_output, secondary_log.open('w') as secondary_output:
            primary = subprocess.Popen([executable, '-noupdate', '-debug'],
                                       env=primary_env, stdout=primary_output,
                                       stderr=subprocess.STDOUT)
            processes.append(primary)
            wait_for_text(primary_log, 'Emoji cache shutdown regression: drain_active=1', primary)
            secondary = subprocess.Popen([executable, '-noupdate', '-debug'],
                                         env=secondary_env, stdout=secondary_output,
                                         stderr=subprocess.STDOUT)
            processes.append(secondary)
            wait_for_text(primary_log, 'Emoji cache shutdown regression: peer_waiting=1', primary, peer=secondary)
            if secondary.poll() is not None:
                raise RuntimeError('relaunch exited while the previous process was draining')
            if mode == 'crash':
                primary.kill()
                primary.wait(timeout=30)
                release.touch()  # Unblocks the test fixture; the real app ignores it.
            else:
                release.touch()
                if primary.wait(timeout=30) != 0:
                    raise RuntimeError('draining process did not exit cleanly')
            if secondary.wait(timeout=30) != 0:
                raise RuntimeError('relaunch did not exit cleanly after startup')
        text = secondary_log.read_text(errors='replace')
        if 'Font database warmup regression: ready=1 custom-fonts=1' not in text:
            raise RuntimeError('relaunch did not start a new application')
        print(f'Emoji cache shutdown relaunch={mode} PASS waited=1 new_application=1')
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'emoji cache shutdown relaunch failed: {error}', file=sys.stderr)
        for path in (primary_log, secondary_log):
            if path.exists():
                print(path.read_text(errors='replace'), file=sys.stderr)
        return 1
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    return 0


if __name__ == '__main__':
    sys.exit(main())
