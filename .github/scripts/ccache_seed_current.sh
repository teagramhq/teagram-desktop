#!/usr/bin/env bash
set -euo pipefail

# Prints current=true when no compile input changed on dev after the built
# commit, so a seed from an older head never lands after a newer push seed.
# The pathspecs match the MacOS. push filter.

if (( $# != 3 )); then
  printf 'Usage: %s <repository> <built-sha> <dev-tip>\n' "${0##*/}" >&2
  exit 2
fi

repository="$1"
built_sha="$2"
dev_tip="$3"

if ! git -C "$repository" merge-base --is-ancestor "$built_sha" "$dev_tip"; then
  printf 'current=false\n'
  exit 0
fi

if git -C "$repository" diff --quiet "$built_sha" "$dev_tip" -- \
  'Telegram' \
  'cmake' \
  'CMakeLists.txt' \
  '.github/workflows/mac.yml' \
  ':(glob).github/scripts/ccache_*.sh' \
  '.github/scripts/mac_ccache_profile.sh'; then
  printf 'current=true\n'
else
  printf 'current=false\n'
fi
