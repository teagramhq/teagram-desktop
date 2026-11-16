#!/usr/bin/env python3

import os
import re
import socket
from pathlib import Path
import subprocess
import sys
import time


def wait_for_text(path, text, process, timeout=15, peer=None, count=1):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.read_text(errors='replace').count(text) >= count:
            return
        if process.poll() is not None or (peer and peer.poll() is not None):
            raise RuntimeError(f'process exited before {text!r}')
        time.sleep(0.02)
    raise RuntimeError(f'timed out waiting for {text!r}')

def run_recheck(executable, home, temp_arg, env):
    log = home / 'recheck.log'
    release = home / 'release-recheck'
    fake_socket = Path(temp_arg) / f'teagram-recheck-{os.getpid()}'
    child_env = dict(env, TDESKTOP_INSTANCE_RECHECK_HOLD_FILE=str(release),
                     TDESKTOP_INSTANCE_RECHECK_TEST_SOCKET=str(fake_socket),
                     TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE=str(home / 'unused-winner-release'))
    process = None
    server = None
    socket_path = None
    socket_inode = None
    try:
        with log.open('w') as output:
            process = subprocess.Popen([executable, '-noupdate', '-debug'],
                                       env=child_env, stdout=output,
                                       stderr=subprocess.STDOUT)
            marker = 'Single instance recheck regression: initial_failure=1 socket='
            wait_for_text(log, marker, process)
            match = re.search(re.escape(marker) + r'([^\n]+)', log.read_text(errors='replace'))
            if not match:
                raise RuntimeError('socket path was not reported')
            socket_path = Path(match.group(1))
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.settimeout(0.1)
            server.bind(str(socket_path))
            socket_inode = socket_path.lstat().st_ino
            server.listen(1)
            release.touch()
            deadline = time.monotonic() + 15
            while True:
                try:
                    connection, _ = server.accept()
                    break
                except socket.timeout:
                    if process.poll() is not None:
                        raise RuntimeError('relaunch exited without rechecking the server')
                    if time.monotonic() >= deadline:
                        raise RuntimeError('relaunch did not recheck the server')
            with connection:
                connection.settimeout(10)
                command = b''
                while b';' not in command:
                    part = connection.recv(8192)
                    if not part:
                        raise RuntimeError('relaunch disconnected before sending its command')
                    command += part
                if b'CMD:show;' not in command:
                    raise RuntimeError('relaunch did not send its command')
                connection.sendall(b'RES:1_0;')
                if process.wait(timeout=20) != 0:
                    raise RuntimeError('relaunch did not forward to the newly available server')
        if 'Emoji cache relaunch: forwarded=1' not in log.read_text(errors='replace'):
            raise RuntimeError('server response was not handled')
        print('Single instance server recheck=PASS forwarded=1')
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'single instance server recheck failed: {error}', file=sys.stderr)
        if log.exists():
            print(log.read_text(errors='replace'), file=sys.stderr)
        return 1
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait()
        if server:
            server.close()
        if socket_path and socket_inode and socket_path.exists():
            if socket_path.lstat().st_ino == socket_inode:
                socket_path.unlink()


