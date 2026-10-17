#!/usr/bin/env bash
set -euo pipefail

prefix="$1"
scope="$2"

client_tools=(
  "ccache|https://github.com/ccache/ccache/releases/download/v4.14.1/ccache-4.14.1-darwin.tar.gz|c279fa81e2e806b4d9e64d4a06cb569a84ef54151feef796e5d4f4e95a464563"
  "ninja|https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-mac.zip|c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b"
)
library_tools=(
  "m4|https://ftp.gnu.org/gnu/m4/m4-1.4.21.tar.gz|38ae59f7a30bf9c108193cc5c25fbb06014f21e230c7ede2eff614f7b7c37ed8"
  "autoconf|https://ftp.gnu.org/gnu/autoconf/autoconf-2.73.tar.gz|259ddfa3bddc799cfb81489cc0f17dfdf1bd6d1505dda53c0f45ff60d6a4f9a7"
  "automake|https://ftp.gnu.org/gnu/automake/automake-1.18.1.tar.gz|63e585246d0fc8772dffdee0724f2f988146d1a3f1c756a3dc5cfbefa3c01915"
  "libtool|https://ftp.gnu.org/gnu/libtool/libtool-2.6.2.tar.gz|24adb3aa9ae035c70faba344af57d73215eb89281045af6c7ccd307751f8b0bf"
  "nasm|https://www.nasm.us/pub/nasm/releasebuilds/3.02/nasm-3.02.tar.gz|f504227b2f529e658d41629075f0503b38d67d790af345f34eba4af60c6a5998"
  "meson|https://github.com/mesonbuild/meson/releases/download/1.12.0/meson-1.12.0.tar.gz|88afe0c20e52030218924ac37d0c81c59b4b5f3ae3752c8c6d7470c7d365886c"
)

case "$scope" in
  client) tools=("${client_tools[@]}") ;;
  libraries) tools=("${library_tools[@]}") ;;
  *)
    printf 'unknown scope: %s\n' "$scope" >&2
    exit 1
    ;;
esac

mkdir -p "$prefix/bin" "$prefix/src"
export PATH="$prefix/bin:$PATH"
jobs="$(sysctl -n hw.logicalcpu)"

for tool in "${tools[@]}"; do
  IFS='|' read -r name url sha256 <<< "$tool"
  archive="$prefix/src/${url##*/}"
  curl -fsSL --retry 3 -o "$archive" "$url"
  echo "$sha256  $archive" | shasum -a 256 -c -
  source_dir="$prefix/src/$name"
  rm -rf "$source_dir"
  mkdir -p "$source_dir"
  case "$archive" in
    *.zip) ditto -x -k "$archive" "$source_dir" ;;
    *) tar -xzf "$archive" -C "$source_dir" --strip-components 1 ;;
  esac
  case "$name" in
    ccache|ninja)
      install -m 0755 "$source_dir/$name" "$prefix/bin/$name"
      ;;
    meson)
      printf '#!/bin/sh\nexec python3 "%s/meson.py" "$@"\n' "$source_dir" > "$prefix/bin/meson"
      chmod 0755 "$prefix/bin/meson"
      ;;
    *)
      configure_args=(--prefix="$prefix")
      if [ "$name" = libtool ]; then
        configure_args+=(--program-prefix=g)
      fi
      (
        cd "$source_dir"
        ./configure "${configure_args[@]}" >/dev/null
        make -j"$jobs" >/dev/null
        make install >/dev/null
      )
      if [ "$name" = libtool ]; then
        ln -sf glibtoolize "$prefix/bin/libtoolize"
      fi
      ;;
  esac
  printf '%s %s\n' "$name" "${url##*/}"
done

if [ "$scope" = libraries ]; then
  for command in m4 autoconf automake autoreconf libtoolize glibtool nasm meson; do
    command -v "$command" >/dev/null
  done
fi
