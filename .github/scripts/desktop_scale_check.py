#!/usr/bin/env python3
import argparse
from datetime import datetime, timezone
from io import BytesIO
import json
import math
from pathlib import Path
import re
import statistics
import sys
import traceback
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPRedirectHandler, Request, build_opener, urlopen
from zipfile import BadZipFile, ZipFile


MIN_DEV_RUNS = 20
ABSOLUTE_BUDGET_MS = 50.0
MAX_SCALE_RATIO = 1.5
DEV_ARTIFACT_NAME = "desktop-scale-dev"
APP_ARTIFACT_NAME = "Teagram"
SAMPLE_FILE_NAME = "desktop-scale-sample.json"
MAX_DEV_ARTIFACT_READS = 40
LEGACY_SCENARIOS = {
    "messages.getDialogs page (100 dialogs)": "dialogs",
    "contacts.getContacts full response": "contacts.getContacts",
    "contact change + name lookup": "contact change and lookup",
    "cache read x100": "thumbnail reads",
}
ADDITIONAL_SCENARIOS = {
    "private history page (1M messages; 20 / 2,000 loaded)": "private history",
    "jump to date in 1M-message history": "jump to date",
    "group media page (200K messages; 20 / 2,000 loaded)": "group media",
    "messages.searchGlobal page (100 hits; 50 / 5,000 dialogs)": "search",
    "updates.getDifference (1,000 messages; 50 / 5,000 dialogs)": "update burst",
}
SCENARIOS = {**LEGACY_SCENARIOS, **ADDITIONAL_SCENARIOS}
STATIC_EXCEPTIONS = {
    "contacts.getContacts full response": "MAIN-3657 (contacts)",
    "contact change + name lookup": "MAIN-3657 (contacts)",
}
REPORT_ONLY_SCENARIOS = {
    "updates.getDifference (1,000 messages; 50 / 5,000 dialogs)": (
        "updates.getDifference timing is report-only"
    ),
}


class ScaleCheckError(Exception):
    pass


class PersistentReadError(ScaleCheckError):
    pass


def _duration(value):
    value = value.strip()
    if value == "n/a":
        return None
    match = re.fullmatch(r"([0-9]+(?:\.[0-9]+)?) ms", value)
    if not match:
        raise ScaleCheckError(f"invalid scale-check duration: {value}")
    return float(match.group(1))


