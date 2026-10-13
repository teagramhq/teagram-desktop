#!/usr/bin/env bash
set -euo pipefail

contract_script=".github/scripts/test_ci_cache_contract.sh"
linux_workflow=".github/workflows/linux.yml"
temporary_directory="$(mktemp -d)"
trap 'rm -rf "$temporary_directory"' EXIT

bash "$contract_script"

unrestricted_workflow="$temporary_directory/linux.yml"
awk '
  /^      - name:/ {
    in_save_step = ($0 == "      - name: Save Telegram Desktop cache.")
  }
  in_save_step && /^[[:space:]]*&& github\.ref_name == github\.event\.repository\.default_branch$/ { next }
  { print }
' "$linux_workflow" > "$unrestricted_workflow"

if cmp -s "$linux_workflow" "$unrestricted_workflow"; then
  echo "The regression must remove the Linux compiler-cache save branch guard." >&2
  exit 1
fi

if LINUX_CI_CONTRACT_WORKFLOW="$unrestricted_workflow" bash "$contract_script" \
  > "$temporary_directory/output" 2>&1; then
  echo "Removing the Linux compiler-cache save branch guard must fail the contract." >&2
  exit 1
fi
if ! grep -Fxq 'Only the default branch may save Linux compiler caches.' "$temporary_directory/output"; then
  cat "$temporary_directory/output" >&2
  echo "The contract must fail on the missing Linux compiler-cache save branch guard." >&2
  exit 1
fi

printf 'CI cache contract regression passed.\n'
