#!/usr/bin/env bash
set -euo pipefail

for argument in "$@"; do
  case "$argument" in
    */window/window_main_menu_helpers.cpp|window_main_menu_helpers.cpp)
      exec "$@"
      ;;
  esac
done

exec ccache "$@"