def parse_report(report):
    if "DESKTOP_SCALE_REPORT_BEGIN\n" not in report:
        raise ScaleCheckError("scale check did not emit its report table")
    table = report.split("DESKTOP_SCALE_REPORT_BEGIN\n", 1)[1].split(
        "\nDESKTOP_SCALE_REPORT_END", 1
    )[0]
    scenario_rows = {}
    for line in table.splitlines():
        match = re.fullmatch(
            r"\|\s*(.*?)\s*\|\s*(.*?)\s*\|\s*(.*?)\s*\|\s*"
            r"(.*?)\s*\|\s*50 ms\s*\|\s*.*?\s*\|",
            line,
        )
        if not match or match.group(1) not in SCENARIOS:
            continue
        name = match.group(1)
        scenario_rows[name] = {
            "one_ms": _duration(match.group(2)),
            "hundred_ms": _duration(match.group(3)),
        }
    if not set(LEGACY_SCENARIOS).issubset(scenario_rows):
        missing = sorted(set(LEGACY_SCENARIOS) - set(scenario_rows))
        raise ScaleCheckError(
            "scale-check report is missing scenario rows: " + ", ".join(missing)
        )
    additional_rows = set(scenario_rows) & set(ADDITIONAL_SCENARIOS)
    if additional_rows and additional_rows != set(ADDITIONAL_SCENARIOS):
        missing = sorted(set(ADDITIONAL_SCENARIOS) - additional_rows)
        raise ScaleCheckError(
            "scale-check report is missing scenario rows: " + ", ".join(missing)
        )

    stalls_line = re.search(
        r"^Main-thread steps over 50 ms: (.+)\.$", report, re.MULTILINE
    )
    if not stalls_line:
        raise ScaleCheckError("scale-check report is missing main-thread stall counts")
    for label in scenario_rows:
        key = SCENARIOS[label]
        prefix = re.escape(key)
        match = re.search(
            rf"(?:^|; ){prefix} 1x=(\d+|n/a), 100x=(\d+|n/a)",
            stalls_line.group(1),
        )
        if not match:
            if label in LEGACY_SCENARIOS or additional_rows:
                raise ScaleCheckError(
                    f"scale-check report is missing stalls for {label}"
                )
            continue
        scenario_rows[label]["one_stalls"] = (
            int(match.group(1)) if match.group(1) != "n/a" else None
        )
        scenario_rows[label]["hundred_stalls"] = (
            int(match.group(2)) if match.group(2) != "n/a" else None
        )

    enum_match = re.search(
        r"^Directory enumerations per thumbnail cache access: "
        r"1x=(\d+|n/a)/100, 100x=(\d+|n/a)/100\.$",
        report,
        re.MULTILINE,
    )
    if not enum_match:
        raise ScaleCheckError("scale-check report is missing cache enumeration counts")
    enumerations = {
        "one": int(enum_match.group(1)) if enum_match.group(1) != "n/a" else None,
        "hundred": int(enum_match.group(2)) if enum_match.group(2) != "n/a" else None,
    }

    checks = re.search(
        r"^Scenario checks: dialogs=(\d)/(\d) state=(\d) contacts=(\d)/(\d) "
        r"contact change=(\d)/(\d) cache=(\d)/(\d)\.$",
        report,
        re.MULTILINE,
    )
    additional_checks = re.search(
        r"^Additional scenario checks: history=(\d)/(\d) date=(\d)/(\d) "
        r"group=(\d)/(\d) search=(\d)/(\d) updates=(\d)/(\d)\.$",
        report,
        re.MULTILINE,
    )
    outcome = re.search(r"^Outcome: (PASS|FAIL)$", report, re.MULTILINE)
    if not checks or not outcome:
        raise ScaleCheckError("scale-check report is missing its outcome")
    if bool(additional_rows) != bool(additional_checks):
        raise ScaleCheckError("scale-check report is missing additional scenario checks")
    checks_passed = all(value == "1" for value in checks.groups()) and (
        not additional_rows
        or all(value == "1" for value in additional_checks.groups())
    )
    metrics_present = all(
        row[field] is not None
        for row in scenario_rows.values()
        for field in ("one_ms", "hundred_ms", "one_stalls", "hundred_stalls")
    )
    valid = (
        outcome.group(1) == "PASS"
        and checks_passed
        and metrics_present
        and all(
            scenario_rows[name][field] > 0
            for name in scenario_rows
            for field in ("one_ms", "hundred_ms")
            if scenario_rows[name][field] is not None
        )
        and enumerations == {"one": 0, "hundred": 0}
    )
    return {
        "schema_version": 2 if additional_rows else 1,
        "valid": valid,
        "scenario_checks_passed": checks_passed,
        "outcome": outcome.group(1),
        "scenarios": scenario_rows,
        "directory_enumerations": enumerations,
    }


def latest_records(records, limit=MIN_DEV_RUNS):
    return _valid_records(records)[-limit:]


def _is_valid_record(record):
    schema_version = record.get("schema_version") if isinstance(record, dict) else None
    if (
        not isinstance(record, dict)
        or isinstance(schema_version, bool)
        or schema_version not in (1, 2)
        or record.get("valid") is not True
        or not isinstance(record.get("created_at"), str)
        or not record.get("created_at")
        or record.get("scenario_checks_passed") is not True
        or record.get("outcome") != "PASS"
        or record.get("directory_enumerations")
        != {"one": 0, "hundred": 0}
        or not isinstance(record.get("scenarios"), dict)
        or (
            schema_version == 1
            and set(record["scenarios"]) != set(LEGACY_SCENARIOS)
        )
        or (schema_version == 2 and set(record["scenarios"]) != set(SCENARIOS))
    ):
        return False
    for row in record["scenarios"].values():
        if not isinstance(row, dict):
            return False
        for field in ("one_ms", "hundred_ms"):
            value = row.get(field)
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value)
                or value <= 0
            ):
                return False
        for field in ("one_stalls", "hundred_stalls"):
            value = row.get(field)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                return False
    return True


def _valid_records(records):
    valid = []
    for record in records:
        if not isinstance(record, dict):
            raise ScaleCheckError("dev scale baseline contains a malformed record")
        if record.get("valid") is not True:
            continue
        if not _is_valid_record(record):
            raise ScaleCheckError("dev scale baseline contains an invalid sample")
        valid.append(record)
    return sorted(valid, key=lambda record: record.get("created_at", ""))


def _median(records, scenario, field):
    values = [
        record["scenarios"][scenario][field]
        for record in records
        if scenario in record["scenarios"]
    ]
    return statistics.median(values) if values else None


