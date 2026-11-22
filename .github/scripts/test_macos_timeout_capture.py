#!/usr/bin/env python3
"""Tests for bounded macOS hang-sample capture."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

from macos_timeout_capture import (
    authorization_timeout_report,
    capture_hang_sample,
)


class TimeoutCaptureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.sample_path = Path(self.temp_dir.name) / "hang-sample.txt"

    def test_success_returns_captured_stack_and_command_output(self) -> None:
        calls: list[tuple[list[str], dict[str, object]]] = []

        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            calls.append((command, kwargs))
            self.sample_path.write_text("Thread 0 stack\nframe 0\n", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, "sample complete", "")

        result = capture_hang_sample(123, self.sample_path, 12, runner=runner)

        self.assertTrue(result.ok)
        self.assertEqual(result.sample_text, "Thread 0 stack\nframe 0\n")
        self.assertIn("sample complete", result.command_output)
        self.assertEqual(
            calls[0][0], ["sample", "123", "1", "5", "-file", str(self.sample_path)]
        )
        self.assertEqual(calls[0][1]["timeout"], 12)

    def test_nonzero_exit_is_capture_failure(self) -> None:
        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(command, 1, "", "sample denied")

        result = capture_hang_sample(123, self.sample_path, 12, runner=runner)

        self.assertFalse(result.ok)
        self.assertIn("status 1", result.error)
        self.assertIn("sample denied", result.command_output)

    def test_timeout_is_capture_failure(self) -> None:
        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            raise subprocess.TimeoutExpired(command, kwargs["timeout"])

        result = capture_hang_sample(123, self.sample_path, 12, runner=runner)

        self.assertFalse(result.ok)
        self.assertIn("timed out after 12 seconds", result.error)

    def test_timeout_bytes_are_preserved_in_command_output(self) -> None:
        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            raise subprocess.TimeoutExpired(
                command,
                kwargs["timeout"],
                output=b"partial stdout",
                stderr=b"partial stderr",
            )

        result = capture_hang_sample(123, self.sample_path, 12, runner=runner)

        self.assertFalse(result.ok)
        self.assertEqual(
            result.command_output, "partial stdout\npartial stderr"
        )

    def test_authorization_timeout_report_formats_capture_failure(self) -> None:
        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            raise subprocess.TimeoutExpired(
                command,
                kwargs["timeout"],
                output=b"partial sample output",
                stderr=b"sample timed out",
            )

        report = authorization_timeout_report(
            123, "runtime", self.sample_path, 12, runner=runner
        )

        self.assertFalse(report.stack_capture_ok)
        self.assertEqual(
            report.command_output, "partial sample output\nsample timed out"
        )
        self.assertIn("HARNESS ERROR: authorization timeout stack capture failed", report.log_output)
        self.assertIn(
            "authorization_timeout=FAIL phase=runtime stack_capture=FAIL",
            report.console_output,
        )
        self.assertIn("reason=sample timed out after 12 seconds", report.console_output)

    def test_authorization_timeout_report_handles_missing_pid(self) -> None:
        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            self.fail("sample must not run without a process ID")

        report = authorization_timeout_report(
            None, "startup", self.sample_path, runner=runner
        )

        self.assertFalse(report.stack_capture_ok)
        self.assertIn("failed because the Teagram PID was not found", report.log_output)
        self.assertIn("reason=missing_pid", report.console_output)

    def test_authorization_timeout_report_limits_success_excerpt_to_80_lines(self) -> None:
        sample_text = "\n".join(f"frame {index}" for index in range(85))

        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            self.sample_path.write_text(sample_text, encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, "sample complete", "")

        report = authorization_timeout_report(
            123, "runtime", self.sample_path, runner=runner
        )

        self.assertTrue(report.stack_capture_ok)
        self.assertIn("Authorization timeout stack capture: PASS", report.log_output)
        self.assertIn("frame 79", report.console_output)
        self.assertNotIn("frame 80", report.console_output)

    def test_empty_sample_file_is_capture_failure(self) -> None:
        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            self.sample_path.write_text("  \n", encoding="utf-8")
            return subprocess.CompletedProcess(command, 0, "sample complete", "")

        result = capture_hang_sample(123, self.sample_path, 12, runner=runner)

        self.assertFalse(result.ok)
        self.assertIn("empty", result.error)

    def test_old_sample_file_cannot_satisfy_a_new_capture(self) -> None:
        self.sample_path.write_text("stale stack\n", encoding="utf-8")

        def runner(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(command, 0, "sample complete", "")

        result = capture_hang_sample(123, self.sample_path, 12, runner=runner)

        self.assertFalse(result.ok)
        self.assertIn("did not create", result.error)


if __name__ == "__main__":
    unittest.main()
