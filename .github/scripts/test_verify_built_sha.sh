#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
script="${scripts_dir}/verify_built_sha.sh"
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

repo="${temp_dir}/repo"
git init -q "${repo}"
git_in_repo() {
  git -C "${repo}" -c user.name=test -c user.email=test@example.invalid "$@"
}
git_in_repo commit -q --allow-empty -m first
older_sha="$(git_in_repo rev-parse HEAD)"
git_in_repo commit -q --allow-empty -m second
newer_sha="$(git_in_repo rev-parse HEAD)"

if ! output="$(cd "${repo}" && bash "${script}" "${newer_sha}" 2>&1)"; then
  printf '%s\n' "${output}" >&2
  fail 'The checked-out commit should pass when it is the reported commit.'
fi
if [[ "${output}" != *"Building ${newer_sha}; the check is reported against ${newer_sha}."* ]]; then
  fail 'The log should name the built and reported commits.'
fi

# Two rapid dev pushes: the run reports against the newer commit while the
# checkout holds the older one. That must never pass.
git_in_repo checkout -q "${older_sha}"
if output="$(cd "${repo}" && bash "${script}" "${newer_sha}" 2>&1)"; then
  fail 'A checkout of a different commit than the reported one should fail.'
fi
if [[ "${output}" != *"is not the reported commit ${newer_sha}"* ]]; then
  printf '%s\n' "${output}" >&2
  fail 'A mismatch should name the reported commit.'
fi

for bad_sha in '' 'dev' "${newer_sha:0:12}" "${newer_sha^^}"; do
  if (cd "${repo}" && bash "${script}" "${bad_sha}") >/dev/null 2>&1; then
    fail "A malformed reported SHA '${bad_sha}' should fail."
  fi
done

printf 'Built commit verification tests passed.\n'