def _noise(records, scenario, field):
    values = [
        record["scenarios"][scenario][field]
        for record in records
        if scenario in record["scenarios"]
    ]
    if not values:
        return None
    median = statistics.median(values)
    median_absolute_deviation = statistics.median(
        abs(value - median) for value in values
    )
    return 1.4826 * median_absolute_deviation


def _baseline_exceptions(records):
    exceptions = {}
    additions = {}
    for scenario in SCENARIOS:
        if scenario in REPORT_ONLY_SCENARIOS:
            continue
        rows = [
            record["scenarios"][scenario]
            for record in records
            if scenario in record["scenarios"]
        ]
        if len(rows) < MIN_DEV_RUNS:
            continue
        stalled_runs = [
            int(bool(row["one_stalls"] or row["hundred_stalls"]))
            for row in rows
        ]
        if statistics.median(stalled_runs) > 0.5:
            reason = "dev runs exceed the 50 ms main-thread budget"
        elif _median(records, scenario, "hundred_ms") > (
            MAX_SCALE_RATIO * _median(records, scenario, "one_ms")
        ):
            reason = "dev runs exceed the 1.5x scale ratio"
        else:
            continue
        exceptions[scenario] = STATIC_EXCEPTIONS.get(scenario, reason)
        if scenario not in STATIC_EXCEPTIONS:
            additions[scenario] = reason
    return exceptions, additions


def _regression_exceeded(base_ms, head_ms, noise_sd_ms):
    increase = head_ms - base_ms
    return increase >= 2.0 and increase > max(base_ms * 0.15, 3.0 * noise_sd_ms)


