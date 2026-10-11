#!/usr/bin/env bash
set -euo pipefail

if ! hits="$(awk -F '\t' '
  $1 == "cache_hit" {
    aggregate_count++
    if (NF != 2 || $2 !~ /^[0-9]+$/) {
      invalid = 1
    } else {
      aggregate = $2
    }
  }
  $1 == "direct_cache_hit" || $1 == "preprocessed_cache_hit" {
    if (NF != 2 || $2 !~ /^[0-9]+$/) {
      invalid = 1
    } else if ($1 == "direct_cache_hit") {
      direct_count++
      direct = $2
    } else {
      preprocessed_count++
      preprocessed = $2
    }
  }
  END {
    if (invalid) {
      exit 2
    }
    if (aggregate_count == 1 && direct_count == 0 && preprocessed_count == 0) {
      print aggregate
    } else if (aggregate_count == 0 && direct_count == 1 && preprocessed_count == 1) {
      print direct + preprocessed
    } else {
      exit 2
    }
  }
')"; then
  printf 'ccache statistics must contain one cache_hit count or one direct and preprocessed hit count.\n' >&2
  exit 2
fi

printf '%s\n' "${hits}"
