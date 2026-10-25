#!/usr/bin/env bash
set -euo pipefail

if (( $# != 1 )); then
  printf 'Usage: %s <pull_request|push|schedule|workflow_dispatch>\n' "${0##*/}" >&2
  exit 2
fi

case "$1" in
  pull_request|push|schedule)
    profile=pr-compatible
    protected_path_integration=OFF
    ;;
  workflow_dispatch)
    profile=pr-compatible
    protected_path_integration=ON
    ;;
  *)
    printf 'Unsupported MacOS compiler cache event: %s\n' "$1" >&2
    exit 2
    ;;
esac

printf 'name=%s\nprotected_path_integration=%s\n' \
  "$profile" "$protected_path_integration"