def evaluate(
    history,
    base_runs,
    head_runs,
    report_only=False,
    history_truncated=False,
    history_unavailable=False,
    absolute_budget_head_runs=None,
    history_malformed_count=0,
    history_first_error=None,
):
    history = _valid_records(history)
    if absolute_budget_head_runs is None:
        absolute_budget_head_runs = head_runs
    invalid_head_outcomes = []
    if report_only:
        invalid_head_outcomes = [
            sample.get("outcome", "unknown")
            if isinstance(sample, dict)
            else "unknown"
            for sample in head_runs
            if not _is_valid_record(sample)
        ]
        base_runs = [sample for sample in base_runs if _is_valid_record(sample)]
        head_runs = [sample for sample in head_runs if _is_valid_record(sample)]
        absolute_budget_head_runs = [
            sample
            for sample in absolute_budget_head_runs
            if _is_valid_record(sample)
        ]
    if any(
        not _is_valid_record(sample)
        for sample in [*base_runs, *head_runs, *absolute_budget_head_runs]
    ):
        raise ScaleCheckError("scale check received an invalid app report")
    count = len(history)
    baseline = latest_records(history)
    scenario_counts = {
        scenario: sum(scenario in record["scenarios"] for record in baseline)
        for scenario in SCENARIOS
    }
    if count < MIN_DEV_RUNS:
        return {
            "record_count": count,
            "calibrating": count,
            "scenario_counts": scenario_counts,
            "exceptions": dict(STATIC_EXCEPTIONS),
            "additional_exceptions": {},
            "rows": {},
            "failures": [],
            "report_only": True,
            "history_truncated": history_truncated,
            "history_unavailable": history_unavailable,
            "history_malformed_count": history_malformed_count,
            "history_first_error": history_first_error,
            "invalid_head_outcomes": invalid_head_outcomes,
        }

    exceptions, additions = _baseline_exceptions(baseline)
    if not report_only and (len(base_runs) < 2 or len(head_runs) < 2):
        raise ScaleCheckError(
            "blocking scale checks require two interleaved base/head samples"
        )
    rows = {}
    failures = []
    for scenario in SCENARIOS:
        baseline_rows = [
            sample for sample in baseline if scenario in sample["scenarios"]
        ]
        base_scenario_runs = [
            sample for sample in base_runs if scenario in sample["scenarios"]
        ]
        head_scenario_runs = [
            sample for sample in head_runs if scenario in sample["scenarios"]
        ]
        noise_one = _noise(baseline, scenario, "one_ms")
        noise_hundred = _noise(baseline, scenario, "hundred_ms")
        row_result = {
            "exception": exceptions.get(scenario),
            "baseline_count": len(baseline_rows),
            "noise_one_ms": noise_one,
            "noise_hundred_ms": noise_hundred,
            "base_one_ms": None,
            "head_one_ms": None,
            "base_hundred_ms": None,
            "head_hundred_ms": None,
            "ratio": None,
            "failures": [],
        }
        if head_scenario_runs:
            row_result["head_one_ms"] = statistics.median(
                sample["scenarios"][scenario]["one_ms"]
                for sample in head_scenario_runs
            )
            row_result["head_hundred_ms"] = statistics.median(
                sample["scenarios"][scenario]["hundred_ms"]
                for sample in head_scenario_runs
            )
            if row_result["head_one_ms"] > 0:
                row_result["ratio"] = (
                    row_result["head_hundred_ms"] / row_result["head_one_ms"]
                )
        if base_scenario_runs:
            row_result["base_one_ms"] = statistics.median(
                sample["scenarios"][scenario]["one_ms"]
                for sample in base_scenario_runs
            )
            row_result["base_hundred_ms"] = statistics.median(
                sample["scenarios"][scenario]["hundred_ms"]
                for sample in base_scenario_runs
            )
        rows[scenario] = row_result

        if (
            report_only
            or scenario in exceptions
            or scenario in REPORT_ONLY_SCENARIOS
            or not head_scenario_runs
        ):
            continue
        absolute_budget_rows = [
            sample["scenarios"][scenario]
            for sample in absolute_budget_head_runs
            if scenario in sample["scenarios"]
        ]
        if any(
            row["one_stalls"] or row["hundred_stalls"]
            for row in absolute_budget_rows
        ):
            row_result["failures"].append(
                f"{scenario}: a main-thread step exceeded "
                f"{ABSOLUTE_BUDGET_MS:g} ms"
            )
        if (
            len(baseline_rows) >= MIN_DEV_RUNS
            and row_result["ratio"] is not None
            and row_result["ratio"] > MAX_SCALE_RATIO
        ):
            row_result["failures"].append(
                f"{scenario}: 100x takes {row_result['ratio']:.2f}x as long as 1x "
                "(limit 1.5x)"
            )
        if len(baseline_rows) >= MIN_DEV_RUNS and base_scenario_runs:
            for scale, base_field, head_field, noise in (
                ("1x", "base_one_ms", "head_one_ms", noise_one),
                ("100x", "base_hundred_ms", "head_hundred_ms", noise_hundred),
            ):
                base_ms = row_result[base_field]
                head_ms = row_result[head_field]
                if _regression_exceeded(base_ms, head_ms, noise):
                    row_result["failures"].append(
                        f"{scenario}: {scale} median is {head_ms - base_ms:.2f} ms "
                        f"slower than dev (limit max(15%, 3σ) and 2 ms)"
                    )
        failures.extend(row_result["failures"])

    return {
        "record_count": count,
        "calibrating": None,
        "scenario_counts": scenario_counts,
        "exceptions": exceptions,
        "additional_exceptions": additions,
        "rows": rows,
        "failures": failures,
        "report_only": report_only,
        "history_truncated": history_truncated,
        "history_unavailable": history_unavailable,
        "history_malformed_count": history_malformed_count,
        "history_first_error": history_first_error,
        "invalid_head_outcomes": invalid_head_outcomes,
    }


def _format_ms(value):
    return "n/a" if value is None else f"{value:.2f} ms"


