#!/usr/bin/env python3
"""Check tested-head C/C++ contents within source paths selected by revision ranges."""

from __future__ import annotations

import argparse
import contextlib
import difflib
import io
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
SOURCE_PATHS = tuple(f"*{suffix}" for suffix in SOURCE_SUFFIXES)
FORMAT_STYLE = (
    "{BasedOnStyle: LLVM, UseTab: Always, IndentWidth: 4, TabWidth: 4, "
    "ContinuationIndentWidth: 4, ColumnLimit: 80, SortIncludes: false, "
    "BreakBeforeBinaryOperators: All, NamespaceIndentation: None}"
)
CHERRY_PICK_TRAILER = re.compile(
    r"\(cherry picked from commit ([0-9a-f]{40})\)"
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


def git_output(root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(arguments)} failed")
    return result.stdout


def parse_added_lines(diff: str) -> dict[str, list[tuple[int, str]]]:
    added: dict[str, list[tuple[int, str]]] = defaultdict(list)
    current_path: str | None = None
    next_line: int | None = None
    for line in diff.splitlines():
        if line.startswith("diff --git "):
            current_path = None
            next_line = None
        elif line.startswith("+++ b/") and next_line is None:
            current_path = line[6:]
            if not current_path.endswith(SOURCE_SUFFIXES):
                current_path = None
        elif line.startswith("@@"):
            match = re.search(r"\+(\d+)(?:,(\d+))?", line)
            next_line = int(match.group(1)) if match else None
        elif current_path and next_line is not None:
            if line.startswith("+"):
                added[current_path].append((next_line, line[1:]))
                next_line += 1
            elif line.startswith(" "):
                next_line += 1
    return dict(added)


def added_lines_between(root: Path, base: str, head: str) -> dict[str, list[tuple[int, str]]]:
    diff = git_output(
        root,
        "diff",
        "--no-ext-diff",
        "--no-color",
        "--unified=0",
        "--diff-filter=ACMRT",
        base,
        head,
        "--",
        *SOURCE_PATHS,
    )
    return parse_added_lines(diff)


def renamed_paths_between(root: Path, base: str, head: str) -> dict[str, str]:
    fields = git_output(
        root,
        "diff",
        "--name-status",
        "--find-renames",
        "-z",
        "--no-ext-diff",
        "--no-color",
        base,
        head,
        "--",
        *SOURCE_PATHS,
    ).split("\0")
    renames: dict[str, str] = {}
    index = 0
    while index < len(fields):
        status = fields[index]
        index += 1
        if not status:
            continue
        if status.startswith(("R", "C")):
            if index + 1 >= len(fields):
                raise RuntimeError("git returned an incomplete rename record")
            source_path, picked_path = fields[index:index + 2]
            renames[source_path] = picked_path
            index += 2
        else:
            index += 1
    return renames


def cherry_pick_sources(message: str) -> list[str]:
    lines = message.rstrip().splitlines()
    start = len(lines)
    while start and lines[start - 1].strip():
        start -= 1
    return [
        match.group(1)
        for line in lines[start:]
        if (match := CHERRY_PICK_TRAILER.fullmatch(line))
    ]


def upstream_pick_lines(
    root: Path,
    base: str,
    tested_head: str,
    upstream_commits: set[str],
) -> dict[str, dict[str, set[int]]]:
    commits = git_output(
        root,
        "log",
        "--reverse",
        "--format=%H",
        "--grep=cherry picked from commit",
        f"{base}..{tested_head}",
    ).split()
    exempt: dict[str, dict[str, set[int]]] = defaultdict(lambda: defaultdict(set))
    for commit in commits:
        if commit in upstream_commits:
            continue
        message = git_output(root, "show", "-s", "--format=%B", commit)
        trailers = cherry_pick_sources(message)
        if len(trailers) != 1 or trailers[0] not in upstream_commits:
            continue
        parents = git_output(root, "rev-list", "--parents", "-n", "1", commit).split()[1:]
        if len(parents) != 1:
            continue
        source = trailers[0]
        source_parents = git_output(root, "rev-list", "--parents", "-n", "1", source).split()[1:]
        if not source_parents:
            continue
        source_parent = source_parents[0]
        source_added = added_lines_between(root, source_parent, source)
        picked_added = added_lines_between(root, parents[0], commit)
        source_renames = renamed_paths_between(root, source_parent, source)
        source_parent_paths = {new: old for old, new in source_renames.items()}
        picked_renames = renamed_paths_between(root, source_parent, parents[0])
        for source_path, source_lines in source_added.items():
            source_parent_path = source_parent_paths.get(source_path, source_path)
            picked_path = picked_renames.get(source_parent_path, source_parent_path)
            picked_lines = picked_added.get(picked_path)
            if picked_lines is None:
                continue
            picked_text = [line for _, line in picked_lines]
            matcher = difflib.SequenceMatcher(
                None,
                [line for _, line in source_lines],
                picked_text,
                autojunk=False,
            )
            for _, picked_index, count in matcher.get_matching_blocks():
                if not count:
                    continue
                for line_number, _ in picked_lines[picked_index:picked_index + count]:
                    exempt[commit][picked_path].add(line_number)
    return {commit: dict(paths) for commit, paths in exempt.items()}


def blame_line_origins(root: Path, revision: str, path: str) -> dict[int, tuple[str, int, str]]:
    output = git_output(root, "blame", "--line-porcelain", "--no-progress", revision, "--", path)
    origins: dict[int, tuple[str, int, str]] = {}
    header = re.compile(r"^(\^?[0-9a-f]{40}) (\d+) (\d+)(?: \d+)?$")
    current_line: tuple[int, str, int] | None = None
    for line in output.splitlines():
        match = header.match(line)
        if match:
            current_line = (
                int(match.group(3)),
                match.group(1).lstrip("^"),
                int(match.group(2)),
            )
        elif current_line is not None and line.startswith("filename "):
            final_line, commit, original_line = current_line
            origins[final_line] = (commit, original_line, line[len("filename "):])
            current_line = None
    return origins


def ranges_without_upstream_lines(
    root: Path,
    tested_head: str,
    path: str,
    ranges: list[tuple[int, int]],
    upstream_commits: set[str],
    picked_lines: dict[str, dict[str, set[int]]],
) -> tuple[list[tuple[int, int]], int]:
    origins = blame_line_origins(root, tested_head, path)
    checked: list[int] = []
    for first, last in ranges:
        for line_number in range(first, last + 1):
            commit, original_line, original_path = origins.get(line_number, ("", 0, ""))
            if commit in upstream_commits:
                continue
            if original_line in picked_lines.get(commit, {}).get(original_path, set()):
                continue
            checked.append(line_number)
    return merge_ranges((line, line) for line in checked), len(checked)


def upstream_history(root: Path, refs: list[str]) -> set[str]:
    if not refs:
        raise RuntimeError("at least one verified upstream history ref is required")
    resolved_refs = [resolve_commit(root, ref) for ref in refs]
    commits = git_output(root, "rev-list", *resolved_refs).split()
    if not commits:
        raise RuntimeError("verified upstream refs contain no commits")
    return set(commits)


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
    try:
        history_test(formatter)
    except Exception as error:
        print(f"::error::Synthetic-history formatter regression failed: {error}")
        return 1
    try:
        upstream_provenance_test(formatter)
    except Exception as error:
        print(f"::error::Synthetic-upstream formatter regression failed: {error}")
        return 1
    return 0


def history_test(formatter: str) -> None:
    with tempfile.TemporaryDirectory(prefix="teagram-format-history-test-") as temporary:
        root = Path(temporary)

        def git(*arguments: str) -> str:
            result = subprocess.run(
                ["git", "-C", str(root), *arguments],
                text=True,
                capture_output=True,
                check=False,
            )
            if result.returncode:
                raise RuntimeError(result.stderr.strip() or f"git {' '.join(arguments)} failed")
            return result.stdout.strip()

        git("init", "--quiet")
        git("config", "user.name", "Formatter Regression")
        git("config", "user.email", "formatter-regression@example.invalid")

        profile = root / "profile.cpp"
        profile.write_text("int initialize_profile() { return 1; }\n", encoding="utf-8")
        git("add", "profile.cpp")
        git("commit", "--quiet", "-m", "base")
        base = git("rev-parse", "HEAD")

        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            empty_result = check_source_range(
                root,
                formatter,
                base,
                "empty-range",
                base,
                base,
                allow_empty=True,
            )
            strict_empty_result = check_source_range(
                root,
                formatter,
                base,
                "strict-empty-range",
                base,
                base,
            )
        if empty_result.get("result") != "success":
            raise RuntimeError(f"an empty C/C++ source range did not pass when allowed: {empty_result}")
        if strict_empty_result.get("result") != "failure":
            raise RuntimeError(
                "an empty C/C++ source range passed without explicit permission: "
                f"{strict_empty_result}"
            )

        profile.write_text("int initialize_profile() { return 2; }\n", encoding="utf-8")
        git("add", "profile.cpp")
        git("commit", "--quiet", "-m", "merged source")
        merged = git("rev-parse", "HEAD")

        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            merged_result = check_source_range(root, formatter, merged, "merged-profile-ipc", base, merged)
        if merged_result.get("result") != "success":
            raise RuntimeError(f"compliant merged-source history did not pass: {merged_result}")

        profile.write_text("int initialize_profile() { return    3; }\n", encoding="utf-8")
        git("add", "profile.cpp")
        git("commit", "--quiet", "-m", "intervening profile change")
        repair_base = git("rev-parse", "HEAD")
        git("merge-base", "--is-ancestor", merged, repair_base)
        if repair_base == merged:
            raise RuntimeError("synthetic formatting violation was not introduced after the merge")

        repair = root / "repair.cpp"
        repair.write_text("int repair_lifecycle() { return 4; }\n", encoding="utf-8")
        git("add", "repair.cpp")
        git("commit", "--quiet", "-m", "lifecycle repair")
        tested_head = git("rev-parse", "HEAD")
        git("merge-base", "--is-ancestor", repair_base, tested_head)

        allow_empty_result = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "--repo-root",
                str(root),
                "--formatter",
                formatter,
                "--allow-empty",
                "--tested-head",
                tested_head,
                "--source-range",
                "empty-range",
                tested_head,
                tested_head,
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if allow_empty_result.returncode:
            detail = (allow_empty_result.stdout + allow_empty_result.stderr).strip()
            raise RuntimeError(
                "--allow-empty should accept a declared range without C++ changes: "
                f"{detail}"
            )

        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            merged_result = check_source_range(
                root,
                formatter,
                tested_head,
                "merged-profile-ipc",
                base,
                merged,
            )
        expected_error = "clang-format rejected changed lines in merged-profile-ipc:profile.cpp"
        if (
            merged_result.get("result") != "failure"
            or merged_result.get("checked_source_count") != 1
            or merged_result.get("checked_changed_line_count", 0) < 1
            or not any(error.startswith(expected_error) for error in merged_result.get("errors", []))
        ):
            raise RuntimeError(
                "a formatting violation introduced after merge and before the repair base "
                f"was not rejected from tested-head contents: {merged_result}"
            )
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            repair_result = check_source_range(
                root,
                formatter,
                tested_head,
                "lifecycle-repair",
                repair_base,
                tested_head,
            )
        if repair_result.get("result") != "success":
            raise RuntimeError(f"compliant repair-source history did not pass: {repair_result}")
    print("Formatter self-test: intervening merged-source violation rejected at tested head.")


def upstream_provenance_test(formatter: str) -> None:
    with tempfile.TemporaryDirectory(prefix="teagram-format-upstream-test-") as temporary:
        root = Path(temporary)

        def git(*arguments: str) -> str:
            return git_output(root, *arguments).strip()

        def write_file(content: str) -> None:
            (root / "fixture.cpp").write_text(content, encoding="utf-8")

        def commit(message: str) -> str:
            git("add", "fixture.cpp")
            git("commit", "--quiet", "-m", message)
            return git("rev-parse", "HEAD")

        def upstream_refs() -> list[str]:
            refs = git("for-each-ref", "--format=%(refname)", "refs/upstream/telegramdesktop")
            return refs.splitlines() if refs else []

        def check(
            name: str,
            base: str,
            head: str,
            should_pass: bool,
            expected_lines: int,
            expected_exempted_lines: int,
            expected_ranges: list[tuple[str, int, int]],
        ) -> None:
            upstream_commits = upstream_history(root, upstream_refs())
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                result = check_source_range(
                    root,
                    formatter,
                    head,
                    name,
                    base,
                    head,
                    allow_empty=True,
                    upstream_commits=upstream_commits,
                )
            if result.get("result") != ("success" if should_pass else "failure"):
                raise RuntimeError(f"{name} had an unexpected formatter result: {result}")
            if result.get("checked_changed_line_count") != expected_lines:
                raise RuntimeError(
                    f"{name} checked {result.get('checked_changed_line_count')} lines, "
                    f"expected {expected_lines}: {result}"
                )
            if result.get("exempted_upstream_line_count") != expected_exempted_lines:
                raise RuntimeError(
                    f"{name} exempted {result.get('exempted_upstream_line_count')} upstream lines, "
                    f"expected {expected_exempted_lines}: {result}"
                )
            actual_ranges = [
                (entry["path"], changed_range["first"], changed_range["last"])
                for entry in result.get("files", [])
                for changed_range in entry.get("ranges", [])
            ]
            if actual_ranges != expected_ranges:
                raise RuntimeError(f"{name} checked ranges {actual_ranges}, expected {expected_ranges}: {result}")

        git("init", "--quiet")
        git("config", "user.name", "Formatter Regression")
        git("config", "user.email", "formatter-regression@example.invalid")
        write_file("int base = 0;\n")
        base = commit("base")
        git("branch", "-M", "main")

        git("checkout", "-b", "upstream-pick")
        write_file("int base = 0;\nint imported=1;\n")
        source = commit("upstream change")
        git("update-ref", "refs/upstream/telegramdesktop/heads/dev", source)

        git("checkout", "main")
        git("cherry-pick", "-x", source)
        picked = git("rev-parse", "HEAD")
        check(
            "upstream-pick",
            base,
            picked,
            should_pass=True,
            expected_lines=0,
            expected_exempted_lines=1,
            expected_ranges=[],
        )

        git("checkout", "-b", "pick-with-own-line", base)
        git("cherry-pick", "-x", source)
        write_file("int base = 0;\nint imported=1;\nint own=2;\n")
        git("add", "fixture.cpp")
        git("commit", "--quiet", "--amend", "--no-edit")
        pick_with_own = git("rev-parse", "HEAD")
        check(
            "upstream-pick-with-own-line",
            base,
            pick_with_own,
            should_pass=False,
            expected_lines=1,
            expected_exempted_lines=1,
            expected_ranges=[("fixture.cpp", 3, 3)],
        )

        git("checkout", "-b", "pick-with-formatted-own-line", base)
        git("cherry-pick", "-x", source)
        write_file("int base = 0;\nint imported=1;\nint own = 2;\n")
        git("add", "fixture.cpp")
        git("commit", "--quiet", "--amend", "--no-edit")
        formatted_pick = git("rev-parse", "HEAD")
        check(
            "upstream-pick-with-formatted-own-line",
            base,
            formatted_pick,
            should_pass=True,
            expected_lines=1,
            expected_exempted_lines=1,
            expected_ranges=[("fixture.cpp", 3, 3)],
        )

        git("checkout", "-b", "upstream-rename-source", base)
        git("mv", "fixture.cpp", "a.cpp")
        (root / "a.cpp").write_text(
            "int base = 0;\nint one = 1;\nint two = 2;\nint three = 3;\n",
            encoding="utf-8",
        )
        git("add", "a.cpp")
        git("commit", "--quiet", "-m", "prepare upstream source path")
        rename_base = git("rev-parse", "HEAD")
        (root / "a.cpp").write_text(
            "int base = 0;\nint one = 1;\nint two = 2;\nint three = 3;\nint imported=4;\n",
            encoding="utf-8",
        )
        git("add", "a.cpp")
        git("commit", "--quiet", "-m", "upstream change in a.cpp")
        rename_source = git("rev-parse", "HEAD")
        git("update-ref", "refs/upstream/telegramdesktop/heads/rename-dev", rename_source)

        git("checkout", "-b", "pick-then-rename", rename_base)
        git("cherry-pick", "-x", rename_source)
        git("mv", "a.cpp", "b.cpp")
        git("commit", "--quiet", "-m", "rename picked source")
        renamed_pick = git("rev-parse", "HEAD")
        check(
            "upstream-pick-then-rename",
            rename_base,
            renamed_pick,
            should_pass=True,
            expected_lines=0,
            expected_exempted_lines=1,
            expected_ranges=[],
        )

        git("checkout", "-b", "upstream-renamed-pick-source", base)
        git("mv", "fixture.cpp", "a.cpp")
        (root / "a.cpp").write_text(
            "int base = 0;\nint one = 1;\nint two = 2;\nint three = 3;\n",
            encoding="utf-8",
        )
        (root / "other.cpp").write_text("int other = 0;\n", encoding="utf-8")
        git("add", "a.cpp", "other.cpp")
        git("commit", "--quiet", "-m", "prepare upstream a.cpp")
        rename_pick_base = git("rev-parse", "HEAD")
        (root / "a.cpp").write_text(
            "int base = 0;\nint one = 1;\nint two = 2;\nint three = 3;\nint imported=4;\n",
            encoding="utf-8",
        )
        git("add", "a.cpp")
        git("commit", "--quiet", "-m", "upstream addition to a.cpp")
        rename_pick_source = git("rev-parse", "HEAD")
        git("update-ref", "refs/upstream/telegramdesktop/heads/renamed-pick-dev", rename_pick_source)

        git("checkout", "-b", "rename-before-pick", rename_pick_base)
        git("mv", "a.cpp", "b.cpp")
        git("commit", "--quiet", "-m", "rename a.cpp before pick")
        rename_before_pick = git("rev-parse", "HEAD")
        git("cherry-pick", "-x", rename_pick_source)
        pick_after_rename = git("rev-parse", "HEAD")
        check(
            "upstream-pick-after-rename",
            rename_before_pick,
            pick_after_rename,
            should_pass=True,
            expected_lines=0,
            expected_exempted_lines=1,
            expected_ranges=[],
        )

        git("checkout", "-b", "upstream-pick-own-duplicate", rename_pick_base)
        git("cherry-pick", "-x", rename_pick_source)
        (root / "other.cpp").write_text(
            "int other = 0;\nint imported=4;\n",
            encoding="utf-8",
        )
        git("add", "other.cpp")
        git("commit", "--quiet", "--amend", "--no-edit")
        duplicate_pick = git("rev-parse", "HEAD")
        check(
            "upstream-pick-with-duplicate-in-unrelated-file",
            rename_pick_base,
            duplicate_pick,
            should_pass=False,
            expected_lines=1,
            expected_exempted_lines=1,
            expected_ranges=[("other.cpp", 2, 2)],
        )

        git("checkout", "-b", "non-upstream-source", base)
        write_file("int base = 0;\nint forged=3;\n")
        forged_source = commit("untrusted source")
        git("update-ref", "refs/tags/telegramdesktop/forged-source", forged_source)
        git("checkout", "-b", "forged-trailer", base)
        write_file("int base = 0;\nint forged=3;\n")
        git("add", "fixture.cpp")
        git(
            "commit",
            "--quiet",
            "-m",
            "forged upstream attribution",
            "-m",
            f"(cherry picked from commit {forged_source})",
        )
        forged = git("rev-parse", "HEAD")
        check(
            "forged-trailer",
            base,
            forged,
            should_pass=False,
            expected_lines=1,
            expected_exempted_lines=0,
            expected_ranges=[("fixture.cpp", 2, 2)],
        )

        git("checkout", "-b", "upstream-merge", base)
        write_file("int conflict=2;\nint imported=4;\n")
        merge_source = commit("upstream merge source")
        git("update-ref", "refs/upstream/telegramdesktop/heads/merge-dev", merge_source)

        git("checkout", "-b", "merge-with-own-changes", base)
        write_file("int conflict = 0;\nint own=5;\n")
        commit("own addition")
        write_file("int conflict=6;\nint own=5;\n")
        commit("own conflict side")
        merge = subprocess.run(
            ["git", "-C", str(root), "merge", "--no-ff", "upstream-merge"],
            text=True,
            capture_output=True,
            check=False,
        )
        if merge.returncode == 0 or "CONFLICT" not in merge.stdout + merge.stderr:
            raise RuntimeError("synthetic upstream merge did not produce the expected conflict")
        write_file("int conflict=7;\nint own=5;\nint imported=4;\n")
        git("add", "fixture.cpp")
        git("commit", "--quiet", "-m", "resolve upstream merge")
        merged = git("rev-parse", "HEAD")
        check(
            "upstream-merge",
            base,
            merged,
            should_pass=False,
            expected_lines=2,
            expected_exempted_lines=1,
            expected_ranges=[("fixture.cpp", 1, 2)],
        )
    print(
        "Formatter self-test: upstream pick exemption, own pick line, forged trailer, "
        "and upstream merge provenance passed."
    )


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
    allow_empty: bool = False,
    upstream_commits: set[str] | None = None,
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

    scope_diff = subprocess.run(
        [
            "git", "-C", str(root), "diff", "--no-ext-diff", "--no-color", "--unified=0",
            "--diff-filter=ACMRT", resolved_base, resolved_head, "--",
            *SOURCE_PATHS,
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    if scope_diff.returncode:
        raise RuntimeError(f"git diff failed: {scope_diff.stderr.strip()}")

    scope_ranges, _ = parse_changed_ranges(scope_diff.stdout)
    tested_diff = subprocess.run(
        [
            "git", "-C", str(root), "diff", "--no-ext-diff", "--no-color", "--unified=0",
            "--diff-filter=ACMRT", resolved_base, tested_head, "--",
            *SOURCE_PATHS,
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    if tested_diff.returncode:
        raise RuntimeError(f"git diff against tested SHA failed: {tested_diff.stderr.strip()}")

    tested_ranges, tested_changed_lines = parse_changed_ranges(tested_diff.stdout)
    ranges = {path: tested_ranges[path] for path in scope_ranges if path in tested_ranges}
    changed_lines = {path: tested_changed_lines[path] for path in ranges}
    exempted_upstream_line_count = 0
    if upstream_commits is not None:
        picked_lines = upstream_pick_lines(root, resolved_base, tested_head, upstream_commits)
        filtered_ranges = {}
        filtered_changed_lines = {}
        for path, path_ranges in ranges.items():
            checked_ranges, checked_lines = ranges_without_upstream_lines(
                root,
                tested_head,
                path,
                path_ranges,
                upstream_commits,
                picked_lines,
            )
            exempted_upstream_line_count += sum(last - first + 1 for first, last in path_ranges) - checked_lines
            if checked_ranges:
                filtered_ranges[path] = checked_ranges
                filtered_changed_lines[path] = checked_lines
        ranges = filtered_ranges
        changed_lines = filtered_changed_lines
    total_changed_lines = sum(changed_lines.values())
    manifest: dict[str, object] = {
        "name": name,
        "declared_source_range": {"base": base, "head": head},
        "resolved_source_range": {"base": resolved_base, "head": resolved_head},
        "coverage_range": {"base": resolved_base, "head": tested_head},
        "content_revision": tested_head,
        "scope_source_count": len(scope_ranges),
        "checked_source_count": len(ranges),
        "checked_changed_line_count": total_changed_lines,
        "exempted_upstream_line_count": exempted_upstream_line_count,
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
        if allow_empty:
            manifest["result"] = "success"
            if exempted_upstream_line_count:
                print(
                    f"Exempted {exempted_upstream_line_count} lines introduced by verified "
                    f"upstream history in {name}."
                )
            print(f"No changed C/C++ source lines in {name}; format check not needed.")
            return manifest
        error = (
            f"source range {name} ({resolved_base}..{resolved_head}) has no changed C/C++ source lines "
            f"at tested SHA {tested_head} in its declared source-file scope; refusing zero-coverage pass"
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
                ["git", "-C", str(root), "show", f"{tested_head}:{relative}"],
                capture_output=True,
                check=False,
            )
            if content.returncode:
                error = f"Changed source file is missing at tested SHA {tested_head}: {relative}"
                print(f"::error::{error}")
                errors.append(error)
                continue
            source.write_bytes(content.stdout)
            print(
                f"Checking {name}:{relative} at {tested_head} "
                f"({changed_lines[relative]} changed lines)"
            )
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
        f"in {name} ({resolved_base}..{tested_head}; scope anchored at {resolved_head})."
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path("."))
    parser.add_argument("--formatter", default="clang-format")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--allow-empty",
        action="store_true",
        help="pass when a source range has no added C/C++ lines",
    )
    parser.add_argument("--tested-head", default="HEAD")
    parser.add_argument(
        "--source-range",
        nargs=3,
        action="append",
        metavar=("NAME", "BASE", "HEAD"),
        help="revision range selecting C/C++ source paths; checked against tested-head contents; may be repeated",
    )
    parser.add_argument(
        "--upstream-ref",
        action="append",
        default=[],
        help="verified upstream history ref used to exempt unchanged upstream-introduced lines",
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
        "exempted_upstream_line_count": 0,
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
        upstream_commits = upstream_history(root, args.upstream_ref) if args.upstream_ref else None

        source_ranges = []
        seen_names = set()
        errors = []
        for name, base, head in args.source_range:
            if name in seen_names:
                errors.append(f"duplicate source range name: {name}")
                continue
            seen_names.add(name)
            try:
                result = check_source_range(
                    root,
                    formatter,
                    tested_head,
                    name,
                    base,
                    head,
                    allow_empty=args.allow_empty,
                    upstream_commits=upstream_commits,
                )
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
        total_exempted_upstream_lines = sum(
            int(source_range.get("exempted_upstream_line_count", 0)) for source_range in source_ranges
        )
        manifest.update({
            "source_ranges": source_ranges,
            "checked_source_count": total_source_count,
            "checked_source_range_count": total_source_range_count,
            "checked_changed_line_count": total_changed_lines,
            "exempted_upstream_line_count": total_exempted_upstream_lines,
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
