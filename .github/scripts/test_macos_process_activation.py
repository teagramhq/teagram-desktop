#!/usr/bin/env python3
"""Tests for runner-side macOS process activation."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

from macos_process_activation import (
    activate_process,
    activation_request_is_new,
    read_activation_request,
    run_with_runtime_deadline,
)


class ProcessActivationTests(unittest.TestCase):
    def test_app_published_pid_is_available_without_stderr_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            request_path = Path(directory) / "runner-external-activation.request"
            stderr_path = Path(directory) / "stderr.log"
            request_path.write_text("pid=4321\nrequest=1\n", encoding="utf-8")

            self.assertFalse(stderr_path.exists())
            self.assertEqual(read_activation_request(request_path), (4321, 1))
            request_path.write_text("pid=4321\nrequest=2\n", encoding="utf-8")
            self.assertEqual(read_activation_request(request_path), (4321, 2))

    def test_missing_or_incomplete_activation_request_is_not_a_pid(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            request_path = Path(directory) / "runner-external-activation.request"

            self.assertIsNone(read_activation_request(request_path))
            request_path.write_text("pid=", encoding="utf-8")
            self.assertIsNone(read_activation_request(request_path))
            request_path.write_text("pid=0\nrequest=1\n", encoding="utf-8")
            self.assertIsNone(read_activation_request(request_path))
            request_path.write_text("pid=4321\nrequest=0\n", encoding="utf-8")
            self.assertIsNone(read_activation_request(request_path))

    def test_new_activation_request_advances_the_handshake(self) -> None:
        self.assertFalse(activation_request_is_new(None, 0))
        self.assertTrue(activation_request_is_new((4321, 1), 0))
        self.assertFalse(activation_request_is_new((4321, 1), 1))
        self.assertTrue(activation_request_is_new((4321, 2), 1))

    def test_stalled_activation_refresh_cannot_outlive_runtime_deadline(self) -> None:
        deadline = 180.0
        now = 179.75
        observed_timeouts: list[float] = []

        def stalled_runner(
            command: list[str], **kwargs: object
        ) -> subprocess.CompletedProcess[str]:
            timeout = float(kwargs["timeout"])
            observed_timeouts.append(timeout)
            raise subprocess.TimeoutExpired(command, timeout)

        with self.assertRaises(subprocess.TimeoutExpired):
            run_with_runtime_deadline(
                ["activation-helper"],
                runtime_deadline=deadline,
                timeout=220,
                runner=stalled_runner,
                clock=lambda: now,
                capture_output=True,
                text=True,
            )

        self.assertEqual(observed_timeouts, [deadline - now])

    def test_retries_until_the_target_process_is_frontmost(self) -> None:
        commands: list[list[str]] = []
        responses = iter(
            [
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "9876\n", ""),
                subprocess.CompletedProcess([], 0, "", ""),
                subprocess.CompletedProcess([], 0, "4321\n", ""),
            ]
        )
        delays: list[float] = []

        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            return next(responses)

        result = activate_process(
            4321, attempts=3, interval_seconds=0.5, runner=runner, sleeper=delays.append
        )

        self.assertTrue(result.ok)
        self.assertEqual(result.attempts, 2)
        self.assertEqual(result.frontmost_pid, 4321)
        self.assertEqual(delays, [0.5])
        self.assertEqual(
            commands[0],
            [
                "osascript",
                "-e",
                'tell application "System Events" to set frontmost of '
                "(first application process whose unix id is 4321) to true",
            ],
        )
        self.assertEqual(
            commands[1],
            [
                "osascript",
                "-e",
                'tell application "System Events" to get unix id of '
                "first application process whose frontmost is true",
            ],
        )

    def test_activation_refusal_is_reported_after_bounded_retries(self) -> None:
        commands: list[list[str]] = []
        responses = iter(
            [
                subprocess.CompletedProcess([], 1, "", "activation denied"),
                subprocess.CompletedProcess([], 0, "9876\n", ""),
                subprocess.CompletedProcess([], 1, "", "activation denied"),
                subprocess.CompletedProcess([], 0, "9876\n", ""),
            ]
        )

        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            commands.append(command)
            return next(responses)

        result = activate_process(
            4321, attempts=2, interval_seconds=0, runner=runner, sleeper=lambda _: None
        )

        self.assertFalse(result.ok)
        self.assertEqual(result.attempts, 2)
        self.assertEqual(result.frontmost_pid, 9876)
        self.assertIn("activation denied", result.diagnostics)
        self.assertIn("frontmost_pid=9876", result.diagnostics)
        self.assertEqual(len(commands), 4)

    def test_invalid_pid_does_not_run_osascript(self) -> None:
        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            self.fail("osascript must not run without a valid process ID")

        result = activate_process(0, runner=runner)

        self.assertFalse(result.ok)
        self.assertEqual(result.attempts, 0)
        self.assertIn("invalid process ID", result.error)


if __name__ == "__main__":
    unittest.main()
