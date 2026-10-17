#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
seed_script="${scripts_dir}/ccache_seed_current.sh"
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT
repo="${temp_dir}/repo"

git init -q "$repo"
commit() {
  local path="$1"
  mkdir -p "$(dirname -- "${repo}/${path}")"
  printf '%s\n' "$RANDOM" >> "${repo}/${path}"
  git -C "$repo" add -- "$path"
  git -C "$repo" -c user.name=ci -c user.email=ci@example.invalid commit -q -m "$path"
  git -C "$repo" rev-parse HEAD
}

assert_current() {
  local built="$1"
  local tip="$2"
  local expected="current=$3"
  local message="$4"
  local actual
  actual="$(bash "$seed_script" "$repo" "$built" "$tip")"
  if [[ "$actual" != "$expected" ]]; then
    printf '%s Expected %s, got %s.\n' "$message" "$expected" "$actual" >&2
    exit 1
  fi
}

nightly_head="$(commit Telegram/SourceFiles/main.cpp)"
assert_current "$nightly_head" "$nightly_head" true 'A run at the dev tip must seed.'

docs_head="$(commit docs/notes.md)"
assert_current "$nightly_head" "$docs_head" true 'A merge without compile inputs must not block the seed.'

push_head="$(commit Telegram/SourceFiles/window.cpp)"
assert_current "$nightly_head" "$push_head" false \
  'A nightly run on an older head must not publish after a newer push seed exists.'
assert_current "$push_head" "$push_head" true 'The newer push seed must publish.'

script_head="$(commit .github/scripts/ccache_launcher.sh)"
assert_current "$push_head" "$script_head" false 'A cache script change must supersede the seed.'

cmake_head="$(commit CMakeLists.txt)"
assert_current "$script_head" "$cmake_head" false 'A root CMake change must supersede the seed.'

git -C "$repo" checkout -q -b rewritten "$nightly_head"
rewritten_head="$(commit docs/other.md)"
assert_current "$cmake_head" "$rewritten_head" false 'A head that left dev history must not seed.'

if bash "$seed_script" "$repo" "$push_head" >/dev/null 2>&1; then
  printf 'Missing arguments must fail.\n' >&2
  exit 1
fi

printf 'ccache seed currency tests passed.\n'