def format_summary(result, current_sample=None, retry_note=None):
    lines = ["### Desktop data scale check", ""]
    if result["calibrating"] is not None:
        lines.append(
            f"Timing thresholds are report-only while calibrating: "
            f"{result['calibrating']}/{MIN_DEV_RUNS}."
        )
    else:
        lines.append(
            f"Noise uses the latest {MIN_DEV_RUNS} valid `dev` runs; "
            "PR timing comparisons use two interleaved `dev`/head pairs, "
            "with one extra pair after an initial timing failure."
        )
    if result.get("history_unavailable"):
        lines.extend(
            ["", "Dev timing history could not be read; this run is report-only."]
        )
    elif result.get("history_truncated"):
        lines.extend(
            [
                "",
                f"History was cut short at the {MAX_DEV_ARTIFACT_READS}-artifact "
                f"read cap with {result['record_count']}/{MIN_DEV_RUNS} valid "
                "samples; this run is report-only.",
            ]
        )
    if result.get("history_malformed_count", 0):
        first_error = result.get("history_first_error") or "unspecified record error"
        lines.extend(
            [
                "",
                f"Skipped {result['history_malformed_count']} malformed dev scale "
                f"artifact record(s). First error: {first_error}.",
            ]
        )
    for outcome in result.get("invalid_head_outcomes", []):
        lines.extend(
            [
                "",
                f"app report Outcome: {outcome}; sample recorded as invalid.",
            ]
        )
    if retry_note:
        lines.extend(["", retry_note])
    lines.extend(
        [
            "",
            "| Scenario | 1x base → head | 100x base → head | Head ratio | "
            "Absolute budget | Noise band |",
            "| --- | ---: | ---: | ---: | --- | --- |",
        ]
    )
    for scenario in SCENARIOS:
        row = result["rows"].get(scenario, {})
        if result["calibrating"] is not None:
            current = (
                current_sample.get("scenarios", {}).get(scenario, {})
                if current_sample
                else {}
            )
            one = _format_ms(current.get("one_ms"))
            hundred = _format_ms(current.get("hundred_ms"))
            one_pair = f"n/a → {one}"
            hundred_pair = f"n/a → {hundred}"
            one_ms = current.get("one_ms")
            hundred_ms = current.get("hundred_ms")
            ratio = (
                f"{hundred_ms / one_ms:.2f}x"
                if one_ms and hundred_ms is not None
                else "n/a"
            )
            baseline_count = result.get("scenario_counts", {}).get(scenario, 0)
            noise = f"calibrating: {baseline_count}/{MIN_DEV_RUNS}"
        else:
            one_pair = (
                f"{_format_ms(row.get('base_one_ms'))} → "
                f"{_format_ms(row.get('head_one_ms'))}"
            )
            hundred_pair = (
                f"{_format_ms(row.get('base_hundred_ms'))} → "
                f"{_format_ms(row.get('head_hundred_ms'))}"
            )
            ratio = "n/a" if row.get("ratio") is None else f"{row['ratio']:.2f}x"
            baseline_count = row.get("baseline_count", 0)
            if baseline_count < MIN_DEV_RUNS:
                noise = f"calibrating: {baseline_count}/{MIN_DEV_RUNS}"
            else:
                noise = (
                    f"1x ±3σ {_format_ms(3 * row['noise_one_ms'])}; "
                    f"100x ±3σ {_format_ms(3 * row['noise_hundred_ms'])}"
                )
        exception = (
            row.get("exception")
            or result["exceptions"].get(scenario)
            or REPORT_ONLY_SCENARIOS.get(scenario)
        )
        budget = (
            f"report-only ({exception})"
            if exception
            else f"{ABSOLUTE_BUDGET_MS:g} ms main-thread step"
        )
        lines.append(
            f"| {scenario} | {one_pair} | {hundred_pair} | {ratio} | "
            f"{budget} | {noise} |"
        )
    if result["additional_exceptions"]:
        names = "; ".join(
            f"{name} ({reason})"
            for name, reason in sorted(result["additional_exceptions"].items())
        )
        lines.extend(
            [
                "",
                "Additional dev-baseline exceptions: " + names + ".",
                "Report these new exceptions to Tech Lead before widening the gate.",
            ]
        )
    if result["failures"]:
        lines.extend(["", "Timing failures:"])
        lines.extend(f"- {failure}" for failure in result["failures"])
    return "\n".join(lines) + "\n"


def format_annotations(result):
    if not result["additional_exceptions"]:
        return []
    names = ", ".join(sorted(result["additional_exceptions"]))
    return [
        "::warning::Desktop scale check added report-only dev-baseline "
        f"exceptions: {names}. Report to Tech Lead."
    ]


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        return None


def _api_json(api_url, token, endpoint, query=None, timeout=10):
    url = api_url.rstrip("/") + endpoint
    if query:
        url += "?" + urlencode(query)
    request = Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2026-03-10",
        },
    )
    with urlopen(request, timeout=timeout) as response:
        return json.loads(response.read())


def _artifact_pages(api_url, token, repository, name):
    page = 1
    artifacts = []
    while True:
        result = _api_json(
            api_url,
            token,
            f"/repos/{repository}/actions/artifacts",
            {"name": name, "per_page": 100, "page": page},
        )
        batch = result.get("artifacts", [])
        artifacts.extend(batch)
        if len(batch) < 100 or len(artifacts) >= result.get("total_count", 0):
            break
        page += 1
    return artifacts


def _retry_read(operation, description):
    for attempt in range(2):
        try:
            return operation()
        except (OSError, ScaleCheckError, json.JSONDecodeError) as error:
            if attempt == 1:
                raise PersistentReadError(
                    f"{description} failed after one retry: {error}"
                ) from error


