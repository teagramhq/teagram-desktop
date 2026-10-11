#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import tempfile


workflow = Path('.github/workflows/mac.yml').read_text()
step = workflow.split('      - name: Launch app smoke test.\n', 1)[1]
step = step.split('\n      - ', 1)[0]
script = '\n'.join(line[10:] for line in step.split('        run: |\n', 1)[1].splitlines())

for expected_status in (0, 42, 143):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        executable = root / 'repo/out/Debug/Teagram.app/Contents/MacOS/Teagram'
        executable.parent.mkdir(parents=True)
        executable.write_text('#!/bin/bash\n' + (
            f'exit {expected_status}\n' if expected_status != 143 else
            'exec /bin/sleep 60\n'))
        executable.chmod(0o755)
        binaries = root / 'bin'
        binaries.mkdir()
        for name, body in {
            'arch': 'shift\nexec "$@"\n',
            'sleep': 'exec /bin/sleep 0.05\n',
        }.items():
            stub = binaries / name
            stub.write_text('#!/bin/bash\n' + body)
            stub.chmod(0o755)
        environment = dict(os.environ, REPO_NAME='repo', BUILD_VARIANT='non-store', RUNNER_TEMP=str(root),
                           GITHUB_SHA='test-sha', GITHUB_STEP_SUMMARY=str(root / 'summary'),
                           PATH=str(binaries) + ':' + os.environ['PATH'])
        result = subprocess.run(['bash', '-c', script], cwd=root, env=environment,
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == (0 if expected_status == 143 else 1), result
        evidence = root / 'repo/out/Debug/evidence/launch-smoke-status.txt'
        assert evidence.exists(), 'launch smoke discarded the child exit status'
        assert f'exit_status={expected_status}\n' in evidence.read_text(), evidence.read_text()
        assert 'tested_sha=test-sha\n' in evidence.read_text(), evidence.read_text()

validation = workflow.split('      - name: Require on-demand validation checks.\n', 1)[1]
validation = validation.split('\n      - ', 1)[0]
validation_script = '\n'.join(
    line[10:] for line in validation.split('        run: |\n', 1)[1].splitlines())
for launch_outcome in ('success', 'failure'):
    environment = dict(os.environ, FORMAT_OUTCOME='success',
                       STORE_REJECTION_OUTCOME='success', BUILD_OUTCOME='success',
                       ARTIFACT_OUTCOME='success', PROFILE_IPC_OUTCOME='success',
                       LAUNCH_OUTCOME=launch_outcome)
    result = subprocess.run(['bash', '-c', validation_script], env=environment,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == (0 if launch_outcome == 'success' else 1), result

print('macOS launch smoke early-exit and normal-stop evidence: PASS')
