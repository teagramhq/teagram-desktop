#!/usr/bin/env bash
set -euo pipefail

operation="${1:-}"
image="${2:-}"
repo_dir="${3:-}"
output_file="${4:-}"
artifact_type='application/vnd.teagram.macos-libraries.v1'
layer_type='application/vnd.teagram.macos-libraries.tar+zstd'
archive_name='macos-libraries.tar.zst'
image_prefix='ghcr.io/teagramhq/teagram-desktop-macos-libraries:'

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

report_registry_result() {
  local result="$1"
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '### macOS dependency artifact\n\n- Image: `%s`\n- Registry result: %s\n\n' \
      "$image" "$result" >> "$GITHUB_STEP_SUMMARY"
  fi
}

validate_image() {
  [[ "$image" =~ ^ghcr\.io/teagramhq/teagram-desktop-macos-libraries:[[:xdigit:]]{64}$ ]] \
    || fail "Invalid macOS dependency image reference: ${image:-missing}"
}

write_cache_result() {
  local result="$1"
  printf 'cache-hit=%s\n' "$result" >> "$output_file"
  if [[ "$result" == 'true' ]]; then
    report_registry_result hit
  else
    report_registry_result miss
  fi
}

check_manifest() {
  local command_output command_status
  if command_output="$(oras manifest fetch "$image" 2>&1)"; then
    return 0
  else
    command_status=$?
  fi

  printf '%s\n' "$command_output" >&2
  if grep -Eiq \
    'MANIFEST_UNKNOWN|manifest unknown|no such manifest|manifest .+ not found|response status code 404|: 404([[:space:]]|$)' \
    <<< "$command_output"; then
    return 1
  fi

  printf '::error::Unable to inspect macOS dependency artifact %s (oras exit %s).\n' \
    "$image" "$command_status" >&2
  exit "$command_status"
}

case "$operation" in
  set-image)
    dependency_hash="${DEPENDENCY_HASH:-}"
    [[ "$dependency_hash" =~ ^[[:xdigit:]]{64}$ ]] \
      || fail "Invalid macOS dependency hash: ${dependency_hash:-missing}"
    [[ -n "${GITHUB_ENV:-}" ]] || fail 'GITHUB_ENV is required to set the macOS dependency image.'
    printf 'MACOS_LIBRARIES_IMAGE=%s%s\n' "$image_prefix" "$dependency_hash" >> "$GITHUB_ENV"
    ;;
  inspect)
    validate_image
    output_file="${3:-}"
    [[ -n "$output_file" ]] || fail 'An output file is required for artifact inspection.'
    if check_manifest; then
      write_cache_result true
    else
      write_cache_result false
    fi
    ;;
  pull)
    validate_image
    [[ -n "$repo_dir" && -d "$repo_dir" ]] || fail 'A checked-out repository directory is required for artifact pull.'
    [[ -n "$output_file" ]] || fail 'An output file is required for artifact pull.'
    if ! check_manifest; then
      write_cache_result false
      exit 0
    fi

    temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/teagram-macos-libraries.XXXXXX")"
    trap 'rm -rf "$temp_dir"' EXIT
    if ! oras pull "$image" --output "$temp_dir"; then
      fail "Unable to pull macOS dependency artifact ${image}."
    fi
    archive="${temp_dir}/${archive_name}"
    [[ -f "$archive" ]] || fail "macOS dependency artifact did not contain ${archive_name}."

    members="$(zstd -dc "$archive" | tar -tf -)" \
      || fail "Unable to list files in macOS dependency artifact ${image}."
    while IFS= read -r member; do
      case "$member" in
        Libraries|Libraries/*|ThirdParty|ThirdParty/*) ;;
        *) fail "Unexpected path in macOS dependency artifact: ${member}" ;;
      esac
      case "/${member}/" in
        */../*|*/./*) fail "Unsafe path in macOS dependency artifact: ${member}" ;;
      esac
    done <<< "$members"
    zstd -dc "$archive" | tar -xf - -C "$repo_dir"
    write_cache_result true
    ;;
  push)
    validate_image
    [[ -n "$repo_dir" && -d "$repo_dir/Libraries" && -d "$repo_dir/ThirdParty" ]] \
      || fail 'Built Libraries and ThirdParty directories are required for artifact publish.'
    temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/teagram-macos-libraries.XXXXXX")"
    trap 'rm -rf "$temp_dir"' EXIT
    archive="${temp_dir}/${archive_name}"
    tar -cf - -C "$repo_dir" Libraries ThirdParty | zstd -T0 -3 -o "$archive"
    [[ -s "$archive" ]] || fail 'The macOS dependency artifact archive is empty.'
    oras push "$image" \
      --artifact-type "$artifact_type" \
      "${archive}:${layer_type}"
    report_registry_result published
    ;;
  prune-legacy-cache)
    [[ -n "${GITHUB_REPOSITORY:-}" ]] || fail 'GITHUB_REPOSITORY is required to prune legacy caches.'
    cache_ids="$(gh cache list --limit 1000 --key macOS-libs- --json id --jq '.[].id' \
      --repo "$GITHUB_REPOSITORY")"
    while IFS= read -r cache_id; do
      [[ -n "$cache_id" ]] || continue
      [[ "$cache_id" =~ ^[0-9]+$ ]] || fail "Unexpected legacy cache ID from GitHub: ${cache_id}"
      gh cache delete "$cache_id" --repo "$GITHUB_REPOSITORY"
    done <<< "$cache_ids"
    ;;
  *)
    fail "Usage: $0 <set-image|inspect|pull|push|prune-legacy-cache> [image] [repo-dir] [output-file]"
    ;;
esac