def _run_artifacts(api_url, token, repository, run_id):
    result = _retry_read(
        lambda: _api_json(
            api_url,
            token,
            f"/repos/{repository}/actions/runs/{run_id}/artifacts",
            {"per_page": 100, "page": 1},
        ),
        f"reading artifacts for dev run {run_id}",
    )
    return result.get("artifacts", [])


def _download_artifact(api_url, token, repository, artifact_id, timeout=60):
    endpoint = (
        f"/repos/{repository}/actions/artifacts/{artifact_id}/zip"
    )
    request = Request(
        api_url.rstrip("/") + endpoint,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2026-03-10",
        },
    )
    opener = build_opener(_NoRedirect())
    try:
        response = opener.open(request, timeout=timeout)
    except HTTPError as error:
        if error.code not in (301, 302, 303, 307, 308):
            raise ScaleCheckError(
                f"GitHub artifact download failed with HTTP {error.code}"
            ) from error
        location = error.headers.get("Location")
        if not location:
            raise ScaleCheckError("GitHub artifact download omitted its redirect")
        with urlopen(location, timeout=timeout) as archive_response:
            return archive_response.read()
    with response:
        return response.read()


def _artifact_file(archive_bytes, filename):
    try:
        with ZipFile(BytesIO(archive_bytes)) as archive:
            matches = [
                name
                for name in archive.namelist()
                if Path(name).name == filename and not name.endswith("/")
            ]
            if len(matches) != 1:
                raise ScaleCheckError(
                    f"artifact must contain exactly one {filename} file"
                )
            return archive.read(matches[0])
    except BadZipFile as error:
        raise ScaleCheckError("GitHub artifact is not a valid zip archive") from error


def _valid_dev_artifacts(artifacts):
    matches = []
    for artifact in artifacts:
        run = artifact.get("workflow_run") or {}
        if (
            artifact.get("expired") is False
            and run.get("head_branch") == "dev"
        ):
            matches.append(artifact)
    matches.sort(key=lambda artifact: artifact.get("created_at", ""))
    return matches


def fetch_history(api_url, token, repository, records_output):
    artifacts = _valid_dev_artifacts(
        _retry_read(
            lambda: _artifact_pages(
                api_url, token, repository, DEV_ARTIFACT_NAME
            ),
            "listing dev scale artifacts",
        )
    )
    candidate_artifacts = artifacts[-MAX_DEV_ARTIFACT_READS:]
    records = []
    malformed_count = 0
    first_error = None
    for artifact in reversed(candidate_artifacts):
        try:
            artifact_id = artifact["id"]
            archive_bytes = _retry_read(
                lambda: _download_artifact(
                    api_url, token, repository, artifact_id, timeout=15
                ),
                f"reading dev scale artifact {artifact_id}",
            )
        except (KeyError, ScaleCheckError) as error:
            if isinstance(error, PersistentReadError):
                raise
            malformed_count += 1
            first_error = first_error or f"artifact metadata is invalid: {error}"
            continue
        try:
            contents = _artifact_file(archive_bytes, SAMPLE_FILE_NAME)
            sample = json.loads(contents)
            if not isinstance(sample, dict):
                raise ScaleCheckError("sample is not a JSON object")
            if sample.get("valid") is False:
                continue
            if sample.get("valid") is not True:
                raise ScaleCheckError("sample has an invalid validity flag")
            if not _is_valid_record(sample):
                raise ScaleCheckError("sample failed the schema validity checks")
        except (ScaleCheckError, json.JSONDecodeError, UnicodeDecodeError) as error:
            malformed_count += 1
            first_error = first_error or f"artifact {artifact.get('id', 'unknown')}: {error}"
            continue
        records.append(sample)
        if len(records) >= MIN_DEV_RUNS:
            break
    history_truncated = (
        len(records) < MIN_DEV_RUNS
        and len(artifacts) > len(candidate_artifacts)
    )
    records.sort(key=lambda sample: sample.get("created_at", ""))
    Path(records_output).write_text(json.dumps(records, indent=2) + "\n")
    return records, history_truncated, malformed_count, first_error


