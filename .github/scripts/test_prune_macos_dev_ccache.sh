#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
profile_script="${scripts_dir}/mac_ccache_profile.sh"
cleanup_workflow="${scripts_dir}/../workflows/pr_cache_cleanup.yml"
temp_dir="$(mktemp -d)"
trap 'rm -rf "${temp_dir}"' EXIT

mkdir -p "${temp_dir}/bin"
cat > "${temp_dir}/bin/gh" <<'MOCK_GH'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "cache" && "${2:-}" == "list" ]]; then
  cat "$MOCK_CACHE_STATE"
  exit 0
fi

if [[ "${1:-}" == "cache" && "${2:-}" == "delete" ]]; then
  cache_id="${3:-}"
  [[ "$cache_id" =~ ^[0-9]+$ ]]
  next_state="${MOCK_CACHE_STATE}.next"
  awk -F '\t' -v cache_id="$cache_id" '$1 != cache_id { print $1 "\t" $2 }' \
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

prune_script="${temp_dir}/prune-macos-cache.sh"
awk '
  /^      - name: Keep the newest compiler cache per profile\.$/ { in_step = 1; next }
  in_step && /^      - name:/ { exit }
  in_step && /^        run: \|$/ { in_run = 1; next }
  in_run { sub(/^          /, ""); print }
' "$cleanup_workflow" > "$prune_script"
[[ -s "$prune_script" ]] || fail 'The cleanup workflow must expose its cache policy for regression tests.'

schedule_profile="$(bash "$profile_script" schedule)"
pr_profile="$(bash "$profile_script" pull_request)"
dispatch_profile="$(bash "$profile_script" workflow_dispatch)"
[[ "$schedule_profile" == "$pr_profile" ]] || fail 'The PR profile must match the scheduled seed profile.'
[[ "$schedule_profile" == $'name=pr-compatible\nprotected_path_integration=OFF' ]] || fail 'The scheduled seed must use the PR-compatible profile.'
[[ "$dispatch_profile" == $'name=protected\nprotected_path_integration=ON' ]] || fail 'The dispatch must use the protected diagnostic profile.'

pr_hash='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
protected_hash='bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
cache_state="${temp_dir}/cache-state.tsv"
deleted_ids="${temp_dir}/deleted-ids.txt"
: > "$deleted_ids"
printf '%s\t%s\n' \
  101 "macOS-arm64-ccache-dev-pr-compatible-${pr_hash}-1001-1" \
  100 "macOS-arm64-ccache-dev-pr-compatible-${pr_hash}-1000-1" > "$cache_state"

env PATH="${temp_dir}/bin:${PATH}" GITHUB_REPOSITORY=teagramhq/teagram-desktop \
  MOCK_CACHE_STATE="$cache_state" MOCK_DELETED_IDS="$deleted_ids" \
  bash "$prune_script" > "${temp_dir}/scheduled-cleanup.log"
grep -Fq $'101\tmacOS-arm64-ccache-dev-pr-compatible-' "$cache_state" || fail 'The scheduled PR-compatible seed must survive cleanup.'
if grep -Fq $'100\t' "$cache_state"; then
  fail 'Cleanup must remove the older PR-compatible snapshot.'
fi

dispatch_key="macOS-arm64-ccache-dev-protected-${protected_hash}-1002-1"
older_dispatch_key="macOS-arm64-ccache-dev-protected-${protected_hash}-1000-1"
packaged_key="macOS-arm64-ccache-dev-${pr_hash}-999-1"
older_packaged_key="macOS-arm64-ccache-dev-${pr_hash}-998-1"
{
  printf '202\t%s\n' "$dispatch_key"
  cat "$cache_state"
  printf '201\t%s\n' "$older_dispatch_key"
  printf '199\t%s\n' "$packaged_key"
  printf '198\t%s\n' "$older_packaged_key"
} > "${cache_state}.next"
mv "${cache_state}.next" "$cache_state"

env PATH="${temp_dir}/bin:${PATH}" GITHUB_REPOSITORY=teagramhq/teagram-desktop \
  MOCK_CACHE_STATE="$cache_state" MOCK_DELETED_IDS="$deleted_ids" \
  bash "$prune_script" > "${temp_dir}/dispatch-cleanup.log"
grep -Fq $'101\tmacOS-arm64-ccache-dev-pr-compatible-' "$cache_state" || fail 'Dispatch cleanup must preserve the scheduled PR-compatible snapshot.'
grep -Fq $'202\tmacOS-arm64-ccache-dev-protected-' "$cache_state" || fail 'Cleanup must retain only the newest protected profile.'
grep -Fq $'199\tmacOS-arm64-ccache-dev-'"${pr_hash}" "$cache_state" || fail 'Cleanup must keep the newest MacOS Packaged. compiler cache.'
for cache_id in 100 198 201; do
  if grep -Fq "${cache_id}"$'\t' "$cache_state"; then
    fail "Cleanup must remove superseded cache id ${cache_id}."
  fi
done

pr_restore_prefix="macOS-arm64-ccache-dev-$(sed -n 's/^name=//p' <<< "$pr_profile")-${pr_hash}-"
if ! awk -F '\t' -v prefix="$pr_restore_prefix" 'index($2, prefix) == 1 { found = 1 } END { exit !found }' "$cache_state"; then
  fail 'A PR restore after dispatch cleanup must find its scheduled profile snapshot.'
fi

printf 'MacOS dev cache cleanup sequence tests passed.\n'
