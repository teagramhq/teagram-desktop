#!/usr/bin/env python3

import concurrent.futures
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import traceback
from urllib.parse import quote
from urllib.request import urlopen


FIXTURE = '41f14cb01791815af2b42667a04aec313d4a15ebe7ef6b0da3e4f884ec861e95'
MANIFEST_SHA256 = 'a1e1286defa111a187c05237c5f743bb36572b439c20a76e824e7a9af2bfc32f'
SERVER = '11aca52138df628f0ef9972e0583c768d12c221d'
RELEASE_URL = f'https://github.com/teagramhq/teagram-server/releases/download/perf-fixture-{FIXTURE}'
PHASES = ('tool setup', 'download', 'postgres setup', 'restore 1x', 'restore 100x',
          'migrations', 'server build', 'server ready', 'cleanup')


def asset_path(name):
    if not isinstance(name, str) or not name or '/' in name or '\\' in name or '\0' in name:
        raise ValueError(f'unsafe release asset name: {name!r}')
    parts = name.split('__')
    if any(part in ('', '.', '..') for part in parts):
        raise ValueError(f'unsafe release asset path: {name!r}')
    return Path(*parts)


def download_fixture(release_url, destination, version, manifest_sha256):
    with urlopen(f'{release_url}/manifest.json', timeout=45) as response:
        manifest_bytes = response.read()
    if hashlib.sha256(manifest_bytes).hexdigest() != manifest_sha256:
        raise ValueError('fixture manifest checksum mismatch')
    manifest = json.loads(manifest_bytes)
    if manifest.get('fixture') != version or not re.fullmatch(r'[0-9a-f]{64}', version):
        raise ValueError('fixture manifest does not match the pinned version')
    if not re.fullmatch(r'[0-9a-f]{40}', manifest.get('server_commit', '')):
        raise ValueError('fixture manifest has an invalid server commit')
    if not re.fullmatch(r'[0-9]{14}', manifest.get('migration_revision', '')):
        raise ValueError('fixture manifest has an invalid migration revision')
    assets = manifest.get('assets')
    if not isinstance(assets, list) or not assets:
        raise ValueError('fixture manifest has no assets')

    names, paths = set(), set()
    for asset in assets:
        name = asset.get('name')
        path = asset_path(name)
        if name in names or path in paths:
            raise ValueError(f'duplicate release asset: {name}')
        if not re.fullmatch(r'[0-9a-f]{64}', asset.get('sha256', '')):
            raise ValueError(f'invalid checksum for {name}')
        names.add(name)
        paths.add(path)
    if not {'perf_1x.dump', 'perf_100x.dump'}.issubset(names):
        raise ValueError('fixture manifest is missing a database dump')
    for scale in ('1x', '100x'):
        if not any(path.parts[0] == f'blobs-{scale}' for path in paths):
            raise ValueError(f'fixture manifest is missing {scale} blobs')

    def download(asset):
        name = asset['name']
        target = destination / asset_path(name)
        target.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        with urlopen(f'{release_url}/{quote(name)}', timeout=45) as response, target.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != asset['sha256']:
            raise ValueError(f'checksum mismatch for {name}')

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as executor:
        list(executor.map(download, assets))
    return manifest


def validate_migration_history(manifest, fixture_sum, runtime_sum):
    revision = manifest['migration_revision']

    def history(contents):
        entries = []
        for line in contents.splitlines():
            match = re.fullmatch(r'(\d{14})[^ ]*\.sql (h1:\S+)', line)
            if match and match.group(1) <= revision:
                entries.append(line)
        return entries

    seeded, runtime = history(fixture_sum), history(runtime_sum)
    if not seeded or not runtime or not seeded[-1].startswith(revision + '_') or not runtime[-1].startswith(revision + '_'):
        raise ValueError(f'fixture migration revision {revision} is absent from the pinned server')
    if seeded != runtime:
        raise ValueError(f'fixture migration history through {revision} differs from the pinned server')


