#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cleanup_workflow="${scripts_dir}/../workflows/pr_cache_cleanup.yml"
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

mkdir -p "${temp_dir}/bin"
cat > "${temp_dir}/bin/gh" <<'MOCK_GH'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "cache" && "${2:-}" == "list" ]]; then
  shift 2
  key_prefix=''
  ref=''
  while (( $# > 0 )); do
    case "$1" in
      --key) key_prefix="$2"; shift 2 ;;
      --ref) ref="$2"; shift 2 ;;
      --limit|--sort|--order|--json|--jq|--repo) shift 2 ;;
      *) printf 'Unexpected gh cache list argument: %s\n' "$1" >&2; exit 1 ;;
    esac
  done
  [[ -n "$key_prefix" && -n "$ref" ]]
  awk -F '\t' -v prefix="$key_prefix" -v ref="$ref" \
    'index($2, prefix) == 1 && $3 == ref { print $1 "\t" $2 }' "$MOCK_CACHE_STATE"
  exit 0
fi

if [[ "${1:-}" == "cache" && "${2:-}" == "delete" ]]; then
  cache_id="${3:-}"
  [[ "$cache_id" =~ ^[0-9]+$ ]]
  next_state="${MOCK_CACHE_STATE}.next"
  awk -F '\t' -v cache_id="$cache_id" '$1 != cache_id { print }' \
    "$MOCK_CACHE_STATE" > "$next_state"
  mv "$next_state" "$MOCK_CACHE_STATE"
  printf '%s\n' "$cache_id" >> "$MOCK_DELETED_IDS"
  exit 0
fi

printf 'Unexpected gh invocation: %s\n' "$*" >&2
exit 1
MOCK_GH
chmod +x "${temp_dir}/bin/gh"

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

prune_script="${temp_dir}/prune-linux-cache.sh"
awk '
  /^      - name: Keep the newest Linux compiler cache\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step && /^        run: \|$/ { in_run = 1; next }
  in_run && /^  [^ ]/ { exit }
  in_run { sub(/^          /, ""); print }
' "$cleanup_workflow" > "$prune_script"
[[ -s "$prune_script" ]] || fail 'The cleanup workflow must expose its Linux cache policy for regression tests.'

run_prune() {
  env PATH="${temp_dir}/bin:${PATH}" GITHUB_REPOSITORY=teagramhq/teagram-desktop \
    MOCK_CACHE_STATE="$cache_state" MOCK_DELETED_IDS="$deleted_ids" \
    bash "$prune_script"
}

libs_hash='cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc'
mac_hash='dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd'
cache_state="${temp_dir}/cache-state.tsv"
deleted_ids="${temp_dir}/deleted-ids.txt"
: > "$deleted_ids"
printf '%s\t%s\t%s\n' \
  303 'Linux-tdesktop-default-3003' refs/heads/dev \
  302 'Linux-tdesktop-default-3002' refs/heads/dev \
  301 'Linux-tdesktop-default-3001' refs/heads/dev \
  300 'Linux-tdesktop-default-2999' refs/pull/7/merge \
  310 "Linux-libs-${libs_hash}" refs/heads/dev \
  320 "macOS-libs-${mac_hash}" refs/heads/dev \
  330 "macOS-arm64-ccache-dev-pr-compatible-${mac_hash}-1001-1" refs/heads/dev \
  > "$cache_state"

run_prune > "${temp_dir}/first-cleanup.log"
grep -Fq $'303\tLinux-tdesktop-default-3003\trefs/heads/dev' "$cache_state" || fail 'Cleanup must keep the newest dev Linux compiler cache.'
for cache_id in 301 302; do
  if grep -Fq "${cache_id}"$'\t' "$cache_state"; then
    fail "Cleanup must remove superseded dev Linux compiler cache id ${cache_id}."
  fi
done
for cache_id in 300 310 320 330; do
  grep -Fq "${cache_id}"$'\t' "$cache_state" || fail "Cleanup must not touch cache id ${cache_id} outside the dev Linux compiler prefix."
done

printf '%s\t%s\t%s\n' 304 'Linux-tdesktop-default-3004' refs/heads/dev > "${cache_state}.next"
cat "$cache_state" >> "${cache_state}.next"
mv "${cache_state}.next" "$cache_state"
run_prune > "${temp_dir}/second-cleanup.log"
grep -Fq $'304\tLinux-tdesktop-default-3004' "$cache_state" || fail 'A later dev save must become the retained Linux compiler cache.'
if grep -Fq $'303\t' "$cache_state"; then
  fail 'A later dev save must supersede the previous Linux compiler cache.'
fi

: > "$deleted_ids"
run_prune > "${temp_dir}/idempotent-cleanup.log"
[[ ! -s "$deleted_ids" ]] || fail 'Cleanup must not delete the only remaining dev Linux compiler cache.'

printf '%s\t%s\t%s\n' 305 'Linux-tdesktop-default-unexpected' refs/heads/dev >> "$cache_state"
if run_prune > "${temp_dir}/unexpected-cleanup.log" 2>&1; then
  fail 'Cleanup must refuse an unexpected dev Linux compiler cache key.'
fi
grep -Fq 'Unexpected dev Linux compiler cache key' "${temp_dir}/unexpected-cleanup.log" || fail 'Cleanup must name the unexpected key it refused.'

printf 'Linux dev cache cleanup sequence tests passed.\n'