def main():
    executable, home_arg, temp_arg, mode = sys.argv[1:]
    home = Path(home_arg)
    home.mkdir(parents=True, exist_ok=True)
    release = home / 'release-emoji-worker'
    primary_log = home / 'primary.log'
    secondary_log = home / 'secondary.log'
    second_log = home / 'second.log'
    winner_release = home / 'release-winner'
    env = dict(os.environ, HOME=str(home), TMPDIR=temp_arg,
               TDESKTOP_MAC_PROFILE_TEST_HOME=str(home),
               TDESKTOP_MAC_PROTECTED_PATH_INTEGRATION_TEST='1',
               TDESKTOP_FONT_STARTUP_REGRESSION='1')
    env.pop('TDESKTOP_FONT_STARTUP_TEST_FAILURE', None)
    if mode == 'recheck':
        return run_recheck(executable, home, temp_arg, env)
    primary_env = dict(env, TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION='1',
                       TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE=str(release))
    secondary_env = dict(env, TDESKTOP_EMOJI_RELAUNCH_SECONDARY_TEST='1',
                         TDESKTOP_EMOJI_RELAUNCH_TEST_FILE=str(release))
    secondary_env.pop('TDESKTOP_EMOJI_CACHE_SHUTDOWN_REGRESSION', None)
    secondary_env.pop('TDESKTOP_EMOJI_SHUTDOWN_RELEASE_FILE', None)
    if mode == 'competing':
        primary_env['TDESKTOP_EMOJI_RELAUNCH_COMPETING'] = '1'
        secondary_env['TDESKTOP_EMOJI_RELAUNCH_HOLD_FILE'] = str(winner_release)
    processes = []
    try:
        with primary_log.open('w') as primary_output, secondary_log.open('w') as secondary_output, second_log.open('w') as second_output:
            primary = subprocess.Popen([executable, '-noupdate', '-debug'],
                                       env=primary_env, stdout=primary_output,
                                       stderr=subprocess.STDOUT)
            processes.append(primary)
            wait_for_text(primary_log, 'Emoji cache shutdown regression: drain_active=1', primary)
            secondary_env['TDESKTOP_EMOJI_RELAUNCH_INDEX'] = '1'
            secondary = subprocess.Popen([executable, '-noupdate', '-debug'],
                                         env=secondary_env, stdout=secondary_output,
                                         stderr=subprocess.STDOUT)
            processes.append(secondary)
            if mode == 'competing':
                third_env = dict(secondary_env, TDESKTOP_EMOJI_RELAUNCH_INDEX='2')
                third = subprocess.Popen([executable, '-noupdate', '-debug'],
                                         env=third_env, stdout=second_output,
                                         stderr=subprocess.STDOUT)
                processes.append(third)
            wait_for_text(primary_log, 'Emoji cache shutdown regression: peer_waiting=1',
                          primary, peer=secondary, count=2 if mode == 'competing' else 1)
            if mode == 'competing' and third.poll() is not None:
                raise RuntimeError('second relaunch exited while draining')
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
            if mode == 'competing':
                deadline = time.monotonic() + 30
                clients = [(secondary, secondary_log), (third, second_log)]
                while time.monotonic() < deadline:
                    logs = [path.read_text(errors='replace') for _, path in clients]
                    winners = [i for i, text in enumerate(logs)
                               if 'Emoji cache relaunch: application_started=1' in text]
                    followers = [i for i, text in enumerate(logs)
                                 if 'Emoji cache relaunch: forwarded=1' in text]
                    if len(winners) > 1 or len(followers) > 1:
                        raise RuntimeError('competing launches chose multiple winners or followers')
                    if len(winners) == len(followers) == 1 and winners[0] != followers[0]:
                        break
                    if any(process.poll() is not None and i not in followers
                           for i, (process, _) in enumerate(clients)):
                        raise RuntimeError('a competing relaunch exited without forwarding')
                    time.sleep(0.02)
                else:
                    raise RuntimeError('competing launches did not elect one live winner')
                winner = clients[winners[0]][0]
                follower = clients[followers[0]][0]
                if winner.poll() is not None or follower.wait(timeout=10) != 0:
                    raise RuntimeError('forwarding did not reach a live winner')
                winner_release.touch()
                if winner.wait(timeout=30) != 0:
                    raise RuntimeError('winning relaunch did not exit cleanly')
            elif secondary.wait(timeout=30) != 0:
                raise RuntimeError('relaunch did not exit cleanly after startup')
        text = secondary_log.read_text(errors='replace')
        if mode != 'competing' and 'Font database warmup regression: ready=1 custom-fonts=1' not in text:
            raise RuntimeError('relaunch did not start a new application')
        print(f'Emoji cache shutdown relaunch={mode} PASS waited=1 new_application=1')
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'emoji cache shutdown relaunch failed: {error}', file=sys.stderr)
        for path in (primary_log, secondary_log, second_log):
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