def validate_restored_revision(scale, actual, expected):
    if actual != expected:
        raise ValueError(f'{scale} database migration revision {actual} does not match manifest {expected}')


def format_summary(status, phase, durations, started_at, finished_at, job_started_at, fingerprints, error=''):
    lines = ['### macOS live performance fixture', '', f'Status: **{status}**']
    if status != 'passed':
        lines.append(f'Failed phase: **{phase}**. {error.replace(chr(10), " ")}')
    lines.extend(['', '| Phase | Wall time |', '| --- | ---: |'])
    for name in PHASES:
        value = f'{durations[name]:.1f} s' if name in durations else 'not reached'
        lines.append(f'| {name} | {value} |')
    lines.append(f'| Total added to job | {finished_at - started_at} s |')
    lines.append('')
    lines.append(f'Fixture started at job elapsed: {started_at - job_started_at} s; '
                 f'finished at job elapsed: {finished_at - job_started_at} s '
                 '(elapsed from the job start timestamp).')
    for scale, fingerprint in fingerprints.items():
        lines.append(f'- {scale}: TCP ready on loopback; server RSA key fingerprint `{fingerprint}`')
    lines.append('')
    return '\n'.join(lines) + '\n'


class Report:
    def __init__(self, started_at):
        self.phase = 'tool setup'
        self.failure_phase = None
        self.durations = {'tool setup': max(0, time.time() - started_at)}
        self.fingerprints = {}
        self.save_duration('tool setup')

    def save_duration(self, name):
        with open(os.environ['PERF_FIXTURE_TIMES_FILE'], 'a', encoding='utf-8') as output:
            output.write(f'{name}|{self.durations[name]:.1f}\n')

    @contextmanager
    def measure(self, name):
        self.phase = name
        if self.failure_phase is None:
            Path(os.environ['PERF_FIXTURE_PHASE_FILE']).write_text(name)
        started = time.monotonic()
        try:
            yield
        except BaseException:
            if self.failure_phase is None:
                self.failure_phase = name
            raise
        finally:
            self.durations[name] = time.monotonic() - started
            self.save_duration(name)


def run(command, *, cwd=None, env=None, capture=False, timeout=600):
    return subprocess.run(command, cwd=cwd, env=env, text=True, check=True,
                          stdout=subprocess.PIPE if capture else None, timeout=timeout)


def available_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


def checkout_server(directory):
    directory.mkdir()
    run(['git', 'init', '-q'], cwd=directory)
    run(['git', 'remote', 'add', 'origin', 'https://github.com/teagramhq/teagram-server.git'], cwd=directory)
    run(['git', 'fetch', '--depth=1', '--no-tags', 'origin', SERVER], cwd=directory, timeout=180)
    run(['git', 'checkout', '--detach', '--quiet', 'FETCH_HEAD'], cwd=directory)
    if run(['git', 'rev-parse', 'HEAD'], cwd=directory, capture=True).stdout.strip() != SERVER:
        raise ValueError('server checkout does not match the pinned revision')


