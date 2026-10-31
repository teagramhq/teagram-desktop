#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
temporary_dir="$(mktemp -d)"
trap 'rm -rf -- "$temporary_dir"' EXIT

message_file="$temporary_dir/commit-message"
printf '%s\n' 'fix: mention MAIN-3030' > "$message_file"
if "$repo_root/.githooks/commit-msg" "$message_file" >/dev/null 2>&1; then
	printf '%s\n' 'A normal commit with an issue key must be rejected.' >&2
	exit 1
fi

printf '%s\n' 'fix: handle DOMAIN-123 identifiers' > "$message_file"
"$repo_root/.githooks/commit-msg" "$message_file"

subjects=(
	"Merge remote-tracking branch 'origin/main' into fix/MAIN-3030"
	'Revert "old commit"'
	'fixup! fix: original commit'
	'squash! fix: original commit'
	'amend! fix: original commit'
)
for subject in "${subjects[@]}"; do
	printf '%s\n' "$subject" > "$message_file"
	"$repo_root/.githooks/commit-msg" "$message_file"
done

test_repo="$temporary_dir/repo"
git init --quiet "$test_repo"
git -C "$test_repo" config user.name 'Hook Regression'
git -C "$test_repo" config user.email 'hook-regression@example.invalid'
git -C "$test_repo" config core.hooksPath "$repo_root/.githooks"
printf '%s\n' base > "$test_repo/base.txt"
git -C "$test_repo" add base.txt
git -C "$test_repo" commit --quiet -m 'chore: add base'
base_commit="$(git -C "$test_repo" rev-parse HEAD)"
git -C "$test_repo" branch -M main

printf '%s\n' main > "$test_repo/main.txt"
git -C "$test_repo" add main.txt
git -C "$test_repo" commit --quiet -m 'fix: update main'
git -C "$test_repo" checkout --quiet -b feature "$base_commit"
printf '%s\n' feature > "$test_repo/feature.txt"
git -C "$test_repo" add feature.txt
git -C "$test_repo" commit --quiet -m 'fix: update feature'
git -C "$test_repo" merge --no-commit --no-ff main
printf '%s\n' 'not conventional MAIN-3030' > "$message_file"
if ! (cd "$test_repo" && "$repo_root/.githooks/commit-msg" "$message_file"); then
	printf '%s\n' 'MERGE_HEAD must exempt the commit message hook.' >&2
	exit 1
fi
git -C "$test_repo" merge --abort
git -C "$test_repo" merge --no-edit --no-ff main

parent_count="$(git -C "$test_repo" rev-list --parents -n 1 HEAD | awk '{ print NF - 1 }')"
if [[ "$parent_count" != 2 ]]; then
	printf '%s\n' 'The diverged main branch was not merged into feature.' >&2
	exit 1
fi

clang_format="$(command -v clang-format || true)"
git_clang_format="$(command -v git-clang-format || true)"
python3_bin="$(command -v python3 || true)"
required_version="$(sed -n -E 's/^[[:space:]]*-[[:space:]]*name:[[:space:]]*Install clang-format ([0-9.]+)\.$/\1/p' "$repo_root/.github/workflows/cpp_format.yml" | head -n 1)"
if [[ -z "$clang_format" || -z "$git_clang_format" || -z "$python3_bin" ]]; then
	printf '%s\n' 'The pre-commit regression requires clang-format, git-clang-format, and python3.' >&2
	exit 1
fi
actual_version="$("$clang_format" --version 2>/dev/null || true)"
if [[ -z "$required_version" || "$actual_version" != "clang-format version $required_version" ]]; then
	printf 'The pre-commit regression requires clang-format %s (found %s).\n' \
		"${required_version:-from .github/workflows/cpp_format.yml}" \
		"${actual_version:-unavailable}" >&2
	exit 1
fi

format_repo="$temporary_dir/format-repo"
git init --quiet "$format_repo"
git -C "$format_repo" config user.name 'Hook Regression'
git -C "$format_repo" config user.email 'hook-regression@example.invalid'
git -C "$format_repo" config core.hooksPath "$repo_root/.githooks"
mkdir -p "$format_repo/.github/workflows" "$format_repo/.github/scripts"
cp "$repo_root/.github/workflows/cpp_format.yml" "$format_repo/.github/workflows/"
cp "$repo_root/.github/scripts/check_cpp_format.py" "$format_repo/.github/scripts/"
git -C "$format_repo" add .github/workflows/cpp_format.yml .github/scripts/check_cpp_format.py
git -C "$format_repo" commit --quiet -m 'chore: add format check'
format_base="$(git -C "$format_repo" rev-parse HEAD)"

printf '%s\n' 'int add(int left,int right){return left+right;}' > "$format_repo/format.cpp"
git -C "$format_repo" add format.cpp
git -C "$format_repo" commit --quiet -m 'test: exercise staged C++ formatting'
format_head="$(git -C "$format_repo" rev-parse HEAD)"
"$python3_bin" "$repo_root/.github/scripts/check_cpp_format.py" \
	--repo-root "$format_repo" \
	--formatter "$clang_format" \
	--tested-head "$format_head" \
	--source-range pre-commit "$format_base" "$format_head"

printf '%s\n' 'commit-msg and staged C++ pre-commit regressions passed.'
