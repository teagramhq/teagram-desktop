#!/usr/bin/env python3
"""Activate a macOS GUI process from the runner and verify it is frontmost."""

from __future__ import annotations

import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


@dataclass(frozen=True)
class ActivationResult:
    ok: bool
    attempts: int
    frontmost_pid: int | None
    diagnostics: str
    error: str = ""


def read_activation_request(request_path: str | Path) -> tuple[int, int] | None:
    """Read the app-published PID and request number, independent of stderr."""
    try:
        lines = Path(request_path).read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError):
        return None
    if (
        len(lines) != 2
        or not lines[0].startswith("pid=")
        or not lines[1].startswith("request=")
    ):
        return None
    try:
        pid = int(lines[0][len("pid=") :])
        request_number = int(lines[1][len("request=") :])
    except ValueError:
        return None
    return (pid, request_number) if pid > 0 and request_number > 0 else None


def activation_request_is_new(
    request: tuple[int, int] | None, last_request_number: int
) -> bool:
    return request is not None and request[1] > last_request_number


def _output_text(value: str | bytes | None) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def _command_output(completed: subprocess.CompletedProcess[str]) -> str:
    return "\n".join(
        text
        for text in (_output_text(completed.stdout), _output_text(completed.stderr))
        if text
    )


def activate_process(
    pid: int,
    attempts: int = 10,
    interval_seconds: float = 1.0,
    *,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    sleeper: Callable[[float], None] = time.sleep,
) -> ActivationResult:
    """Use System Events outside the target process and verify the frontmost PID."""
    if pid <= 0:
        return ActivationResult(False, 0, None, "", "invalid process ID")
    if attempts <= 0:
        return ActivationResult(False, 0, None, "", "attempt count must be positive")

    activate_script = (
        'tell application "System Events" to set frontmost of '
        f"(first application process whose unix id is {pid}) to true"
    )
    frontmost_script = (
        'tell application "System Events" to get unix id of '
        "first application process whose frontmost is true"
    )
    diagnostics = []
    frontmost_pid = None

    for attempt in range(1, attempts + 1):
        try:
            activated = runner(
                ["osascript", "-e", activate_script],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            activation_output = _command_output(activated)
            diagnostics.append(
                f"attempt={attempt} activation_status={activated.returncode}"
                + (f" output={activation_output}" if activation_output else "")
            )
        except subprocess.TimeoutExpired as error:
            activation_output = "\n".join(
                text
                for text in (_output_text(error.stdout), _output_text(error.stderr))
                if text
            )
            diagnostics.append(
                f"attempt={attempt} activation_error=osascript timed out"
                + (f" output={activation_output}" if activation_output else "")
            )
        except OSError as error:
            diagnostics.append(
                f"attempt={attempt} activation_error=could not run osascript: {error}"
            )

        try:
            queried = runner(
                ["osascript", "-e", frontmost_script],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            query_output = _command_output(queried)
            frontmost_pid = None
            if queried.returncode == 0:
                try:
                    frontmost_pid = int(_output_text(queried.stdout).strip())
                except ValueError:
                    frontmost_pid = None
            diagnostics.append(
                f"attempt={attempt} query_status={queried.returncode} "
                f"frontmost_pid={frontmost_pid or 'unavailable'}"
                + (f" output={query_output}" if query_output else "")
            )
        except subprocess.TimeoutExpired as error:
            query_output = "\n".join(
                text
                for text in (_output_text(error.stdout), _output_text(error.stderr))
                if text
            )
            frontmost_pid = None
            diagnostics.append(
                f"attempt={attempt} query_error=osascript timed out"
                + (f" output={query_output}" if query_output else "")
            )
        except OSError as error:
            frontmost_pid = None
            diagnostics.append(
                f"attempt={attempt} query_error=could not run osascript: {error}"
            )

        if frontmost_pid == pid:
            return ActivationResult(
                True, attempt, frontmost_pid, "\n".join(diagnostics)
            )
        if attempt < attempts:
            sleeper(interval_seconds)

    return ActivationResult(
        False,
        attempts,
        frontmost_pid,
        "\n".join(diagnostics),
        f"process {pid} did not become frontmost",
    )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: macos_process_activation.py <pid>", file=sys.stderr)
        return 2
    try:
        pid = int(sys.argv[1])
    except ValueError:
        print(f"invalid process ID: {sys.argv[1]}", file=sys.stderr)
        return 2

    result = activate_process(pid)
    if result.diagnostics:
        print(result.diagnostics)
    if not result.ok:
        print(
            f"runner_external_activation=FAIL pid={pid} "
            f"attempts={result.attempts} error={result.error}",
            file=sys.stderr,
        )
        return 1
    print(
        f"runner_external_activation=PASS pid={pid} "
        f"frontmost_pid={result.frontmost_pid} attempts={result.attempts}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
