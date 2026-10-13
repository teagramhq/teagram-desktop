#!/usr/bin/env bash
set -euo pipefail

script=".github/scripts/check_ghcr_image.sh"
temporary_directory="$(mktemp -d)"
trap 'rm -rf "$temporary_directory"' EXIT

cat > "$temporary_directory/docker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$*" != "buildx imagetools inspect ghcr.io/example/dependencies:test" ]]; then
  echo "Unexpected docker arguments: $*" >&2
  exit 2
fi
case "${FAKE_DOCKER_RESULT:-}" in
  found)
    echo "Name: ghcr.io/example/dependencies:test"
    ;;
  missing)
    echo "ERROR: failed to resolve image: not found" >&2
    exit 1
    ;;
  failure)
    echo "ERROR: denied: requested access to the resource is denied" >&2
    exit 1
    ;;
  *)
    echo "Unknown FAKE_DOCKER_RESULT" >&2
    exit 2
    ;;
esac
EOF
chmod +x "$temporary_directory/docker"

export PATH="$temporary_directory:$PATH"
export GITHUB_OUTPUT="$temporary_directory/output"

FAKE_DOCKER_RESULT=found bash "$script" ghcr.io/example/dependencies:test
grep -Fxq 'exists=true' "$GITHUB_OUTPUT"

: > "$GITHUB_OUTPUT"
FAKE_DOCKER_RESULT=missing bash "$script" ghcr.io/example/dependencies:test
grep -Fxq 'exists=false' "$GITHUB_OUTPUT"

: > "$GITHUB_OUTPUT"
if FAKE_DOCKER_RESULT=failure bash "$script" ghcr.io/example/dependencies:test; then
  echo "Registry access failures must not be treated as a missing image." >&2
  exit 1
fi
if [[ -s "$GITHUB_OUTPUT" ]]; then
  echo "A registry access failure must not produce an image-exists result." >&2
  exit 1
fi

printf 'GHCR image check tests passed.\n'
