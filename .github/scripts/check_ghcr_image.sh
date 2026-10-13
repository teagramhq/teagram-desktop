#!/usr/bin/env bash
set -euo pipefail

image_ref="${1:?Usage: check_ghcr_image.sh <image-ref>}"
if [[ -z "${GITHUB_OUTPUT:-}" ]]; then
  echo "::error::GITHUB_OUTPUT is not set." >&2
  exit 1
fi

if output="$(docker buildx imagetools inspect "$image_ref" 2>&1)"; then
  echo "exists=true" >> "$GITHUB_OUTPUT"
  printf 'Published dependency image exists: %s\n' "$image_ref"
elif grep -Eiq 'manifest unknown|no such manifest|not found' <<< "$output"; then
  echo "exists=false" >> "$GITHUB_OUTPUT"
  printf 'Published dependency image is missing: %s\n' "$image_ref"
else
  printf '%s\n' "$output" >&2
  echo "::error::Unable to check published dependency image: $image_ref" >&2
  exit 1
fi