def fetch_base_binary(
    api_url,
    token,
    repository,
    base_sha,
    binary_directory,
    require_binary=True,
):
    runs_result = _retry_read(
        lambda: _api_json(
            api_url,
            token,
            f"/repos/{repository}/actions/runs",
            {
                "head_sha": base_sha,
                "branch": "dev",
                "per_page": 100,
                "page": 1,
            },
        ),
        f"finding the dev run for PR base SHA {base_sha}",
    )
    runs = [
        run
        for run in runs_result.get("workflow_runs", [])
        if (
            run.get("head_sha") == base_sha
            and run.get("head_branch") == "dev"
            and Path(run.get("path", "")).name == "linux.yml"
        )
    ]
    runs.sort(key=lambda run: run.get("created_at", ""), reverse=True)
    if not runs:
        if not require_binary:
            return False
        raise ScaleCheckError(
            f"no dev Linux run or Teagram artifact matches PR base SHA {base_sha}; "
            "rebase onto an earlier dev commit or rerun after the dev build "
            "for this SHA completes"
        )

    completed_runs = [
        run
        for run in runs
        if run.get("status") == "completed" and run.get("conclusion") == "success"
    ]
    run = completed_runs[0] if completed_runs else runs[0]
    run_id = run.get("id")
    if run_id is None:
        raise ScaleCheckError(f"dev run for PR base SHA {base_sha} has no run ID")
    binary_artifacts = _run_artifacts(api_url, token, repository, run_id)
    matching = [
        artifact
        for artifact in binary_artifacts
        if artifact.get("name") == APP_ARTIFACT_NAME
        and artifact.get("expired") is False
    ]
    matching.sort(key=lambda artifact: artifact.get("created_at", ""))
    if not matching:
        if not require_binary:
            return False
        raise ScaleCheckError(
            f"no dev Teagram artifact matches PR base SHA {base_sha}; "
            "rebase onto an earlier dev commit or rerun after the dev build "
            "for this SHA completes"
        )

    artifact = matching[-1]
    for attempt in range(2):
        try:
            contents = _artifact_file(
                _download_artifact(
                    api_url,
                    token,
                    repository,
                    artifact["id"],
                    timeout=12,
                ),
                "Teagram",
            )
            break
        except (OSError, ScaleCheckError) as error:
            if attempt == 1:
                raise ScaleCheckError(
                    f"could not download the PR base Teagram artifact for SHA "
                    f"{base_sha} after one retry: {error}"
                ) from error

    directory = Path(binary_directory)
    directory.mkdir(parents=True, exist_ok=True)
    binary_path = directory / "Teagram"
    binary_path.write_bytes(contents)
    binary_path.chmod(0o755)
    return True


def fetch_baseline(
    api_url,
    token,
    repository,
    base_sha,
    records_output,
    binary_directory=None,
    require_binary=False,
):
    records, history_truncated, malformed_count, first_error = fetch_history(
        api_url, token, repository, records_output
    )

    binary_found = False
    if len(records) >= MIN_DEV_RUNS and binary_directory:
        binary_found = fetch_base_binary(
            api_url,
            token,
            repository,
            base_sha,
            binary_directory,
            require_binary,
        )
    if require_binary and len(records) >= MIN_DEV_RUNS and not binary_found:
        raise ScaleCheckError(
            f"could not retrieve the PR base Teagram artifact for SHA {base_sha}; "
            "rebase onto an earlier dev commit or rerun after the dev build "
            "for this SHA completes"
        )
    return records, binary_found, history_truncated, malformed_count, first_error


def _write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


_MISSING = object()


def _load_json(path, default=_MISSING):
    try:
        return json.loads(Path(path).read_text())
    except FileNotFoundError:
        if default is _MISSING:
            raise ScaleCheckError(f"required JSON file is missing: {path}")
        return default


def _cmd_fetch(args):
    if args.history_only:
        records, history_truncated, malformed_count, first_error = fetch_history(
            args.api_url,
            args.token,
            args.repository,
            args.records_output,
        )
        binary_found = False
    else:
        (
            records,
            binary_found,
            history_truncated,
            malformed_count,
            first_error,
        ) = fetch_baseline(
            args.api_url,
            args.token,
            args.repository,
            args.base_sha,
            args.records_output,
            args.binary_directory,
            args.require_binary,
        )
    print(f"record_count={len(records)}")
    print(f"binary_available={'true' if binary_found else 'false'}")
    print(f"history_truncated={'true' if history_truncated else 'false'}")
    print(f"history_malformed_count={malformed_count}")
    safe_first_error = (first_error or "").replace("\r", " ").replace("\n", " ")
    print(f"history_first_error={safe_first_error}")


def _cmd_fetch_binary(args):
    fetch_base_binary(
        args.api_url,
        args.token,
        args.repository,
        args.base_sha,
        args.binary_directory,
    )
    print("binary_available=true")


