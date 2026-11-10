#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zipfile
from contextlib import redirect_stderr, redirect_stdout
from io import BytesIO, StringIO
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


def make_runner_harness(root, history=None):
    repo_root = Path(__file__).parents[2]
    runner_temp = root / "runner-temp"
    runner_temp.mkdir()
    history_file = root / "history.json"
    history_file.write_text(json.dumps(history or []))

    bin_dir = root / "bin"
    bin_dir.mkdir()
    python_stub = bin_dir / "python3"
    python_stub.write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env bash
            set -euo pipefail

            if [[ "${1:-}" == ".github/scripts/desktop_scale_check.py" ]]; then
              case "${2:-}" in
                fetch)
                  printf 'fetch\\n' >> "$FETCH_CALL_LOG"
                  status="${MOCK_FETCH_STATUS:-0}"
                  if [[ "$status" != 0 ]]; then exit "$status"; fi
                  shift 2
                  output_path=
                  while (($#)); do
                    if [[ "$1" == --records-output ]]; then
                      output_path="$2"
                      shift 2
                    else
                      shift
                    fi
                  done
                  if [[ -f "$MOCK_HISTORY_FILE" ]]; then
                    cp "$MOCK_HISTORY_FILE" "$output_path"
                  else
                    printf '[]\\n' > "$output_path"
                  fi
                  printf 'record_count=%s\\nhistory_truncated=false\\n' "${MOCK_RECORD_COUNT:-0}"
                  exit 0
                  ;;
                fetch-binary)
                  printf 'fetch-binary\\n' >> "$FETCH_CALL_LOG"
                  status="${MOCK_BINARY_STATUS:-0}"
                  if [[ "$status" != 0 ]]; then exit "$status"; fi
                  shift 2
                  binary_directory=
                  while (($#)); do
                    if [[ "$1" == --binary-directory ]]; then
                      binary_directory="$2"
                      shift 2
                    else
                      shift
                    fi
                  done
                  mkdir -p "$binary_directory"
                  printf 'dev binary' > "$binary_directory/Teagram"
                  exit 0
                  ;;
                evaluate)
                  printf 'evaluate\\n' >> "$EVALUATE_CALL_LOG"
                  status="${MOCK_EVALUATE_STATUS:-0}"
                  if [[ "$status" != 0 ]]; then exit "$status"; fi
                  ;;
              esac
            fi

            exec "$REAL_PYTHON3" "$@"
            """
        )
    )
    python_stub.chmod(0o755)

    docker_stub = bin_dir / "docker"
    docker_stub.write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env python3
            import os
            from pathlib import Path
            import sys
            import time

            count_path = Path(os.environ["DOCKER_COUNT_FILE"])
            count = int(count_path.read_text()) if count_path.exists() else 0
            count += 1
            count_path.write_text(str(count))
            binary = ""
            for index, value in enumerate(sys.argv):
                if value == "-e" and index + 1 < len(sys.argv):
                    if sys.argv[index + 1].startswith("DESKTOP_SCALE_BINARY="):
                        binary = sys.argv[index + 1].split("=", 1)[1]
            with Path(os.environ["DOCKER_CALL_LOG"]).open("a") as log:
                log.write(f"{count} {binary}\\n")
            sleep = float(os.environ.get("MOCK_DOCKER_SLEEP_SECONDS", "0"))
            if sleep:
                time.sleep(sleep)
            status = int(os.environ.get("MOCK_DOCKER_STATUS", "0"))
            fail_before_report = (
                os.environ.get("MOCK_DOCKER_FAIL_BEFORE_REPORT") == "true"
            )
            if status and fail_before_report:
                sys.exit(status)
            slow_counts = set(os.environ.get("MOCK_DOCKER_SLOW_COUNTS", "").split(","))
            stall_counts = set(os.environ.get("MOCK_DOCKER_STALL_COUNTS", "").split(","))
            one = "13.00" if str(count) in slow_counts else "10.00"
            dialogs_stalls = "1" if str(count) in stall_counts else "0"
            print(f\"\"\"DESKTOP_SCALE_REPORT_BEGIN
            | Scenario | 1x | 100x | Ratio | Absolute budget | Noise band |
            | --- | ---: | ---: | ---: | --- | --- |
            | messages.getDialogs page (100 dialogs) | {one} ms | 12.00 ms | 1.20x | 50 ms | calibrating |
            | contacts.getContacts full response | {one} ms | 12.00 ms | 1.20x | 50 ms | calibrating |
            | contact change + name lookup | {one} ms | 12.00 ms | 1.20x | 50 ms | calibrating |
            | cache read x100 | {one} ms | 12.00 ms | 1.20x | 50 ms | calibrating |
            Directory enumerations per thumbnail cache access: 1x=0/100, 100x=0/100.
            Main-thread steps over 50 ms: dialogs 1x={dialogs_stalls}, 100x=0; contacts.getContacts 1x=0, 100x=0; contact change and lookup 1x=0, 100x=0; thumbnail reads 1x=0, 100x=0; total={dialogs_stalls}.
            Scenario checks: dialogs=1/1 contacts=1/1 contact change=1/1 cache=1/1.
            Outcome: PASS
            DESKTOP_SCALE_REPORT_END\"\"\")
            if status:
                sys.exit(status)
            """
        )
    )
    docker_stub.chmod(0o755)

    env = os.environ.copy()
    env.update(
        {
            "PATH": f"{bin_dir}:{env['PATH']}",
            "REAL_PYTHON3": sys.executable,
            "RUNNER_TEMP": str(runner_temp),
            "GITHUB_STEP_SUMMARY": str(root / "step-summary.md"),
            "GITHUB_OUTPUT": str(root / "github-output.txt"),
            "GITHUB_RUN_ID": "123",
            "GITHUB_RUN_ATTEMPT": "1",
            "GITHUB_SHA": "head-sha",
            "GITHUB_REF": "refs/pull/285/merge",
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_API_URL": "https://api.github.com",
            "GITHUB_REPOSITORY": "owner/repo",
            "GH_TOKEN": "token",
            "PR_BASE_SHA": "base-sha",
            "IMAGE_TAG": "tdesktop:test",
            "DESKTOP_SCALE_RUN_BUDGET_SECONDS": "60",
            "MOCK_RECORD_COUNT": str(len(history or [])),
            "MOCK_HISTORY_FILE": str(history_file),
            "MOCK_FETCH_STATUS": "0",
            "MOCK_BINARY_STATUS": "0",
            "MOCK_EVALUATE_STATUS": "0",
            "MOCK_DOCKER_STATUS": "0",
            "MOCK_DOCKER_FAIL_BEFORE_REPORT": "false",
            "MOCK_DOCKER_SLEEP_SECONDS": "0",
            "MOCK_DOCKER_STALL_COUNTS": "",
            "MOCK_DOCKER_SLOW_COUNTS": "",
            "FETCH_CALL_LOG": str(root / "fetch-calls.txt"),
            "EVALUATE_CALL_LOG": str(root / "evaluate-calls.txt"),
            "DOCKER_COUNT_FILE": str(root / "docker-count.txt"),
            "DOCKER_CALL_LOG": str(root / "docker-calls.txt"),
        }
    )
    return repo_root, runner_temp, env


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

    def test_main_thread_stalls_remain_valid_timing_samples(self):
        self.require_implementation()
        report = REPORT.replace("dialogs 1x=0, 100x=0", "dialogs 1x=1, 100x=0")
        report = report.replace("thumbnail reads 1x=0, 100x=0; total=0", "thumbnail reads 1x=0, 100x=0; total=1")

        sample = scale_check.parse_report(report)

        self.assertTrue(sample["valid"])
        self.assertEqual(
            sample["scenarios"]["messages.getDialogs page (100 dialogs)"]["one_stalls"],
            1,
        )

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
        records = [make_sample() for _ in range(25)]
        for index, record in enumerate(records):
            record["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
            record["scenarios"]["cache read x100"]["one_ms"] = (
                100.0 if index < 5 else 10.0
            )
        selected = scale_check.latest_records(records, 20)
        self.assertEqual(len(selected), 20)
        self.assertEqual(selected[0]["scenarios"]["cache read x100"]["one_ms"], 10.0)
        self.assertEqual(selected[-1]["scenarios"]["cache read x100"]["one_ms"], 10.0)
        result = scale_check.evaluate(
            records, [make_sample()], [make_sample()], report_only=True
        )
        self.assertEqual(result["rows"]["cache read x100"]["noise_one_ms"], 0.0)

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

        for artifact_id, created_at, expired, branch in (
            (22, "2026-11-22T00:00:00Z", True, "dev"),
            (23, "2026-11-23T00:00:00Z", False, "feature/scale"),
        ):
            sample = make_sample(one_ms=100.0)
            sample["created_at"] = created_at
            archive = BytesIO()
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr(
                    "desktop-scale-sample.json", json.dumps(sample).encode()
                )
            archives[artifact_id] = archive.getvalue()
            artifacts.append(
                {
                    "id": artifact_id,
                    "expired": expired,
                    "created_at": created_at,
                    "workflow_run": {"head_branch": branch},
                }
            )

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
                side_effect=lambda _api, _token, _repo, artifact_id, **_kwargs: archives[
                    artifact_id
                ],
            ):
                records, binary_found, history_truncated = scale_check.fetch_baseline(
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
            self.assertFalse(history_truncated)
            self.assertEqual((binary_dir / "Teagram").read_bytes(), b"dev app")
            self.assertTrue(binary_dir.joinpath("Teagram").stat().st_mode & 0o111)

    def test_history_only_fetch_cli_does_not_request_a_base_binary(self):
        self.require_implementation()
        sample = make_sample()
        sample["created_at"] = "2026-11-10T00:00:00Z"
        archive_bytes = BytesIO()
        with zipfile.ZipFile(archive_bytes, "w") as bundle:
            bundle.writestr(scale_check.SAMPLE_FILE_NAME, json.dumps(sample).encode())
        artifacts = [
            {
                "id": 1,
                "expired": False,
                "created_at": sample["created_at"],
                "workflow_run": {"head_branch": "dev"},
            }
        ]

        with tempfile.TemporaryDirectory() as directory:
            records_path = Path(directory) / "history.json"
            stdout = StringIO()
            with patch.object(
                scale_check,
                "_artifact_pages",
                return_value=artifacts,
            ) as list_artifacts, patch.object(
                scale_check, "_download_artifact", return_value=archive_bytes.getvalue()
            ), redirect_stdout(stdout):
                status = scale_check.main(
                    [
                        "fetch",
                        "--history-only",
                        "--api-url",
                        "https://api.github.com",
                        "--token",
                        "token",
                        "--repository",
                        "owner/repo",
                        "--records-output",
                        str(records_path),
                    ]
                )

            self.assertEqual(json.loads(records_path.read_text()), [sample])

        self.assertEqual(status, 0)
        self.assertIn("record_count=1", stdout.getvalue())
        self.assertIn("binary_available=false", stdout.getvalue())
        self.assertEqual(
            list_artifacts.call_args.args[-1], scale_check.DEV_ARTIFACT_NAME
        )

    def test_missing_base_binary_has_an_actionable_error(self):
        self.require_implementation()
        records = [make_sample() for _ in range(20)]
        artifacts = []
        archives = {}
        for index, sample in enumerate(records):
            sample["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
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
        wrong_base = {
            "id": 100,
            "expired": False,
            "created_at": "2026-11-22T00:00:00Z",
            "workflow_run": {"head_branch": "dev", "head_sha": "other-sha"},
        }
        artifacts_by_name = {
            "desktop-scale-dev": artifacts,
            "Teagram": [wrong_base],
        }

        with tempfile.TemporaryDirectory() as directory:
            with patch.object(
                scale_check,
                "_artifact_pages",
                side_effect=lambda _api, _token, _repo, name: artifacts_by_name[name],
            ), patch.object(
                scale_check,
                "_download_artifact",
                side_effect=lambda _api, _token, _repo, artifact_id, **_kwargs: archives[
                    artifact_id
                ],
            ):
                with self.assertRaisesRegex(
                    scale_check.ScaleCheckError,
                    "base SHA base-sha.*rebase.*rerun",
                ):
                    scale_check.fetch_baseline(
                        "https://api.github.com",
                        "token",
                        "owner/repo",
                        "base-sha",
                        Path(directory) / "history.json",
                        Path(directory) / "base",
                        require_binary=True,
                    )

    def test_base_binary_download_retries_once_and_fails_after_the_second_error(self):
        self.require_implementation()
        artifact = {
            "id": 100,
            "expired": False,
            "created_at": "2026-11-22T00:00:00Z",
            "workflow_run": {"head_branch": "dev", "head_sha": "base-sha"},
        }
        binary_archive = BytesIO()
        with zipfile.ZipFile(binary_archive, "w") as bundle:
            bundle.writestr("Teagram", b"dev app")
        archive_bytes = binary_archive.getvalue()
        stdout = StringIO()

        with tempfile.TemporaryDirectory() as directory:
            with patch.object(
                scale_check, "_artifact_pages", return_value=[artifact]
            ), patch.object(
                scale_check,
                "_download_artifact",
                side_effect=[scale_check.ScaleCheckError("temporary"), archive_bytes],
            ) as download:
                with redirect_stdout(stdout):
                    status = scale_check.main(
                        [
                            "fetch-binary",
                            "--api-url",
                            "https://api.github.com",
                            "--token",
                            "token",
                            "--repository",
                            "owner/repo",
                            "--base-sha",
                            "base-sha",
                            "--binary-directory",
                            str(Path(directory) / "base"),
                        ]
                    )

            self.assertEqual(status, 0)
            self.assertIn("binary_available=true", stdout.getvalue())
            self.assertEqual(
                (Path(directory) / "base" / "Teagram").read_bytes(), b"dev app"
            )
            self.assertEqual(download.call_count, 2)

        stderr = StringIO()
        with patch.object(
            scale_check, "_artifact_pages", return_value=[artifact]
        ), patch.object(
            scale_check,
            "_download_artifact",
            side_effect=[scale_check.ScaleCheckError("first"), scale_check.ScaleCheckError("second")],
        ) as download:
            with redirect_stderr(stderr):
                status = scale_check.main(
                    [
                        "fetch-binary",
                        "--api-url",
                        "https://api.github.com",
                        "--token",
                        "token",
                        "--repository",
                        "owner/repo",
                        "--base-sha",
                        "base-sha",
                        "--binary-directory",
                        "unused-binary-directory",
                    ]
                )

        self.assertEqual(status, 2)
        self.assertIn("after one retry", stderr.getvalue())
        self.assertEqual(download.call_count, 2)

    def test_artifact_reads_are_capped_and_history_cut_short_is_reported(self):
        self.require_implementation()
        artifacts = []
        archives = {}
        for index in range(5):
            sample = make_sample()
            sample["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
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
        downloaded = []

        with tempfile.TemporaryDirectory() as directory:
            with patch.object(scale_check, "MAX_DEV_ARTIFACT_READS", 3), patch.object(
                scale_check, "_artifact_pages", return_value=artifacts
            ), patch.object(
                scale_check,
                "_download_artifact",
                side_effect=lambda _api, _token, _repo, artifact_id, **_kwargs: (
                    downloaded.append(artifact_id) or archives[artifact_id]
                ),
            ):
                records, binary_found, history_truncated = scale_check.fetch_baseline(
                    "https://api.github.com",
                    "token",
                    "owner/repo",
                    "base-sha",
                    Path(directory) / "history.json",
                )

        self.assertEqual(len(downloaded), 3)
        self.assertEqual(downloaded, [5, 4, 3])
        self.assertEqual(len(records), 3)
        self.assertFalse(binary_found)
        self.assertTrue(history_truncated)
        result = scale_check.evaluate(
            records,
            [],
            [make_sample()],
            history_truncated=history_truncated,
        )
        summary = scale_check.format_summary(result, make_sample())
        self.assertIn("History was cut short", summary)

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

    def test_three_sigma_noise_term_can_allow_a_two_point_five_ms_increase(self):
        self.require_implementation()
        records = []
        for index in range(20):
            value = 9.0 if index < 10 else 11.0
            sample = make_sample(one_ms=value, hundred_ms=12.0)
            sample["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
            records.append(sample)
        base = [make_sample(one_ms=10.0), make_sample(one_ms=10.0)]
        head = [make_sample(one_ms=12.5), make_sample(one_ms=12.5)]

        result = scale_check.evaluate(records, base, head)

        self.assertAlmostEqual(result["rows"]["cache read x100"]["noise_one_ms"], 1.0)
        self.assertFalse(result["failures"])

    def test_fifteen_percent_noise_term_can_allow_a_five_point_five_ms_increase(self):
        self.require_implementation()
        records = []
        for index in range(20):
            value = 39.9 if index < 10 else 40.1
            sample = make_sample(one_ms=value, hundred_ms=48.0)
            sample["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
            records.append(sample)
        base = [make_sample(one_ms=40.0, hundred_ms=48.0)] * 2
        head = [make_sample(one_ms=45.5, hundred_ms=48.0)] * 2

        result = scale_check.evaluate(records, base, head)

        self.assertAlmostEqual(result["rows"]["cache read x100"]["noise_one_ms"], 0.1)
        self.assertFalse(result["failures"])

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

    def test_retry_absolute_budget_uses_only_the_retry_head_sample(self):
        self.require_implementation()
        records = [make_sample() for _ in range(20)]
        base = [make_sample(), make_sample()]
        first_pair = [make_sample(stalls=1), make_sample()]
        clean_retry = make_sample()
        stalled_retry = make_sample(stalls=1)

        clean = scale_check.evaluate(
            records,
            base,
            [*first_pair, clean_retry],
            absolute_budget_head_runs=[clean_retry],
        )
        reproduced = scale_check.evaluate(
            records,
            base,
            [*first_pair, stalled_retry],
            absolute_budget_head_runs=[stalled_retry],
        )

        self.assertFalse(any("50 ms" in failure for failure in clean["failures"]))
        self.assertTrue(any("50 ms" in failure for failure in reproduced["failures"]))

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

    def test_evaluate_cli_returns_one_only_for_timing_thresholds(self):
        self.require_implementation()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            history = [make_sample() for _ in range(20)]
            history_path = root / "history.json"
            history_path.write_text(json.dumps(history))
            base_paths = []
            passing_head_paths = []
            failing_head_paths = []
            for index in range(2):
                base_path = root / f"base-{index}.json"
                passing_path = root / f"passing-head-{index}.json"
                failing_path = root / f"failing-head-{index}.json"
                base_path.write_text(json.dumps(make_sample()))
                passing_path.write_text(json.dumps(make_sample()))
                failing_path.write_text(json.dumps(make_sample(one_ms=13.0)))
                base_paths.append(base_path)
                passing_head_paths.append(passing_path)
                failing_head_paths.append(failing_path)

            def evaluate_cli(head_paths, name):
                command = [
                    sys.executable,
                    str(Path(__file__).with_name("desktop_scale_check.py")),
                    "evaluate",
                    "--history",
                    str(history_path),
                    "--output",
                    str(root / f"{name}-evaluation.json"),
                ]
                for path in base_paths:
                    command.extend(["--base", str(path)])
                for path in head_paths:
                    command.extend(["--head", str(path)])
                return subprocess.run(
                    command, capture_output=True, text=True, check=False
                )

            passed = evaluate_cli(passing_head_paths, "passed")
            failed = evaluate_cli(failing_head_paths, "failed")

        self.assertEqual(passed.returncode, 0, passed.stderr)
        self.assertEqual(failed.returncode, 1, failed.stderr)

    def test_missing_sample_cli_error_is_not_a_threshold_failure(self):
        self.require_implementation()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            history = root / "history.json"
            history.write_text("[]")
            result = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("desktop_scale_check.py")),
                    "evaluate",
                    "--history",
                    str(history),
                    "--head",
                    str(root / "missing-head.json"),
                    "--output",
                    str(root / "evaluation.json"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("missing-head.json", result.stderr)

    def test_pr_retry_clears_an_initial_only_head_stall(self):
        self.require_implementation()
        repo_root = Path(__file__).parents[2]
        runner = repo_root / ".github/scripts/run_desktop_scale_check.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            history = [make_sample() for _ in range(20)]
            for index, sample in enumerate(history):
                sample["created_at"] = f"2026-11-{index + 1:02d}T00:00:00Z"
            repo_root, runner_temp, env = make_runner_harness(root, history)
            env["MOCK_DOCKER_STALL_COUNTS"] = "4"
            result = subprocess.run(
                ["bash", str(runner)],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

            calls = (root / "docker-calls.txt").read_text().splitlines()
            sample_names = sorted(
                path.name for path in runner_temp.glob("desktop-scale-*.json")
            )
            summary = (root / "step-summary.md").read_text()

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(calls), 6)
        self.assertEqual(
            [line.split()[1] for line in calls],
            [
                "/desktop-scale/Teagram",
                "/usr/src/tdesktop/out/Debug/Teagram",
                "/desktop-scale/Teagram",
                "/usr/src/tdesktop/out/Debug/Teagram",
                "/desktop-scale/Teagram",
                "/usr/src/tdesktop/out/Debug/Teagram",
            ],
        )
        self.assertIn("desktop-scale-base-3.json", sample_names)
        self.assertIn("desktop-scale-head-3.json", sample_names)
        self.assertNotIn("desktop-scale-base-4.json", sample_names)
        self.assertIn("One additional interleaved pair was added", summary)
        self.assertNotIn("Timing failures:", summary)

    def test_dev_run_records_sample_before_preserving_app_failure_status(self):
        self.require_implementation()
        runner = Path(__file__).parents[2] / ".github/scripts/run_desktop_scale_check.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_root, runner_temp, env = make_runner_harness(root)
            env["GITHUB_EVENT_NAME"] = "push"
            env["GITHUB_REF"] = "refs/heads/dev"
            env["MOCK_DOCKER_STATUS"] = "42"
            result = subprocess.run(
                ["bash", str(runner)],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            output = (root / "github-output.txt").read_text()
            sample_path = runner_temp / "desktop-scale-sample.json"
            sample_exists = sample_path.exists()
            sample = json.loads(sample_path.read_text()) if sample_exists else None
            docker_calls = (root / "docker-calls.txt").read_text().splitlines()
            fetch_calls = (root / "fetch-calls.txt").read_text()

        self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
        self.assertIn("recorded=true", output)
        self.assertTrue(sample_exists)
        self.assertTrue(sample["valid"])
        self.assertEqual(len(docker_calls), 1)
        self.assertNotIn("fetch-binary", fetch_calls)

    def test_history_fetch_errors_fall_back_to_report_only(self):
        self.require_implementation()
        runner = Path(__file__).parents[2] / ".github/scripts/run_desktop_scale_check.sh"
        for fetch_status in ("2", "124"):
            with self.subTest(fetch_status=fetch_status), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                repo_root, _runner_temp, env = make_runner_harness(root)
                env["MOCK_FETCH_STATUS"] = fetch_status
                result = subprocess.run(
                    ["bash", str(runner)],
                    cwd=repo_root,
                    env=env,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                summary = (root / "step-summary.md").read_text()

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("history could not be read", summary)
            if fetch_status == "124":
                self.assertIn("::error::", result.stdout)
                self.assertIn("timed out after 55 seconds", result.stdout)

    def test_binary_fetch_error_after_history_is_a_hard_failure(self):
        self.require_implementation()
        history = [make_sample() for _ in range(20)]
        runner = Path(__file__).parents[2] / ".github/scripts/run_desktop_scale_check.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_root, _runner_temp, env = make_runner_harness(root, history)
            env["MOCK_BINARY_STATUS"] = "2"
            result = subprocess.run(
                ["bash", str(runner)],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            summary = (root / "step-summary.md").read_text()
            fetch_calls = (root / "fetch-calls.txt").read_text().splitlines()

        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(fetch_calls, ["fetch", "fetch-binary"])
        self.assertIn("PR base Teagram artifact fetch failed", result.stdout)
        self.assertIn("No timing verdict", summary)
        self.assertNotIn("report-only", summary)
        self.assertFalse((root / "docker-calls.txt").exists())

    def test_evaluation_error_summary_names_the_step_and_status(self):
        self.require_implementation()
        history = [make_sample() for _ in range(20)]
        runner = Path(__file__).parents[2] / ".github/scripts/run_desktop_scale_check.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_root, _runner_temp, env = make_runner_harness(root, history)
            env["MOCK_EVALUATE_STATUS"] = "2"
            result = subprocess.run(
                ["bash", str(runner)],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            summary = (root / "step-summary.md").read_text()

        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("evaluation failed with status 2", summary)
        self.assertNotIn("unknown", summary)

    def test_nonzero_run_without_report_returns_the_run_status(self):
        self.require_implementation()
        runner = Path(__file__).parents[2] / ".github/scripts/run_desktop_scale_check.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_root, _runner_temp, env = make_runner_harness(root)
            env["MOCK_DOCKER_STATUS"] = "42"
            env["MOCK_DOCKER_FAIL_BEFORE_REPORT"] = "true"
            result = subprocess.run(
                ["bash", str(runner)],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            summary = (root / "step-summary.md").read_text()

        self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
        self.assertIn("head-1 failed with status 42", summary)
        self.assertNotIn("unknown", summary)
        self.assertNotIn("did not emit its report", result.stdout)

    def test_timeout_and_budget_exhaustion_paths_report_status_124(self):
        self.require_implementation()
        runner = Path(__file__).parents[2] / ".github/scripts/run_desktop_scale_check.sh"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_root, _runner_temp, env = make_runner_harness(root)
            env["DESKTOP_SCALE_RUN_BUDGET_SECONDS"] = "0"
            result = subprocess.run(
                ["bash", str(runner)],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            summary = (root / "step-summary.md").read_text()

        self.assertEqual(result.returncode, 124, result.stdout + result.stderr)
        self.assertIn("budget was exhausted", result.stdout)
        self.assertIn("head-1 failed with status 124", summary)
        self.assertFalse((root / "docker-calls.txt").exists())

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo_root, _runner_temp, env = make_runner_harness(root)
            env["DESKTOP_SCALE_RUN_BUDGET_SECONDS"] = "1"
            env["MOCK_DOCKER_SLEEP_SECONDS"] = "3"
            result = subprocess.run(
                ["bash", str(runner)],
                cwd=repo_root,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            summary = (root / "step-summary.md").read_text()

        self.assertEqual(result.returncode, 124, result.stdout + result.stderr)
        self.assertIn("exit 124", result.stdout)
        self.assertIn("head-1 failed with status 124", summary)
        self.assertFalse((root / "evaluate-calls.txt").exists())

    def test_unexpected_cli_error_prints_a_traceback(self):
        self.require_implementation()
        stderr = StringIO()
        with patch.object(
            scale_check, "_cmd_annotate", side_effect=RuntimeError("unexpected bug")
        ), redirect_stderr(stderr):
            status = scale_check.main(["annotate", "--evaluation", "unused.json"])

        self.assertEqual(status, 2)
        self.assertIn("Traceback (most recent call last)", stderr.getvalue())
        self.assertIn("RuntimeError: unexpected bug", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
