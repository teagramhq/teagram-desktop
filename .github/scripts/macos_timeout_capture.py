"""Capture and validate a macOS process sample used for timeout diagnostics."""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


@dataclass(frozen=True)
class CaptureResult:
    ok: bool
    sample_path: Path
    sample_text: str
    command_output: str
    error: str = ""


@dataclass(frozen=True)
class AuthorizationTimeoutReport:
    stack_capture_ok: bool
    command_output: str
    log_output: str
    console_output: str


def _output_text(value: str | bytes | None) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def _combine_output(*values: str | bytes | None) -> str:
    return "\n".join(text for value in values if (text := _output_text(value)))


def capture_hang_sample(
    pid: int,
    sample_path: Path,
    timeout_seconds: int = 12,
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> CaptureResult:
    """Run `sample` with a bound and require a non-empty stack capture."""
    command = ["sample", str(pid), "1", "5", "-file", str(sample_path)]
    try:
        sample_path.unlink(missing_ok=True)
    except OSError as error:
        return CaptureResult(
            False,
            sample_path,
            "",
            "",
            f"could not clear previous sample output: {error}",
        )
    try:
        completed = runner(
            command,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        output = _combine_output(error.stdout, error.stderr)
        return CaptureResult(
            False,
            sample_path,
            "",
            output,
            f"sample timed out after {timeout_seconds} seconds",
        )
    except OSError as error:
        return CaptureResult(
            False, sample_path, "", "", f"could not run sample: {error}"
        )

    command_output = _combine_output(completed.stdout, completed.stderr)
    if completed.returncode != 0:
        return CaptureResult(
            False,
            sample_path,
            "",
            command_output,
            f"sample exited with status {completed.returncode}",
        )
    try:
        sample_text = sample_path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return CaptureResult(
            False,
            sample_path,
            "",
            command_output,
            f"sample did not create its output file: {error}",
        )
    if not sample_text.strip():
        return CaptureResult(
            False,
            sample_path,
            sample_text,
            command_output,
            "sample output file is empty",
        )
    return CaptureResult(True, sample_path, sample_text, command_output)


def authorization_timeout_report(
    pid: int | None,
    timeout_phase: str,
    sample_path: Path,
    timeout_seconds: int = 12,
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> AuthorizationTimeoutReport:
    """Capture and format diagnostics for an authorization process timeout."""
    if pid is None:
        error = (
            "HARNESS ERROR: authorization timeout stack capture "
            "failed because the Teagram PID was not found"
        )
        return AuthorizationTimeoutReport(
            False,
            "",
            error,
            "::error::"
            + error
            + "\nauthorization_timeout=FAIL "
            + f"phase={timeout_phase} stack_capture=FAIL reason=missing_pid",
        )

    capture = capture_hang_sample(
        pid, sample_path, timeout_seconds=timeout_seconds, runner=runner
    )
    if capture.ok:
        excerpt = "\n".join(capture.sample_text.splitlines()[:80])
        return AuthorizationTimeoutReport(
            True,
            capture.command_output,
            "Authorization timeout stack capture: PASS; "
            f"sample: {sample_path}\nHang sample excerpt:\n{excerpt}",
            "authorization_timeout=FAIL "
            + f"phase={timeout_phase} stack_capture=PASS sample={sample_path}\n"
            + "Hang sample excerpt:\n"
            + excerpt,
        )

    error = (
        "HARNESS ERROR: authorization timeout stack capture failed: "
        f"{capture.error}"
    )
    return AuthorizationTimeoutReport(
        False,
        capture.command_output,
        f"{error}; sample: {sample_path}",
        "::error::"
        + error
        + "\nauthorization_timeout=FAIL "
        + f"phase={timeout_phase} stack_capture=FAIL reason={capture.error}",
    )
