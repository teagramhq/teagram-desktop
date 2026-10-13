#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
script="${scripts_dir}/linux_libraries_image.sh"
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

mkdir -p "${temp_dir}/bin"
cat > "${temp_dir}/bin/docker" <<'MOCK_DOCKER'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${MOCK_DOCKER_STATUS:-0}" == "0" ]]; then
  printf '%s\n' "mock docker $*"
  exit 0
fi
printf '%s\n' "${MOCK_DOCKER_OUTPUT:-mock docker error}" >&2
exit "${MOCK_DOCKER_STATUS}"
MOCK_DOCKER
chmod +x "${temp_dir}/bin/docker"

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

output="${temp_dir}/output"
summary="${temp_dir}/summary"
: > "$output"
env PATH="${temp_dir}/bin:${PATH}" GITHUB_STEP_SUMMARY="$summary" MOCK_DOCKER_STATUS=0 \
  bash "$script" pull ghcr.io/example/linux-libraries:abc tdesktop:centos_env "$output" \
  > "${temp_dir}/success.log"
grep -Fqx 'cache-hit=true' "$output" || fail 'A successful pull must report a cache hit.'
grep -Fq 'mock docker tag ghcr.io/example/linux-libraries:abc tdesktop:centos_env' \
  "${temp_dir}/success.log" || fail 'A successful pull must tag the local build image.'
grep -Fq -- '- Registry result: hit' "$summary" || fail 'A successful pull must report its cache hit in the step summary.'

: > "$output"
env PATH="${temp_dir}/bin:${PATH}" GITHUB_STEP_SUMMARY="$summary" MOCK_DOCKER_STATUS=1 \
  MOCK_DOCKER_OUTPUT='manifest unknown' \
  bash "$script" inspect ghcr.io/example/linux-libraries:missing '' "$output" \
  > "${temp_dir}/missing.log" 2>&1
grep -Fqx 'cache-hit=false' "$output" || fail 'A missing manifest must request a build fallback.'
grep -Fq -- '- Registry result: miss' "$summary" || fail 'A missing tag must report the cache miss in the step summary.'

: > "$output"
env PATH="${temp_dir}/bin:${PATH}" GITHUB_STEP_SUMMARY="$summary" MOCK_DOCKER_STATUS=1 \
  MOCK_DOCKER_OUTPUT='Error response from daemon: manifest for ghcr.io/example/linux-libraries:missing not found: manifest unknown' \
  bash "$script" pull ghcr.io/example/linux-libraries:missing tdesktop:centos_env "$output" \
  > "${temp_dir}/missing-pull.log" 2>&1
grep -Fqx 'cache-hit=false' "$output" || fail 'A missing image manifest during pull must request a build fallback.'

: > "$output"
if env PATH="${temp_dir}/bin:${PATH}" MOCK_DOCKER_STATUS=1 \
  MOCK_DOCKER_OUTPUT='denied: requested access to the resource is denied' \
  bash "$script" pull ghcr.io/example/linux-libraries:abc tdesktop:centos_env "$output" \
  > "${temp_dir}/denied.log" 2>&1; then
  fail 'An authorization error must not be treated as a cache miss.'
fi
if [[ -s "$output" ]]; then
  fail 'An authorization error must not report a cache result.'
fi

: > "$output"
if env PATH="${temp_dir}/bin:${PATH}" MOCK_DOCKER_STATUS=1 \
  MOCK_DOCKER_OUTPUT='failed to copy: unexpected status from GET request to https://ghcr.io/v2/example/linux-libraries/blobs/sha256:abc: 404 Not Found' \
  bash "$script" pull ghcr.io/example/linux-libraries:abc tdesktop:centos_env "$output" \
  > "${temp_dir}/layer-404.log" 2>&1; then
  fail 'A missing layer during pull must fail instead of requesting a cache-miss fallback.'
fi
if [[ -s "$output" ]]; then
  fail 'A missing layer during pull must not report a cache result.'
fi

printf 'Linux dependency image tests passed.\n'
