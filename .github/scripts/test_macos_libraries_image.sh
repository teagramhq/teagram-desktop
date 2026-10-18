#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
script="${scripts_dir}/macos_libraries_image.sh"
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

workspace="${temp_dir}/workspace"
checkout="${workspace}/teagram-desktop"
mkdir -p "${temp_dir}/bin" "${workspace}/Libraries/lib" \
  "${workspace}/ThirdParty/include" "${checkout}/Telegram"
printf 'library fixture\n' > "${workspace}/Libraries/lib/example.a"
printf 'header fixture\n' > "${workspace}/ThirdParty/include/example.h"

cat > "${temp_dir}/bin/oras" <<'MOCK_ORAS'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$MOCK_ORAS_LOG"
operation="${1:-}"
shift || true
case "$operation" in
  manifest)
    if [[ "${MOCK_ORAS_MANIFEST_STATUS:-0}" == '0' ]]; then
      printf '{"schemaVersion":2}\n'
      exit 0
    fi
    printf '%s\n' "${MOCK_ORAS_MANIFEST_OUTPUT:-manifest unknown}" >&2
    exit "${MOCK_ORAS_MANIFEST_STATUS}"
    ;;
  pull)
    if [[ "${MOCK_ORAS_PULL_STATUS:-0}" != '0' ]]; then
      printf '%s\n' "${MOCK_ORAS_PULL_OUTPUT:-mock pull failed}" >&2
      exit "${MOCK_ORAS_PULL_STATUS}"
    fi
    output_dir='.'
    while (($#)); do
      if [[ "$1" == '--output' ]]; then
        output_dir="$2"
        shift 2
      else
        shift
      fi
    done
    cp "$MOCK_ORAS_ARCHIVE" "$output_dir/macos-libraries.tar.zst"
    ;;
  push)
    archive=''
    for argument in "$@"; do
      if [[ "$argument" == *'application/vnd.teagram.macos-libraries.tar+zstd' ]]; then
        archive="${argument%%:*}"
      fi
    done
    [[ "$archive" != /* ]] || {
      printf 'oras push must receive a relative archive path.\n' >&2
      exit 1
    }
    [[ "$archive" == 'macos-libraries.tar.zst' ]] || {
      printf 'oras push must receive the archive basename.\n' >&2
      exit 1
    }
    [[ -n "$archive" && -f "$archive" ]] || {
      printf 'oras push did not receive the macOS library archive.\n' >&2
      exit 1
    }
    cp "$archive" "$MOCK_ORAS_PUSHED_ARCHIVE"
    ;;
  *)
    printf 'Unexpected oras operation: %s\n' "$operation" >&2
    exit 2
    ;;
esac
MOCK_ORAS
chmod +x "${temp_dir}/bin/oras"

cat > "${temp_dir}/bin/zstd" <<'MOCK_ZSTD'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == '-dc' ]]; then
  gzip -dc "$2"
else
  output=''
  while (($#)); do
    if [[ "$1" == '-o' ]]; then
      output="$2"
      shift 2
    else
      shift
    fi
  done
  [[ -n "$output" ]] || exit 2
  gzip -1c > "$output"
fi
MOCK_ZSTD
chmod +x "${temp_dir}/bin/zstd"

cat > "${temp_dir}/bin/gh" <<'MOCK_GH'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == 'cache' && "${2:-}" == 'list' ]]; then
  [[ " $* " == *' --key macOS-libs- '* ]] || {
    printf 'Legacy cache listing must use the macOS library key prefix.\n' >&2
    exit 1
  }
  [[ " $* " == *' --repo teagramhq/teagram-desktop '* ]] || {
    printf 'Legacy cache listing must target the desktop repository.\n' >&2
    exit 1
  }
  printf '91\n92\n'
elif [[ "${1:-}" == 'cache' && "${2:-}" == 'delete' ]]; then
  printf '%s\n' "$3" >> "$MOCK_GH_DELETED"
else
  printf 'Unexpected gh command: %s\n' "$*" >&2
  exit 2
fi
MOCK_GH
chmod +x "${temp_dir}/bin/gh"

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

image='ghcr.io/teagramhq/teagram-desktop-macos-libraries:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
output="${temp_dir}/output"
summary="${temp_dir}/summary"
archive="${temp_dir}/macos-libraries.tar.zst"
push_archive="${temp_dir}/pushed.tar.zst"
export PATH="${temp_dir}/bin:${PATH}"
export GITHUB_REPOSITORY='teagramhq/teagram-desktop'
export GITHUB_STEP_SUMMARY="$summary"
export MOCK_ORAS_LOG="${temp_dir}/oras.log"
export MOCK_ORAS_ARCHIVE="$archive"
export MOCK_ORAS_PUSHED_ARCHIVE="$push_archive"

hash='0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
DEPENDENCY_HASH="$hash" GITHUB_ENV="$output" bash "$script" set-image
grep -Fqx "MACOS_LIBRARIES_IMAGE=ghcr.io/teagramhq/teagram-desktop-macos-libraries:${hash}" \
  "$output" || fail 'A valid dependency hash must produce a recipe-hash GHCR reference.'
: > "$output"
if env DEPENDENCY_HASH=null GITHUB_ENV="$output" bash "$script" set-image \
  > "${temp_dir}/invalid-hash.log" 2>&1; then
  fail 'A missing dependency hash must fail.'
fi
[[ ! -s "$output" ]] || fail 'An invalid hash must not write an image reference.'

: > "$output"
: > "$summary"
env MOCK_ORAS_MANIFEST_STATUS=0 bash "$script" inspect "$image" "$output" \
  > "${temp_dir}/inspect-hit.log"
grep -Fqx 'cache-hit=true' "$output" || fail 'A found artifact must report a hit.'
grep -Fq -- '- Registry result: hit' "$summary" || fail 'A hit must appear in the step summary.'

: > "$output"
env MOCK_ORAS_MANIFEST_STATUS=1 \
  MOCK_ORAS_MANIFEST_OUTPUT='Error: response status code 404: MANIFEST_UNKNOWN: manifest unknown' \
  bash "$script" inspect "$image" "$output" > "${temp_dir}/inspect-miss.log" 2>&1
grep -Fqx 'cache-hit=false' "$output" || fail 'A missing artifact must request a build fallback.'

: > "$output"
if env MOCK_ORAS_MANIFEST_STATUS=1 \
  MOCK_ORAS_MANIFEST_OUTPUT='Error: response status code 401: UNAUTHORIZED: authentication required' \
  bash "$script" inspect "$image" "$output" > "${temp_dir}/inspect-denied.log" 2>&1; then
  fail 'An authorization error must not be treated as an artifact miss.'
fi
[[ ! -s "$output" ]] || fail 'An authorization error must not report a cache result.'

tar -cf - -C "$workspace" Libraries ThirdParty | gzip -1c > "$archive"
rm -rf "$workspace/Libraries" "$workspace/ThirdParty"
: > "$output"
env MOCK_ORAS_MANIFEST_STATUS=0 bash "$script" pull "$image" "$workspace" "$output" \
  > "${temp_dir}/pull-hit.log"
grep -Fqx 'cache-hit=true' "$output" || fail 'A successful pull must report a hit.'
grep -Fq 'library fixture' "$workspace/Libraries/lib/example.a" \
  || fail 'A successful pull must extract Libraries into the workspace.'
grep -Fq 'header fixture' "$workspace/ThirdParty/include/example.h" \
  || fail 'A successful pull must extract ThirdParty into the workspace.'
[[ ! -e "$checkout/Libraries" && ! -e "$checkout/ThirdParty" ]] \
  || fail 'A successful pull must not put dependencies inside the checkout.'

: > "$output"
env MOCK_ORAS_MANIFEST_STATUS=1 MOCK_ORAS_MANIFEST_OUTPUT='manifest unknown: 404' \
  bash "$script" pull "$image" "$workspace" "$output" \
  > "${temp_dir}/pull-miss.log" 2>&1
grep -Fqx 'cache-hit=false' "$output" || fail 'A missing pull must request a build fallback.'

: > "$output"
env MOCK_ORAS_MANIFEST_STATUS=1 \
  MOCK_ORAS_MANIFEST_OUTPUT='Error response from registry: failed to fetch the content of "ghcr.io/example/image:tag": ghcr.io/example/image:tag: not found' \
  bash "$script" pull "$image" "$workspace" "$output" \
  > "${temp_dir}/pull-registry-miss.log" 2>&1
grep -Fqx 'cache-hit=false' "$output" \
  || fail 'A not-found registry response must request a build fallback.'

: > "$output"
if env MOCK_ORAS_MANIFEST_STATUS=0 MOCK_ORAS_PULL_STATUS=1 \
  MOCK_ORAS_PULL_OUTPUT='missing layer: 404' \
  bash "$script" pull "$image" "$workspace" "$output" \
  > "${temp_dir}/pull-layer-error.log" 2>&1; then
  fail 'A missing layer must fail instead of using an incomplete artifact.'
fi
[[ ! -s "$output" ]] || fail 'A failed layer pull must not report a cache result.'

bash "$script" push "$image" "$workspace" > "${temp_dir}/push.log"
grep -Fq "push $image --artifact-type application/vnd.teagram.macos-libraries.v1" \
  "$MOCK_ORAS_LOG" || fail 'Publishing must use the recipe-hash tag and artifact type.'
gzip -dc "$push_archive" | tar -tf - | grep -Fqx 'Libraries/lib/example.a' \
  || fail 'The published artifact must contain Libraries.'
gzip -dc "$push_archive" | tar -tf - | grep -Fqx 'ThirdParty/include/example.h' \
  || fail 'The published artifact must contain ThirdParty.'

export MOCK_GH_DELETED="${temp_dir}/deleted-caches"
bash "$script" prune-legacy-cache > "${temp_dir}/prune.log"
printf '91\n92\n' > "${temp_dir}/expected-deletes"
cmp -s "$MOCK_GH_DELETED" "${temp_dir}/expected-deletes" \
  || fail 'Pruning must delete every returned legacy macOS library cache ID.'

printf 'macOS dependency artifact tests passed.\n'
