#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
resources_dir="${repo_root}/Telegram/Resources"

# SHA-256 of the upstream Telegram Desktop paper-plane artwork each of these
# files carried before it was replaced with the Teagram mug.
upstream_hashes=(
  "art/logo_256.png 3fb1400c7dc9bbc3b5cb3ffedcbf4a9b09c53e28b57a7ff33a8a6b9048864090"
  "art/logo_256_no_margin.png 44af9a76a532559ef8cb450b48096713be819103ab79a19d7e4abb9b32ff4cb7"
  "art/icon16.png ea1f36152b143cc2664daef5026656d55be2230ed42d43628e17ef7d2fff718c"
  "art/icon16@2x.png beecd061f6df3a63a8165bcac0b4acc322f5d096f1439c06e812225cfa3e23ae"
  "art/icon32.png 9e4180c78c895783b4559c5e1a7868a2c9aa30a29969fe5dbe9a4ce4cf5cde6c"
  "art/icon32@2x.png a67e2b8375e8ef28655f8772a85013ce637124971c0307d9d04497233cac400a"
  "art/icon48.png 041b78681a35f07c3c929662fc66b6592c88588dc7297a5394ef7f923f2118e2"
  "art/icon48@2x.png 2f7c026037ddfe12ecd3567f296aa6cbfb98a3c7f278798a9eca5f434f2893f7"
  "art/icon64.png 554dff9f55697d4e8ec69e9aa53678aa5dae3e91aa13adc3b115526d8d51dfc9"
  "art/icon64@2x.png 3072dec3872444fc3c1b8d7900205d3867e2fa312bb7084ac4f820a1ca518b78"
  "art/icon128.png 731431e47a5bc91c697d25c3a54fe7ba004752f5b66e0f282c47588ff7a314e6"
  "art/icon128@2x.png a6e26ac151cea4bf7642ce5ffcfed1dfec938a0c2db2ce26626adb4b14880b9c"
  "art/icon256.png 3fb1400c7dc9bbc3b5cb3ffedcbf4a9b09c53e28b57a7ff33a8a6b9048864090"
  "art/icon256@2x.png c395c36446e752513c3bba8a31da3a0bfd130440be2b71e6c3fa5ea05ffbd7ce"
  "art/icon512.png e297771c75bd2f81d637a3234f83568be62092f67d16946be23895fa92fa7119"
  "art/icon512@2x.png 286fbd6f210511daf5e7d26b62784d5d6abe575d72863b45c409f4e57b93db5d"
  "art/icon_green.png 056a4868177971c9ba1fc94c94c4ac44177f5740b9f578659fd86311530d7ae4"
  "art/iconbig_green.png b0ad1899c9186c5afc4a4a20cf6edb7406d2f2381feca7b9f6c02512842d05dd"
  "art/icon256.ico 6d27da5cf7669e32dc2cf09e331befe8e39045f75f95e0cbfe02c16e675d81bb"
  "icons/mac_tray_icon.png fdd45401382301b3495398a7e40919e5717c2b7239240a843d01ef15ebc54a1c"
  "icons/mac_tray_icon@2x.png ccadde7f1b72e1aec15dc1054be5f36191357df847f883bc0f7d54b439f3b369"
  "icons/mac_tray_icon@3x.png 334731eed269374c2c3cb5836aa70a932fbf2697715bd2574ba4b2c5b4291eb5"
)

failed=0
for entry in "${upstream_hashes[@]}"; do
  name="${entry%% *}"
  upstream="${entry##* }"
  path="${resources_dir}/${name}"
  if [[ ! -f "${path}" ]]; then
    printf 'Missing brand art: Telegram/Resources/%s\n' "${name}" >&2
    failed=1
    continue
  fi
  actual="$(sha256sum "${path}" | cut -d ' ' -f 1)"
  if [[ "${actual}" == "${upstream}" ]]; then
    printf 'Telegram/Resources/%s is the upstream Telegram logo again.\n' "${name}" >&2
    failed=1
  fi
done

if [[ "${failed}" != 0 ]]; then
  exit 1
fi

printf 'Brand art tests passed.\n'