def _cmd_parse(args):
    sample = parse_report(Path(args.report).read_text())
    now = args.created_at or datetime.now(timezone.utc).isoformat()
    sample.update(
        {
            "run_id": args.run_id,
            "run_attempt": args.run_attempt,
            "created_at": now,
            "commit": args.commit,
        }
    )
    _write_json(args.output, sample)
    print(f"recorded={'true' if sample['valid'] else 'false'}")
    return 0 if sample["valid"] else 3


def _cmd_evaluate(args):
    history = _load_json(args.history, [])
    base_runs = [_load_json(path) for path in args.base]
    head_runs = [_load_json(path) for path in args.head]
    absolute_budget_head_runs = (
        [_load_json(path) for path in args.absolute_budget_head]
        if args.absolute_budget_head
        else None
    )
    if args.include_head_in_history:
        history = [
            *history,
            *(
                sample
                for sample in head_runs
                if not args.report_only or _is_valid_record(sample)
            ),
        ]
    result = evaluate(
        history,
        base_runs,
        head_runs,
        args.report_only,
        args.history_truncated,
        args.history_unavailable,
        absolute_budget_head_runs,
        args.history_malformed_count,
        args.history_first_error,
    )
    _write_json(args.output, result)
    if args.summary_output:
        Path(args.summary_output).write_text(
            format_summary(result, head_runs[-1] if head_runs else None, args.retry_note)
        )
    if result["failures"]:
        return 1
    return 0


def _cmd_annotate(args):
    result = _load_json(args.evaluation, {})
    for annotation in format_annotations(result):
        print(annotation)
    for failure in result.get("failures", []):
        print(f"::error::{failure}")


def _build_parser():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)

    fetch = commands.add_parser("fetch")
    fetch.add_argument("--api-url", required=True)
    fetch.add_argument("--token", required=True)
    fetch.add_argument("--repository", required=True)
    fetch.add_argument("--base-sha", default="")
    fetch.add_argument("--records-output", required=True)
    fetch.add_argument("--binary-directory")
    fetch.add_argument("--require-binary", action="store_true")
    fetch.add_argument("--history-only", action="store_true")
    fetch.set_defaults(function=_cmd_fetch)

    fetch_binary = commands.add_parser("fetch-binary")
    fetch_binary.add_argument("--api-url", required=True)
    fetch_binary.add_argument("--token", required=True)
    fetch_binary.add_argument("--repository", required=True)
    fetch_binary.add_argument("--base-sha", required=True)
    fetch_binary.add_argument("--binary-directory", required=True)
    fetch_binary.set_defaults(function=_cmd_fetch_binary)

    parse = commands.add_parser("parse")
    parse.add_argument("--report", required=True)
    parse.add_argument("--output", required=True)
    parse.add_argument("--run-id", default="")
    parse.add_argument("--run-attempt", default="")
    parse.add_argument("--commit", default="")
    parse.add_argument("--created-at", default="")
    parse.set_defaults(function=_cmd_parse)

    compare = commands.add_parser("evaluate")
    compare.add_argument("--history", required=True)
    compare.add_argument("--base", action="append", default=[])
    compare.add_argument("--head", action="append", default=[])
    compare.add_argument("--absolute-budget-head", action="append", default=[])
    compare.add_argument("--output", required=True)
    compare.add_argument("--summary-output")
    compare.add_argument("--retry-note")
    compare.add_argument("--report-only", action="store_true")
    compare.add_argument("--include-head-in-history", action="store_true")
    compare.add_argument("--history-truncated", action="store_true")
    compare.add_argument("--history-unavailable", action="store_true")
    compare.add_argument("--history-malformed-count", type=int, default=0)
    compare.add_argument("--history-first-error")
    compare.set_defaults(function=_cmd_evaluate)

    annotate = commands.add_parser("annotate")
    annotate.add_argument("--evaluation", required=True)
    annotate.set_defaults(function=_cmd_annotate)
    return parser


def main(argv=None):
    parser = _build_parser()
    args = parser.parse_args(argv)
    try:
        return args.function(args) or 0
    except (OSError, ScaleCheckError, HTTPError) as error:
        print(f"::error::Desktop scale check: {error}", file=sys.stderr)
        return 2
    except Exception as error:
        print(
            "::error::Unexpected desktop scale check error "
            f"({type(error).__name__}): {error}\n{traceback.format_exc()}",
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    sys.exit(main())
