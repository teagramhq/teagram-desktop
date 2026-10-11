#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
parser_script="${scripts_dir}/ccache_hit_count.sh"

assert_count() {
  local stats="$1"
  local expected="$2"
  local actual
  actual="$(printf '%s' "${stats}" | bash "${parser_script}")"
  if [[ "${actual}" != "${expected}" ]]; then
    printf 'Expected %s hits, got %s.\n' "${expected}" "${actual}" >&2
    exit 1
  fi
}

assert_rejected() {
  local stats="$1"
  if printf '%s' "${stats}" | bash "${parser_script}" >/dev/null 2>&1; then
    printf 'Invalid ccache statistics should fail.\n' >&2
    exit 1
  fi
}

assert_count $'cache_hit\t9\n' 9
assert_count $'direct_cache_hit\t7\npreprocessed_cache_hit\t3\n' 10
assert_rejected $'direct_cache_hit\t7\n'
assert_rejected $'cache_hit\t9\ndirect_cache_hit\t7\npreprocessed_cache_hit\t3\n'
assert_rejected $'direct_cache_hit\tbad\npreprocessed_cache_hit\t3\n'
assert_rejected $'direct_cache_hit\t7\ndirect_cache_hit\t2\npreprocessed_cache_hit\t3\n'

printf 'ccache hit count tests passed.\n'
