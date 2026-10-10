#!/usr/bin/env bash
set -Eeuo pipefail

: "${GITHUB_REPOSITORY:?}"
: "${GITHUB_SHA:?}"
: "${TEAGRAM_UPDATE_BUILD:?}"

tag="teagram-build-${TEAGRAM_UPDATE_BUILD}"
tag_ref="refs/tags/$tag"
refs="$(gh api "repos/$GITHUB_REPOSITORY/git/matching-refs/tags/$tag")"
exact_refs="$(jq -ce --arg ref "$tag_ref" '[.[] | select(.ref == $ref)]' <<< "$refs")"
match_count="$(jq -er 'length' <<< "$exact_refs")"

if [[ "$match_count" == 0 ]]; then
  created="$(gh api --method POST "repos/$GITHUB_REPOSITORY/git/refs" \
    --field "ref=$tag_ref" --field "sha=$GITHUB_SHA")"
  jq -e --arg ref "$tag_ref" --arg sha "$GITHUB_SHA" \
    '.ref == $ref and .object.type == "commit" and .object.sha == $sha' \
    <<< "$created" >/dev/null
  exit 0
fi

if [[ "$match_count" != 1 ]]; then
  echo "::error::The release tag lookup returned multiple exact refs."
  exit 1
fi

reference="$(jq -ce '.[0]' <<< "$exact_refs")"
object_type="$(jq -er '.object.type | strings' <<< "$reference")"
object_sha="$(jq -er '.object.sha | strings' <<< "$reference")"
depth=0
while [[ "$object_type" == tag ]]; do
  if (( depth >= 5 )); then
    echo "::error::The release tag exceeds the supported annotated-tag depth."
    exit 1
  fi
  reference="$(gh api "repos/$GITHUB_REPOSITORY/git/tags/$object_sha")"
  object_type="$(jq -er '.object.type | strings' <<< "$reference")"
  object_sha="$(jq -er '.object.sha | strings' <<< "$reference")"
  depth=$((depth + 1))
done

if [[ "$object_type" != commit || "$object_sha" != "$GITHUB_SHA" ]]; then
  echo "::error::The existing release tag does not resolve to this source commit."
  exit 1
fi
