#!/usr/bin/env python3

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

import macos_perf_fixture as fixture


class FixtureReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.release = self.root / 'release'
        self.release.mkdir()
        self.version = 'a' * 64
        self.files = {
            'perf_1x.dump': b'1x database',
            'perf_100x.dump': b'100x database',
            'blobs-1x__01__1': b'1x blob',
            'blobs-100x__02__2': b'100x blob',
        }
        self.manifest = {
            'fixture': self.version,
            'server_commit': 'b' * 40,
            'migration_revision': '20261026000006',
            'assets': [
                {'name': name, 'sha256': hashlib.sha256(data).hexdigest()}
                for name, data in self.files.items()
            ],
        }
        for name, data in self.files.items():
            (self.release / name).write_bytes(data)
        self.write_manifest()

    def write_manifest(self):
        path = self.release / 'manifest.json'
        path.write_text(json.dumps(self.manifest))
        self.manifest_sha256 = hashlib.sha256(path.read_bytes()).hexdigest()

    def download(self):
        return fixture.download_fixture(self.release.as_uri(), self.root / 'snapshot', self.version,
                                        self.manifest_sha256)

    def test_download_verifies_every_asset_and_restores_blob_paths(self):
        manifest = self.download()
        self.assertEqual(manifest['fixture'], self.version)
        for name, data in self.files.items():
            self.assertEqual((self.root / 'snapshot' / name.replace('__', '/')).read_bytes(), data)

    def test_checksum_mismatch_fails_the_fixture(self):
        (self.release / 'blobs-100x__02__2').write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch.*blobs-100x__02__2'):
            self.download()

    def test_manifest_must_match_the_pinned_digest(self):
        self.manifest['server_commit'] = 'c' * 40
        (self.release / 'manifest.json').write_text(json.dumps(self.manifest))
        with self.assertRaisesRegex(ValueError, 'manifest checksum mismatch'):
            self.download()

    def test_rejects_unsafe_or_duplicate_asset_paths(self):
        for name in ('../escape', 'blobs-1x__..__escape', '/absolute'):
            with self.subTest(name=name):
                self.manifest['assets'][0]['name'] = name
                self.write_manifest()
                with self.assertRaisesRegex(ValueError, 'unsafe'):
                    self.download()
        self.manifest['assets'][0]['name'] = self.manifest['assets'][1]['name']
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            self.download()

    def test_migration_history_must_match_fixture_and_runtime(self):
        seed_sum = 'h1:top\n20261025000001_first.sql h1:first\n20261026000006_last.sql h1:last\n'
        runtime_sum = seed_sum + '20261101000001_next.sql h1:next\n'
        fixture.validate_migration_history(self.manifest, seed_sum, runtime_sum)
        with self.assertRaisesRegex(ValueError, 'migration history'):
            fixture.validate_migration_history(self.manifest, seed_sum, runtime_sum.replace('h1:last', 'h1:changed'))
        with self.assertRaisesRegex(ValueError, 'migration revision'):
            fixture.validate_migration_history(self.manifest, seed_sum, runtime_sum.replace('20261026000006', '20261026000007'))

    def test_restored_database_revision_must_match_the_manifest(self):
        fixture.validate_restored_revision('1x', '20261026000006', '20261026000006')
        with self.assertRaisesRegex(ValueError, '100x database migration revision 20261026000005 does not match manifest 20261026000006'):
            fixture.validate_restored_revision('100x', '20261026000005', '20261026000006')

    def test_failure_summary_retains_the_phase_and_elapsed_time(self):
        text = fixture.format_summary(
            'failed', 'restore 100x', {'download': 4.0, 'restore 1x': 2.0},
            started_at=100, finished_at=112, job_started_at=50, fingerprints={},
        )
        self.assertIn('failed', text)
        self.assertIn('restore 100x', text)
        self.assertIn('| download | 4.0 s |', text)
        self.assertIn('| Total added to job | 12 s |', text)
        self.assertIn('job elapsed: 62 s', text)
        self.assertIn('| migrations | not reached |', text)

    def test_failed_tool_setup_reports_failure_without_a_database(self):
        summary = self.root / 'summary.md'
        env = dict(os.environ, RUNNER_TEMP=str(self.root), PERF_FIXTURE_STARTED_AT='100',
                   MACOS_JOB_STARTED_AT='50', PERF_GO_OUTCOME='failure', PERF_ATLAS_OUTCOME='skipped',
                   PERF_FIXTURE_PHASE_FILE=str(self.root / 'phase'),
                   PERF_FIXTURE_TIMES_FILE=str(self.root / 'times'),
                   PERF_FIXTURE_SUMMARY_FILE=str(self.root / 'summary-written'),
                   GITHUB_STEP_SUMMARY=str(summary))
        result = subprocess.run([sys.executable, fixture.__file__], env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Failed phase: **tool setup**', summary.read_text())
        self.assertIn('Go setup failed', summary.read_text())
        self.assertTrue((self.root / 'summary-written').exists())

    def test_checksum_failure_is_reported_as_download_after_cleanup(self):
        summary = self.root / 'summary.md'
        env = dict(os.environ, RUNNER_TEMP=str(self.root), PERF_FIXTURE_STARTED_AT='100',
                   MACOS_JOB_STARTED_AT='50', PERF_GO_OUTCOME='success', PERF_ATLAS_OUTCOME='success',
                   PERF_FIXTURE_PHASE_FILE=str(self.root / 'phase'),
                   PERF_FIXTURE_TIMES_FILE=str(self.root / 'times'),
                   PERF_FIXTURE_SUMMARY_FILE=str(self.root / 'summary-written'),
                   GITHUB_STEP_SUMMARY=str(summary))
        with patch.dict(os.environ, env), patch.object(fixture, 'download_fixture',
                                                       side_effect=ValueError('checksum mismatch for test')), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(fixture.main(), 1)
        self.assertIn('Failed phase: **download**', summary.read_text())
        self.assertIn('checksum mismatch for test', summary.read_text())
        self.assertIn('| cleanup |', summary.read_text())

    def test_setup_failure_preserves_postgres_diagnostics(self):
        (self.root / 'postgres.log').write_text('temporary postgres failure detail')
        env = dict(os.environ, PERF_GO_OUTCOME='failure', PERF_ATLAS_OUTCOME='skipped',
                   PERF_FIXTURE_PHASE_FILE=str(self.root / 'phase'),
                   PERF_FIXTURE_TIMES_FILE=str(self.root / 'times'))
        output = io.StringIO()
        with patch.dict(os.environ, env), contextlib.redirect_stderr(output):
            report = fixture.Report(time.time())
            with self.assertRaisesRegex(RuntimeError, 'Go setup failed'):
                fixture.exercise(self.root, report)
        self.assertIn('temporary postgres failure detail', output.getvalue())
        output = io.StringIO()
        with patch.dict(os.environ, env), patch.object(Path, 'read_text', side_effect=OSError('log unreadable')), \
                contextlib.redirect_stderr(output):
            report = fixture.Report(time.time())
            with self.assertRaisesRegex(RuntimeError, 'Go setup failed'):
                fixture.exercise(self.root, report)
        self.assertIn('Could not read PostgreSQL log: log unreadable', output.getvalue())

    def test_fixture_run_reports_success_and_preserves_failures(self):
        migration_sum = 'h1:top\n20261026000006_last.sql h1:last\n'

        def checkout(directory):
            migrations = directory / 'migrations'
            migrations.mkdir(parents=True)
            (migrations / 'atlas.sum').write_text(migration_sum)
            helper = directory / 'internal/pgtest/pgtest.go'
            helper.parent.mkdir(parents=True)
            helper.write_text('func EncKey() []byte { return bytes.Repeat([]byte{0x2a}, 32) }')

        for (name, restore_fails, outer_error, server_stop_fails, interrupted, stop_fails,
             revision_mismatch, migrate_fails) in (
                ('clean-stop', False, False, False, None, False, False, False),
                ('restore', True, False, False, None, True, False, False),
                ('cleanup', False, False, False, None, True, False, False),
                ('cleanup-in-except', False, True, False, None, True, False, False),
                ('server-stop', False, False, True, None, True, False, False),
                ('wrong-revision', False, False, False, None, False, True, False),
                ('migration-fails', False, False, False, None, False, False, True),
                ('interrupt-restore', False, False, False, 'restore 1x', False, False, False),
                ('interrupt-restore-stop-failure', False, False, False, 'restore 1x', True, False, False),
                ('interrupt-migrations', False, False, False, 'migrations', False, False, False),
                ('interrupt-migrations-stop-failure', False, False, False, 'migrations', True, False, False)):
            with self.subTest(name=name):
                case = self.root / name
                case.mkdir()
                summary = case / 'summary.md'
                env = dict(os.environ, RUNNER_TEMP=str(case), PERF_FIXTURE_STARTED_AT='100',
                           MACOS_JOB_STARTED_AT='50', PERF_GO_OUTCOME='success', PERF_ATLAS_OUTCOME='success',
                           PERF_FIXTURE_PHASE_FILE=str(case / 'phase'),
                           PERF_FIXTURE_TIMES_FILE=str(case / 'times'),
                           PERF_FIXTURE_SUMMARY_FILE=str(case / 'summary-written'),
                           GITHUB_STEP_SUMMARY=str(summary))
                stopped = []
                migration_calls = []

                def run(command, **kwargs):
                    name = Path(command[0]).name
                    if name == 'pg_ctl' and command[-1] == 'start':
                        data = Path(command[command.index('-D') + 1])
                        data.mkdir(parents=True)
                        (data / 'postmaster.pid').touch()
                    if name == 'pg_ctl' and command[-1] == 'stop':
                        stopped.append(True)
                        if stop_fails:
                            raise subprocess.CalledProcessError(7, command)
                    if name == 'pg_restore' and interrupted == 'restore 1x':
                        signal.raise_signal(signal.SIGINT)
                    if name == 'pg_restore' and restore_fails:
                        raise RuntimeError('restore failed first')
                    revision = ('20261026000005' if revision_mismatch and '-XAt' in command
                                else self.manifest['migration_revision'])
                    output = revision if '-XAt' in command else ''
                    return subprocess.CompletedProcess(command, 0, stdout=output)

                error_output = io.StringIO()
                process = Mock()
                process.poll.return_value = None if server_stop_fails else 0
                if server_stop_fails:
                    process.terminate.side_effect = OSError('server stop failed')

                def migrate(*args, **kwargs):
                    migration_calls.append(args[0][0])
                    if interrupted == 'migrations':
                        signal.raise_signal(signal.SIGINT)
                    if migrate_fails:
                        raise subprocess.CalledProcessError(1, args[0])
                    return subprocess.CompletedProcess(args[0], 0)

                with patch.dict(os.environ, env), \
                        patch.object(fixture, 'download_fixture', return_value=self.manifest), \
                        patch.object(fixture, 'checkout_server', side_effect=checkout), \
                        patch.object(fixture, 'urlopen', side_effect=lambda *args, **kwargs: io.BytesIO(migration_sum.encode())), \
                        patch.object(fixture, 'postgres_binaries', return_value=case / 'pg-bin'), \
                        patch.object(fixture, 'available_port', side_effect=range(31000, 31010)), \
                        patch.object(fixture, 'run', side_effect=run), \
                        patch.object(fixture.subprocess, 'run', side_effect=migrate), \
                        patch.object(fixture.subprocess, 'Popen', return_value=process), \
                        patch.object(fixture, 'wait_for_server', return_value='123'), \
                        contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(error_output):
                    if interrupted:
                        with self.assertRaises(KeyboardInterrupt):
                            fixture.main()
                    elif outer_error:
                        try:
                            raise ValueError('unrelated caller error')
                        except ValueError:
                            result = fixture.main()
                    else:
                        result = fixture.main()
                    if not interrupted:
                        self.assertEqual(result, 0 if name == 'clean-stop' else 1)
                if interrupted:
                    self.assertEqual((case / 'phase').read_text(), interrupted)
                    self.assertIn('cleanup|', (case / 'times').read_text())
                    self.assertFalse((case / 'summary-written').exists())
                    self.assertEqual(stopped, [True])
                    if stop_fails:
                        self.assertIn('pg_ctl', error_output.getvalue())
                    else:
                        self.assertNotIn('Fixture cleanup failed:', error_output.getvalue())
                    continue
                text = summary.read_text()
                if name == 'clean-stop':
                    self.assertIn('Status: **passed**', text)
                    self.assertIn('- 1x: TCP ready on loopback; server RSA key fingerprint `123`', text)
                    self.assertIn('- 100x: TCP ready on loopback; server RSA key fingerprint `123`', text)
                    self.assertEqual(stopped, [True])
                    self.assertEqual(migration_calls, ['atlas', 'atlas'])
                elif revision_mismatch:
                    self.assertIn(
                        'Failed phase: **restore 1x**. 1x database migration revision 20261026000005 '
                        'does not match manifest 20261026000006', text,
                    )
                    self.assertEqual(migration_calls, [])
                    self.assertEqual(stopped, [True])
                elif migrate_fails:
                    self.assertIn('Failed phase: **migrations**.', text)
                    self.assertIn('returned non-zero exit status 1', text)
                    self.assertEqual(migration_calls, ['atlas'])
                    self.assertEqual(stopped, [True])
                elif restore_fails:
                    self.assertIn('Failed phase: **restore 1x**. restore failed first', text)
                    self.assertNotIn('pg_ctl', text)
                    self.assertIn('pg_ctl', error_output.getvalue())
                else:
                    self.assertIn('Failed phase: **cleanup**.', text)
                    self.assertIn('pg_ctl', text)
                    if server_stop_fails:
                        self.assertIn('server shutdown failed: server stop failed', text)

    def test_server_readiness_requires_a_fingerprint_and_a_live_listener(self):
        class Running:
            def poll(self):
                return None

        log = self.root / 'server.log'
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            port = listener.getsockname()[1]
            log.write_text(f'msg="server RSA key" fingerprint=-123\nmsg=listening addr=127.0.0.1:{port}\n')
            self.assertEqual(fixture.wait_for_server(Running(), log, port, timeout=1), '-123')
            for missing in ('fingerprint', 'listening'):
                with self.subTest(missing=missing):
                    line = (f'msg=listening addr=127.0.0.1:{port}\n' if missing == 'fingerprint'
                            else 'msg="server RSA key" fingerprint=-123\n')
                    log.write_text(line)
                    with self.assertRaisesRegex(RuntimeError, 'did not become ready'):
                        fixture.wait_for_server(Running(), log, port, timeout=0.01)
            log.write_text(f'msg="server RSA key" fingerprint=-123\nmsg=listening addr=127.0.0.1:{port}\n')
        with self.assertRaisesRegex(RuntimeError, 'did not become ready'):
            fixture.wait_for_server(Running(), log, port, timeout=0.01)


if __name__ == '__main__':
    unittest.main()
