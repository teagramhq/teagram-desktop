#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from io import BytesIO
from unittest.mock import patch

try:
    import desktop_scale_check as scale_check
except ModuleNotFoundError:
    scale_check = None


REPORT = """DESKTOP_SCALE_REPORT_BEGIN
| Scenario | 1x | 100x | Ratio | Absolute budget | Noise band |
| --- | ---: | ---: | ---: | --- | --- |
| messages.getDialogs page (100 dialogs) | 10.00 ms | 12.00 ms | 1.20x | 50 ms | calibrating |
| contacts.getContacts full response | 10.00 ms | 12.00 ms | 1.20x | 50 ms | calibrating |
| contact change + name lookup | 10.00 ms | 12.00 ms | 1.20x | 50 ms | calibrating |
| cache read x100 | 10.00 ms | 12.00 ms | 1.20x | 50 ms | calibrating |
Directory enumerations per thumbnail cache access: 1x=0/100, 100x=0/100.
Main-thread steps over 50 ms: dialogs 1x=0, 100x=0; contacts.getContacts 1x=0, 100x=0; contact change and lookup 1x=0, 100x=0; thumbnail reads 1x=0, 100x=0; total=0.
Scenario checks: dialogs=1/1 contacts=1/1 contact change=1/1 cache=1/1.
Outcome: PASS
DESKTOP_SCALE_REPORT_END
"""


def make_sample(one_ms=10.0, hundred_ms=12.0, stalls=0):
    scenarios = {}
    for name in scale_check.SCENARIOS:
        scenarios[name] = {
            "one_ms": one_ms,
            "hundred_ms": hundred_ms,
            "one_stalls": stalls,
            "hundred_stalls": stalls,
        }
    return {
        "schema_version": 1,
        "created_at": "2026-11-10T00:00:00Z",
        "valid": True,
        "scenario_checks_passed": True,
        "outcome": "PASS",
        "directory_enumerations": {"one": 0, "hundred": 0},
        "scenarios": scenarios,
    }


