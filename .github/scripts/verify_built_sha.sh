#!/usr/bin/env bash
set -euo pipefail

expected_sha="${1:-}"
if [[ ! "${expected_sha}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "::error::The reported commit SHA is missing or malformed: '${expected_sha}'"
  exit 1
fi

if ! built_sha="$(git rev-parse --verify HEAD)"; then
  echo "::error::Unable to resolve the checked-out commit."
  exit 1
fi

echo "Building ${built_sha}; the check is reported against ${expected_sha}."
if [[ "${built_sha}" != "${expected_sha}" ]]; then
  echo "::error::The checked-out commit ${built_sha} is not the reported commit ${expected_sha}."
  exit 1
fi
