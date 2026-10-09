#!/usr/bin/env bash
set -euo pipefail

scripts_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
launcher_script="${scripts_dir}/ccache_launcher.sh"
temporary_dir="$(mktemp -d)"
trap 'rm -rf "$temporary_dir"' EXIT

cat > "$temporary_dir/ccache" <<'SH'
#!/usr/bin/env bash
printf 'ccache:%s\n' "$*" >> "$LAUNCH_LOG"
SH

cat > "$temporary_dir/compiler" <<'SH'
#!/usr/bin/env bash
printf 'compiler:%s\n' "$*" >> "$LAUNCH_LOG"
SH

chmod +x "$temporary_dir/ccache" "$temporary_dir/compiler"
export PATH="$temporary_dir:$PATH"
export LAUNCH_LOG="$temporary_dir/launch.log"

"$launcher_script" "$temporary_dir/compiler" -c "$temporary_dir/cacheable.cpp"
"$launcher_script" "$temporary_dir/compiler" -c \
  "$temporary_dir/Telegram/SourceFiles/window/window_main_menu_helpers.cpp"

grep -Fq "ccache:$temporary_dir/compiler -c $temporary_dir/cacheable.cpp" "$LAUNCH_LOG"
grep -Fq "compiler:-c $temporary_dir/Telegram/SourceFiles/window/window_main_menu_helpers.cpp" "$LAUNCH_LOG"
if grep -Fq "ccache:$temporary_dir/compiler -c $temporary_dir/Telegram/SourceFiles/window/window_main_menu_helpers.cpp" "$LAUNCH_LOG"; then
  printf 'The __DATE__ translation unit must bypass ccache.\n' >&2
  exit 1
fi

printf 'ccache launcher tests passed.\n'