class DesktopScaleCheckTests(unittest.TestCase):
    def require_implementation(self):
        self.assertIsNotNone(
            scale_check,
            "desktop_scale_check.py must implement recorded-run evaluation",
        )

    def test_parses_per_scenario_timings_and_absolute_stalls(self):
        self.require_implementation()
        sample = scale_check.parse_report(REPORT)
        row = sample["scenarios"]["cache read x100"]
        self.assertEqual(row["one_ms"], 10.0)
        self.assertEqual(row["hundred_ms"], 12.0)
        self.assertEqual(row["one_stalls"], 0)
        self.assertEqual(sample["directory_enumerations"], {"one": 0, "hundred": 0})
        self.assertTrue(sample["valid"])

    def test_invalid_app_report_is_saved_but_fails_the_scale_step(self):
        self.require_implementation()
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.txt"
            sample = Path(directory) / "sample.json"
            report.write_text(REPORT.replace("Outcome: PASS", "Outcome: FAIL"))
            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("desktop_scale_check.py")),
                    "parse",
                    "--report",
                    str(report),
                    "--output",
                    str(sample),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertFalse(json.loads(sample.read_text())["valid"])

    def test_uses_latest_twenty_dev_runs_for_noise(self):
        self.require_implementation()
        records = [make_sample() for _ in range(21)]
        for index, record in enumerate(records):
            record["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
            record["scenarios"]["cache read x100"]["one_ms"] = float(index + 1)
        selected = scale_check.latest_records(records, 20)
        self.assertEqual(len(selected), 20)
        self.assertEqual(selected[0]["scenarios"]["cache read x100"]["one_ms"], 2.0)
        self.assertEqual(selected[-1]["scenarios"]["cache read x100"]["one_ms"], 21.0)

    def test_fetches_latest_twenty_dev_samples_and_matching_base_binary(self):
        self.require_implementation()
        samples = []
        artifacts = []
        archives = {}
        for index in range(21):
            sample = make_sample()
            sample["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
            sample["scenarios"]["cache read x100"]["one_ms"] = float(index + 1)
            sample["run_id"] = str(index + 1)
            samples.append(sample)
            artifact_id = index + 1
            artifacts.append(
                {
                    "id": artifact_id,
                    "expired": False,
                    "created_at": sample["created_at"],
                    "workflow_run": {"head_branch": "dev"},
                }
            )
            archive = BytesIO()
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr(
                    "desktop-scale-sample.json", json.dumps(sample).encode()
                )
            archives[artifact_id] = archive.getvalue()

        binary_archive = BytesIO()
        with zipfile.ZipFile(binary_archive, "w") as bundle:
            bundle.writestr("Teagram", b"dev app")
        archives[100] = binary_archive.getvalue()
        base_artifact = {
            "id": 100,
            "expired": False,
            "created_at": "2026-11-22T00:00:00Z",
            "workflow_run": {"head_branch": "dev", "head_sha": "base-sha"},
        }

        def list_artifacts(_api_url, _token, _repository, name):
            if name == "desktop-scale-dev":
                return artifacts
            self.assertEqual(name, "Teagram")
            return [base_artifact]

        with tempfile.TemporaryDirectory() as directory:
            records_path = Path(directory) / "history.json"
            binary_dir = Path(directory) / "base"
            with patch.object(
                scale_check, "_artifact_pages", side_effect=list_artifacts
            ), patch.object(
                scale_check,
                "_download_artifact",
                side_effect=lambda _api, _token, _repo, artifact_id: archives[
                    artifact_id
                ],
            ):
                records, binary_found = scale_check.fetch_baseline(
                    "https://api.github.com",
                    "token",
                    "owner/repo",
                    "base-sha",
                    records_path,
                    binary_dir,
                    require_binary=True,
                )
            self.assertEqual(len(records), 20)
            self.assertEqual(
                records[0]["scenarios"]["cache read x100"]["one_ms"], 2.0
            )
            self.assertTrue(binary_found)
            self.assertEqual((binary_dir / "Teagram").read_bytes(), b"dev app")
            self.assertTrue(binary_dir.joinpath("Teagram").stat().st_mode & 0o111)

    def test_example_below_two_milliseconds_passes_and_above_fails(self):
        self.require_implementation()
        records = []
        for index in range(20):
            value = 9.6 if index < 10 else 10.4
            sample = make_sample(one_ms=value, hundred_ms=11.0)
            sample["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
            records.append(sample)
        base = [make_sample(one_ms=9.8), make_sample(one_ms=10.2)]
        below = [make_sample(one_ms=11.7), make_sample(one_ms=12.1)]
        above = [make_sample(one_ms=11.9), make_sample(one_ms=12.3)]

        passes = scale_check.evaluate(records, base, below)
        fails = scale_check.evaluate(records, base, above)

        self.assertAlmostEqual(passes["rows"]["cache read x100"]["noise_one_ms"], 0.4)
        self.assertFalse(passes["failures"])
        self.assertTrue(any("cache read x100" in failure for failure in fails["failures"]))

    def test_nineteen_dev_runs_remain_report_only_even_for_timing_failures(self):
        self.require_implementation()
        records = [make_sample() for _ in range(19)]
        base = [make_sample()]
        head = [make_sample(one_ms=20.0, hundred_ms=40.0, stalls=1)]

        result = scale_check.evaluate(records, base, head)
        summary = scale_check.format_summary(result, head[-1])

        self.assertEqual(result["calibrating"], 19)
        self.assertFalse(result["failures"])
        self.assertIn("calibrating: 19/20", summary)

    def test_absolute_budget_and_scale_ratio_fail_after_calibration(self):
        self.require_implementation()
        records = [make_sample() for _ in range(20)]
        base = [make_sample(), make_sample()]

        absolute = scale_check.evaluate(
            records, base, [make_sample(stalls=1), make_sample()]
        )
        ratio = scale_check.evaluate(
            records,
            base,
            [make_sample(hundred_ms=16.0), make_sample(hundred_ms=16.0)],
        )

        self.assertTrue(any("50 ms" in failure for failure in absolute["failures"]))
        self.assertTrue(any("1.5x" in failure for failure in ratio["failures"]))

    def test_dev_baseline_exceptions_are_named_and_report_only(self):
        self.require_implementation()
        records = [make_sample() for _ in range(20)]
        for record in records:
            record["scenarios"]["cache read x100"]["one_stalls"] = 1
        head = [make_sample(stalls=1), make_sample(stalls=1)]

        result = scale_check.evaluate(records, head, head)
        summary = scale_check.format_summary(result, head[-1])

        self.assertIn("cache read x100", result["exceptions"])
        self.assertFalse(any("cache read x100" in item for item in result["failures"]))
        self.assertIn("Additional dev-baseline exceptions", summary)
        self.assertIn("::warning::", "\n".join(scale_check.format_annotations(result)))

    def test_static_baseline_exceptions_are_report_only(self):
        self.require_implementation()
        records = [make_sample() for _ in range(20)]
        base = [make_sample(), make_sample()]
        head = [make_sample(), make_sample()]
        baseline_exceptions = (
            "messages.getDialogs page (100 dialogs)",
            "contacts.getContacts full response",
            "contact change + name lookup",
        )
        for record in records:
            for name in baseline_exceptions:
                record["scenarios"][name]["one_stalls"] = 1
        for sample in head:
            for name in baseline_exceptions:
                sample["scenarios"][name].update(
                    {"one_ms": 30.0, "hundred_ms": 60.0, "one_stalls": 1, "hundred_stalls": 1}
                )

        result = scale_check.evaluate(records, base, head)

        self.assertIn("MAIN-3656", result["exceptions"]["messages.getDialogs page (100 dialogs)"])
        self.assertIn("MAIN-3657", result["exceptions"]["contacts.getContacts full response"])
        self.assertIn("MAIN-3657", result["exceptions"]["contact change + name lookup"])
        self.assertFalse(result["failures"])

    def test_known_baseline_exception_lifts_when_dev_timings_are_clear(self):
        self.require_implementation()
        records = [make_sample() for _ in range(20)]
        base = [make_sample(), make_sample()]
        head = [make_sample(), make_sample()]
        for sample in head:
            sample["scenarios"]["messages.getDialogs page (100 dialogs)"][
                "one_stalls"
            ] = 1

        result = scale_check.evaluate(records, base, head)

        scenario = "messages.getDialogs page (100 dialogs)"
        self.assertNotIn(scenario, result["exceptions"])
        self.assertTrue(any(scenario in failure for failure in result["failures"]))

    def test_workflow_repeats_interleaved_samples_once_with_a_five_minute_cap(self):
        workflow = Path(__file__).parents[1] / "workflows/linux.yml"
        text = workflow.read_text()
        self.assertIn("for pair in 1 2", text)
        self.assertIn("for pair in 3 4", text)
        self.assertIn("scale_budget_seconds=200", text)
        self.assertIn("timeout 90s python3 .github/scripts/desktop_scale_check.py", text)
        self.assertIn("repeating the interleaved samples once", text)
        self.assertIn("name: desktop-scale-dev", text)
        self.assertIn("retention-days: 90", text)


if __name__ == "__main__":
    unittest.main()
