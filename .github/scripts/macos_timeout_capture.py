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
        output = "\n".join(
            value for value in (error.stdout, error.stderr) if isinstance(value, str)
        )
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

    command_output = "\n".join(
        value for value in (completed.stdout, completed.stderr) if value
    )
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
