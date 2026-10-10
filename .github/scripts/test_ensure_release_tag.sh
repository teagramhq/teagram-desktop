#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"

cat > "$tmp/bin/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [[ "$1" != api ]]; then
  exit 2
fi
shift
method=GET
endpoint=""
ref=""
sha=""
while (($#)); do
  case "$1" in
    --method)
      method="$2"
      shift 2
      ;;
    --field)
      case "$2" in
        ref=*) ref="${2#ref=}" ;;
        sha=*) sha="${2#sha=}" ;;
      esac
      shift 2
      ;;
    *)
      if [[ -z "$endpoint" ]]; then
        endpoint="$1"
      fi
      shift
      ;;
  esac
done

printf '%s %s %s %s\n' "$method" "$endpoint" "$ref" "$sha" >> "$FAKE_GH_LOG"
case "$endpoint" in
  */git/matching-refs/tags/*)
    cat "$FAKE_TAG_STATE"
    ;;
  */git/refs)
    if [[ "$method" != POST || "$ref" != "refs/tags/$EXPECTED_TAG" || "$sha" != "$GITHUB_SHA" ]]; then
      exit 3
    fi
    jq -n --arg ref "$ref" --arg sha "$sha" \
      '{ref: $ref, object: {type: "commit", sha: $sha}}' > "$FAKE_TAG_STATE"
    cat "$FAKE_TAG_STATE"
    ;;
  *)
    exit 4
    ;;
esac
EOF
chmod +x "$tmp/bin/gh"

GITHUB_REPOSITORY=teagramhq/teagram-desktop
GITHUB_SHA=0123456789abcdef0123456789abcdef01234567
TEAGRAM_UPDATE_BUILD=417
EXPECTED_TAG="teagram-build-$TEAGRAM_UPDATE_BUILD"
export GITHUB_REPOSITORY GITHUB_SHA TEAGRAM_UPDATE_BUILD EXPECTED_TAG
export PATH="$tmp/bin:$PATH"
export FAKE_TAG_STATE="$tmp/tag-state.json"
export FAKE_GH_LOG="$tmp/gh.log"

run_helper() {
  : > "$FAKE_GH_LOG"
  bash "$repo_root/.github/scripts/ensure_release_tag.sh"
}

printf '[]\n' > "$FAKE_TAG_STATE"
run_helper
jq -e --arg ref "refs/tags/$EXPECTED_TAG" --arg sha "$GITHUB_SHA" \
  '.ref == $ref and .object.type == "commit" and .object.sha == $sha' \
  "$FAKE_TAG_STATE" >/dev/null
if ! grep -Fq 'POST repos/teagramhq/teagram-desktop/git/refs' "$FAKE_GH_LOG"; then
  printf 'An absent release tag must be created at the current source SHA.\n' >&2
  exit 1
fi

jq -n --arg ref "refs/tags/$EXPECTED_TAG" --arg sha "$GITHUB_SHA" \
  '[{ref: $ref, object: {type: "commit", sha: $sha}}]' > "$FAKE_TAG_STATE"
run_helper
if grep -Fq 'POST repos/teagramhq/teagram-desktop/git/refs' "$FAKE_GH_LOG"; then
  printf 'An existing matching release tag must not be recreated.\n' >&2
  exit 1
fi

other_sha=abcdef0123456789abcdef0123456789abcdef01
jq -n --arg ref "refs/tags/$EXPECTED_TAG" --arg sha "$other_sha" \
  '[{ref: $ref, object: {type: "commit", sha: $sha}}]' > "$FAKE_TAG_STATE"
if run_helper > "$tmp/mismatch-error.log" 2>&1; then
  printf 'An existing release tag at a different SHA must fail verification.\n' >&2
  exit 1
fi
if grep -Fq 'POST repos/teagramhq/teagram-desktop/git/refs' "$FAKE_GH_LOG"; then
  printf 'A mismatched release tag must not be overwritten.\n' >&2
  exit 1
fi

printf 'release tag source tests passed.\n'