def postgres_binaries():
    env = dict(os.environ, HOMEBREW_NO_AUTO_UPDATE='1', HOMEBREW_NO_INSTALL_CLEANUP='1')
    installed = subprocess.run(['brew', 'list', '--versions', 'postgresql@18'], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if not installed:
        run(['brew', 'install', 'postgresql@18'], env=env, timeout=360)
    prefix = run(['brew', '--prefix', 'postgresql@18'], env=env, capture=True).stdout.strip()
    return Path(prefix) / 'bin'


def wait_for_server(process, log, port, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        output = log.read_text(errors='replace')
        fingerprint = re.search(r'msg="server RSA key"[^\n]*\bfingerprint=(-?\d+)', output)
        listening = f'msg=listening addr=127.0.0.1:{port}' in output
        if process.poll() is not None:
            raise RuntimeError(f'teagramd exited before readiness:\n{output[-6000:]}')
        if fingerprint and listening:
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=1):
                    return fingerprint.group(1)
            except OSError:
                pass
        time.sleep(0.2)
    raise RuntimeError(f'teagramd did not become ready on 127.0.0.1:{port}:\n{log.read_text(errors="replace")[-6000:]}')


def exercise(root, report):
    processes = []
    pg_bin = None
    pg_data = root / 'postgres'
    original_error = None
    try:
        if os.environ['PERF_GO_OUTCOME'] != 'success':
            report.failure_phase = 'tool setup'
            raise RuntimeError('Go setup failed')
        if os.environ['PERF_ATLAS_OUTCOME'] != 'success':
            report.failure_phase = 'tool setup'
            raise RuntimeError('Atlas setup failed')

        with report.measure('download'):
            snapshot = root / 'snapshot'
            server = root / 'server'
            manifest = download_fixture(RELEASE_URL, snapshot, FIXTURE, MANIFEST_SHA256)
            checkout_server(server)
            fixture_url = ('https://raw.githubusercontent.com/teagramhq/teagram-server/'
                           f"{manifest['server_commit']}/migrations/atlas.sum")
            with urlopen(fixture_url, timeout=45) as response:
                fixture_sum = response.read().decode('utf-8')
            runtime_sum = (server / 'migrations/atlas.sum').read_text()
            validate_migration_history(manifest, fixture_sum, runtime_sum)
            seed_helper = (server / 'internal/pgtest/pgtest.go').read_text()
            if not re.search(r'func EncKey\(\) \[\]byte \{\s*return bytes.Repeat\(\[\]byte\{0x2a\}, 32\)', seed_helper):
                raise ValueError('fixture auth-key encryption key differs from the pinned server test helper')

        with report.measure('postgres setup'):
            pg_bin = postgres_binaries()
            pg_port = available_port()
            run([str(pg_bin / 'initdb'), '-D', str(pg_data), '-U', 'postgres',
                 '--auth=trust', '--no-locale', '--encoding=UTF8'], timeout=120)
            options = (f'-h 127.0.0.1 -p {pg_port} -c unix_socket_directories=\'\' '
                       '-c fsync=off -c synchronous_commit=off -c full_page_writes=off -c autovacuum=off')
            run([str(pg_bin / 'pg_ctl'), '-D', str(pg_data), '-l', str(root / 'postgres.log'),
                 '-o', options, '-w', 'start'], timeout=120)
            base_dsn = f'postgres://postgres@127.0.0.1:{pg_port}'
            run([str(pg_bin / 'psql'), '-X', '-v', 'ON_ERROR_STOP=1', '-d', f'{base_dsn}/postgres?sslmode=disable',
                 '-c', 'CREATE ROLE browser_acceptance_probe NOLOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION NOBYPASSRLS'])

        for scale in ('1x', '100x'):
            with report.measure(f'restore {scale}'):
                database = f'teagram_{scale}'
                dsn = f'{base_dsn}/{database}?sslmode=disable'
                run([str(pg_bin / 'createdb'), '-h', '127.0.0.1', '-p', str(pg_port), '-U', 'postgres', database])
                run([str(pg_bin / 'pg_restore'), '--exit-on-error', '--no-owner', '--no-acl', '-j', '4',
                     '-d', dsn, str(snapshot / f'perf_{scale}.dump')])
                actual = run([str(pg_bin / 'psql'), '-XAt', '-v', 'ON_ERROR_STOP=1', '-d', dsn,
                              '-c', 'SELECT version FROM atlas_schema_revisions.atlas_schema_revisions ORDER BY version DESC LIMIT 1'],
                             capture=True).stdout.strip()
                validate_restored_revision(scale, actual, manifest['migration_revision'])

        with report.measure('migrations'):
            for scale in ('1x', '100x'):
                dsn = f'{base_dsn}/teagram_{scale}?sslmode=disable'
                migration_log = root / f'migrations-{scale}.log'
                try:
                    with migration_log.open('w') as output:
                        subprocess.run(['atlas', 'migrate', 'apply', '--url', dsn,
                                        '--dir', f'file://{server / "migrations"}'],
                                       stdout=output, stderr=subprocess.STDOUT, check=True, timeout=600)
                except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                    print(migration_log.read_text(errors='replace')[-12000:], file=sys.stderr)
                    raise
                print(f'{scale} database migrated to the pinned server revision')

        with report.measure('server build'):
            binary = root / 'teagramd'
            run(['go', 'build', '-o', str(binary), './cmd/teagramd'], cwd=server)

        with report.measure('server ready'):
            key = root / 'server-key.pem'
            env = {key: value for key, value in os.environ.items() if not key.startswith('TG_')}
            env.update(TG_RSA_KEY_PATH=str(key), TG_AUTHKEY_ENC_KEY='2a' * 32,
                       TG_PUBLIC_LINK_PREFIX='https://links.example.test/', TG_REGISTRATION='closed')
            run([str(binary), 'bootstrap-identity'], env=env)
            ports = {pg_port}
            servers = []
            for scale in ('1x', '100x'):
                port = available_port()
                while port in ports:
                    port = available_port()
                ports.add(port)
                address = f'127.0.0.1:{port}'
                server_env = dict(env, TG_POSTGRES_DSN=f'{base_dsn}/teagram_{scale}?sslmode=disable',
                                  TG_BLOB_DIR=str(snapshot / f'blobs-{scale}'),
                                  TG_LISTEN_ADDR=address, TG_ADVERTISE_ADDR=address)
                log = root / f'server-{scale}.log'
                with log.open('w') as output:
                    process = subprocess.Popen([str(binary)], cwd=root, env=server_env,
                                               stdout=output, stderr=subprocess.STDOUT)
                processes.append(process)
                servers.append((scale, process, log, port))
            for scale, process, log, port in servers:
                report.fingerprints[scale] = wait_for_server(process, log, port)
    except BaseException as error:
        original_error = error
        if report.failure_phase is None:
            report.failure_phase = report.phase
        postgres_log = root / 'postgres.log'
        try:
            if postgres_log.exists():
                print(f'PostgreSQL log:\n{postgres_log.read_text(errors="replace")[-6000:]}', file=sys.stderr)
        except OSError as error:
            print(f'Could not read PostgreSQL log: {error}', file=sys.stderr)
        raise
    finally:
        try:
            with report.measure('cleanup'):
                cleanup_errors = []
                for process in processes:
                    try:
                        if process.poll() is None:
                            process.terminate()
                            try:
                                process.wait(timeout=10)
                            except subprocess.TimeoutExpired:
                                process.kill()
                                process.wait(timeout=10)
                    except Exception as error:
                        cleanup_errors.append(f'server shutdown failed: {error}')
                if pg_bin and (pg_data / 'postmaster.pid').exists():
                    try:
                        run([str(pg_bin / 'pg_ctl'), '-D', str(pg_data), '-m', 'fast', '-w', 'stop'], timeout=60)
                    except Exception as error:
                        cleanup_errors.append(f'PostgreSQL shutdown failed: {error}')
                if cleanup_errors:
                    raise RuntimeError('; '.join(cleanup_errors))
        except Exception as cleanup_error:
            if original_error is None:
                raise
            print(f'Fixture cleanup failed: {cleanup_error}', file=sys.stderr)


def main():
    started_at = int(os.environ['PERF_FIXTURE_STARTED_AT'])
    report = Report(started_at)
    status, failure_phase, error = 'passed', '', ''
    try:
        with tempfile.TemporaryDirectory(prefix='macos-perf-fixture-', dir=os.environ['RUNNER_TEMP']) as directory:
            exercise(Path(directory), report)
    except Exception as exc:
        status, failure_phase, error = 'failed', report.failure_phase or report.phase, str(exc)
        traceback.print_exc()
    finished_at = int(time.time())
    summary = format_summary(status, failure_phase, report.durations, started_at, finished_at,
                             int(os.environ['MACOS_JOB_STARTED_AT']), report.fingerprints, error)
    with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as output:
        output.write(summary)
    Path(os.environ['PERF_FIXTURE_SUMMARY_FILE']).touch()
    print(summary)
    return 0 if status == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
