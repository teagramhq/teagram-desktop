#!/usr/bin/env python3
"""Tests for runner-side macOS process activation."""

from __future__ import annotations

import subprocess
import unittest

from macos_process_activation import activate_process


class ProcessActivationTests(unittest.TestCase):
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
