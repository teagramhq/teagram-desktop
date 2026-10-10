#!/usr/bin/env python3
"""Check clang-format on changed C/C++ source lines from an explicit revision range."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Iterable


SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm")
FORMAT_STYLE = (
    "{BasedOnStyle: LLVM, UseTab: Always, IndentWidth: 4, TabWidth: 4, "
    "ContinuationIndentWidth: 4, ColumnLimit: 80, SortIncludes: false, "
    "BreakBeforeBinaryOperators: All, NamespaceIndentation: None}"
)


def parse_changed_ranges(diff: str) -> tuple[dict[str, list[tuple[int, int]]], dict[str, int]]:
    ranges: dict[str, list[tuple[int, int]]] = defaultdict(list)
    changed_lines: dict[str, int] = defaultdict(int)
    current_path: str | None = None
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            current_path = line[6:]
            if not current_path.endswith(SOURCE_SUFFIXES):
                current_path = None
        elif line.startswith("@@") and current_path:
            match = re.search(r"\+(\d+)(?:,(\d+))?", line)
            if match:
                first = int(match.group(1))
                count = int(match.group(2) or "1")
                if count:
                    ranges[current_path].append((first, first + count - 1))
                    changed_lines[current_path] += count
    return dict(ranges), dict(changed_lines)


def merge_ranges(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for first, last in sorted(ranges):
        if merged and first <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], last)
        else:
            merged.append([first, last])
    return [(first, last) for first, last in merged]


def format_file(formatter: str, path: Path, ranges: list[tuple[int, int]]) -> subprocess.CompletedProcess[str]:
    command = [formatter, "--dry-run", "--Werror", f"--style={FORMAT_STYLE}"]
    command.extend(f"--lines={first}:{last}" for first, last in merge_ranges(ranges))
    command.append(str(path))
    return subprocess.run(command, text=True, capture_output=True, check=False)


def self_test(formatter: str) -> int:
    fixture_diff = (
        "diff --git a/formatter-fixture.cpp b/formatter-fixture.cpp\n"
        "--- a/formatter-fixture.cpp\n"
        "+++ b/formatter-fixture.cpp\n"
        "@@ -1 +1 @@\n"
        "+int answer = 42;\n"
    )
    ranges, changed_lines = parse_changed_ranges(fixture_diff)
    if ranges != {"formatter-fixture.cpp": [(1, 1)]} or changed_lines != {"formatter-fixture.cpp": 1}:
        print("::error::Formatter self-test could not identify the synthetic changed C++ line.")
        return 1

    with tempfile.TemporaryDirectory(prefix="teagram-format-self-test-") as temporary:
        root = Path(temporary)
        compliant = root / "formatter-fixture.cpp"
        compliant.write_text("int answer = 42;\n", encoding="utf-8")
        passed = format_file(formatter, compliant, ranges["formatter-fixture.cpp"])
        if passed.returncode:
            detail = (passed.stderr or passed.stdout).strip()
            print(f"::error::Compliant formatter fixture failed: {detail or 'clang-format returned nonzero.'}")
            return 1
        print("Formatter self-test: compliant synthetic C++ fixture passed.")

        violation = root / "formatter-fixture.cpp"
        violation.write_text("int answer=42;\n", encoding="utf-8")
        rejected = format_file(formatter, violation, ranges["formatter-fixture.cpp"])
        if rejected.returncode == 0:
            print("::error::Formatter self-test accepted a deliberately misformatted changed C++ line.")
            return 1
        detail = (rejected.stderr or rejected.stdout).strip()
        if not detail:
            print("::error::Formatter rejected the negative fixture without an actionable diagnostic.")
            return 1
        print(f"Formatter self-test: malformed synthetic C++ fixture rejected: {detail}")
    return 0


def resolve_commit(root: Path, revision: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--verify", f"{revision}^{{commit}}"],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(f"cannot resolve revision {revision!r}: {result.stderr.strip()}")
    return result.stdout.strip()


def check_source_range(
    root: Path,
    formatter: str,
    tested_head: str,
    name: str,
    base: str,
    head: str,
) -> dict[str, object]:
    resolved_base = resolve_commit(root, base)
    resolved_head = resolve_commit(root, head)
    base_is_ancestor = subprocess.run(
        ["git", "-C", str(root), "merge-base", "--is-ancestor", resolved_base, resolved_head],
        check=False,
    )
    if base_is_ancestor.returncode:
        raise RuntimeError(f"declared source base {resolved_base} is not an ancestor of range head {resolved_head}")
    head_is_ancestor = subprocess.run(
        ["git", "-C", str(root), "merge-base", "--is-ancestor", resolved_head, tested_head],
        check=False,
    )
    if head_is_ancestor.returncode:
        raise RuntimeError(f"source range head {resolved_head} is not an ancestor of tested SHA {tested_head}")

    diff = subprocess.run(
        [
            "git", "-C", str(root), "diff", "--no-ext-diff", "--no-color", "--unified=0",
            "--diff-filter=ACMRT", resolved_base, resolved_head, "--",
            "*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hh", "*.hpp", "*.hxx", "*.m", "*.mm",
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    if diff.returncode:
        raise RuntimeError(f"git diff failed: {diff.stderr.strip()}")

    ranges, changed_lines = parse_changed_ranges(diff.stdout)
    total_changed_lines = sum(changed_lines.values())
    manifest: dict[str, object] = {
        "name": name,
        "declared_source_range": {"base": base, "head": head},
        "resolved_source_range": {"base": resolved_base, "head": resolved_head},
        "checked_source_count": len(ranges),
        "checked_changed_line_count": total_changed_lines,
        "files": [
            {
                "path": path,
                "changed_line_count": changed_lines.get(path, 0),
                "ranges": [{"first": first, "last": last} for first, last in merge_ranges(path_ranges)],
            }
            for path, path_ranges in sorted(ranges.items())
        ],
        "result": "pending",
    }
    if not ranges or not total_changed_lines:
        error = (
            f"source range {name} ({resolved_base}..{resolved_head}) contains no changed C/C++ source lines; "
            "refusing zero-coverage pass"
        )
        manifest.update({"result": "failure", "error": error})
        print(f"::error::{error}")
        return manifest

    errors = []
    with tempfile.TemporaryDirectory(prefix=f"teagram-format-{name}-") as temporary:
        snapshot = Path(temporary)
        for relative, path_ranges in sorted(ranges.items()):
            source = snapshot / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            content = subprocess.run(
                ["git", "-C", str(root), "show", f"{resolved_head}:{relative}"],
                capture_output=True,
                check=False,
            )
            if content.returncode:
                error = f"Changed source file is missing at range head {resolved_head}: {relative}"
                print(f"::error::{error}")
                errors.append(error)
                continue
            source.write_bytes(content.stdout)
            print(f"Checking {name}:{relative} ({changed_lines[relative]} changed lines)")
            result = format_file(formatter, source, path_ranges)
            detail = (result.stdout + result.stderr).replace(str(source), f"{name}:{relative}")
            if detail:
                print(detail, end="", file=sys.stderr if result.stderr else sys.stdout)
            if result.returncode:
                error = f"clang-format rejected changed lines in {name}:{relative} (exit {result.returncode})"
                print(f"::error::{error}.")
                errors.append(error)
    manifest["result"] = "failure" if errors else "success"
    if errors:
        manifest["errors"] = errors
        print("::error::clang-format rejected one or more changed source ranges.")
        return manifest
    print(
        f"Checked {len(ranges)} changed source files and {total_changed_lines} changed lines "
        f"in {name} ({resolved_base}..{resolved_head})."
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path("."))
    parser.add_argument("--formatter", default="clang-format")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--tested-head", default="HEAD")
    parser.add_argument(
        "--source-range",
        nargs=3,
        action="append",
        metavar=("NAME", "BASE", "HEAD"),
        help="declared C/C++ diff range to check; may be repeated",
    )
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()

    formatter = shutil.which(args.formatter)
    if not formatter:
        print(f"::error::Formatter executable not found: {args.formatter}")
        return 1
    version = subprocess.run([formatter, "--version"], text=True, capture_output=True, check=False)
    version_text = (version.stdout or version.stderr).strip()
    print(f"Formatter: {version_text}")
    if version.returncode:
        print("::error::Could not read clang-format version.")
        return 1
    if args.self_test:
        return self_test(formatter)

    manifest: dict[str, object] = {
        "declared_tested_head": args.tested_head,
        "formatter": version_text,
        "resolved_tested_head": None,
        "checked_out_head": None,
        "checked_source_count": 0,
        "checked_source_range_count": 0,
        "checked_changed_line_count": 0,
        "source_ranges": [],
        "result": "failure",
    }
    status = 1
    try:
        root = args.repo_root.resolve()
        tested_head = resolve_commit(root, args.tested_head)
        checked_out_head = resolve_commit(root, "HEAD")
        manifest["resolved_tested_head"] = tested_head
        manifest["checked_out_head"] = checked_out_head
        if tested_head != checked_out_head:
            raise RuntimeError(f"checked-out HEAD {checked_out_head} does not match tested SHA {tested_head}")
        if not args.source_range:
            raise RuntimeError("at least one explicit --source-range is required")

        source_ranges = []
        seen_names = set()
        errors = []
        for name, base, head in args.source_range:
            if name in seen_names:
                errors.append(f"duplicate source range name: {name}")
                continue
            seen_names.add(name)
            try:
                result = check_source_range(root, formatter, tested_head, name, base, head)
            except Exception as error:
                result = {
                    "name": name,
                    "declared_source_range": {"base": base, "head": head},
                    "checked_source_count": 0,
                    "checked_changed_line_count": 0,
                    "files": [],
                    "result": "failure",
                    "error": str(error),
                }
                print(f"::error::{name}: {error}")
            source_ranges.append(result)
            if result.get("result") != "success":
                errors.append(str(result.get("error", f"{name} source-format check failed")))
        total_source_count = len({
            entry["path"]
            for source_range in source_ranges
            for entry in source_range.get("files", [])
        })
        total_source_range_count = sum(
            int(source_range.get("checked_source_count", 0)) for source_range in source_ranges
        )
        total_changed_lines = sum(int(entry.get("checked_changed_line_count", 0)) for entry in source_ranges)
        manifest.update({
            "source_ranges": source_ranges,
            "checked_source_count": total_source_count,
            "checked_source_range_count": total_source_range_count,
            "checked_changed_line_count": total_changed_lines,
            "result": "failure" if errors else "success",
        })
        if errors:
            manifest["errors"] = errors
        else:
            status = 0
    except Exception as error:
        manifest["error"] = str(error)
        print(f"::error::{error}")
    finally:
        if args.manifest:
            args.manifest.parent.mkdir(parents=True, exist_ok=True)
            args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            print(f"Formatter coverage manifest: {args.manifest}")
    return status


if __name__ == "__main__":
    sys.exit(main())
