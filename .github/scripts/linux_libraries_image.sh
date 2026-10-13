#!/usr/bin/env bash
set -euo pipefail

operation="${1:-}"
image="${2:-}"
local_image="${3:-}"
output_file="${4:-}"

report_cache_result() {
  local result="$1"
  if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    printf '### Linux dependency image\n\n- Image: `%s`\n- Registry result: %s\n\n' \
      "$image" "$result" >> "$GITHUB_STEP_SUMMARY"
  fi
}

if [[ "$operation" != "pull" && "$operation" != "inspect" ]]; then
  printf 'Usage: %s <pull|inspect> <image> <local-image> <output-file>\n' "$0" >&2
  exit 2
fi
if [[ -z "$image" || -z "$output_file" || ( "$operation" == "pull" && -z "$local_image" ) ]]; then
  printf 'Image, output file, and pull destination are required.\n' >&2
  exit 2
fi

if [[ "$operation" == "pull" ]]; then
  if command_output="$(docker pull "$image" 2>&1)"; then
    printf '%s\n' "$command_output"
    docker tag "$image" "$local_image"
    echo 'cache-hit=true' >> "$output_file"
    report_cache_result hit
    exit 0
  else
    command_status=$?
  fi
else
  if command_output="$(docker manifest inspect "$image" 2>&1)"; then
    printf '%s\n' "$command_output"
    echo 'cache-hit=true' >> "$output_file"
    report_cache_result hit
    exit 0
  else
    command_status=$?
  fi
fi

printf '%s\n' "$command_output" >&2
if grep -Eiq \
  'MANIFEST_UNKNOWN|manifest unknown|no such manifest|manifest for .+ not found|/manifests/[^[:space:]]+[^[:cntrl:]]*: 404([[:space:]]|$)' \
  <<< "$command_output"; then
  echo "::notice::Linux dependency image tag is missing: ${image}"
  echo 'cache-hit=false' >> "$output_file"
  report_cache_result miss
  exit 0
fi

printf '::error::Unable to %s Linux dependency image %s (docker exit %s).\n' \
  "$operation" "$image" "$command_status" >&2
exit "$command_status"
