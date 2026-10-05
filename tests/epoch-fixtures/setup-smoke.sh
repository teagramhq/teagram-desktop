#!/usr/bin/env bash

set -euo pipefail

python_bin=${1:-/usr/bin/python3}
temporary_dir=$(mktemp -d)
trap 'rm -rf "$temporary_dir"' EXIT

if ! command -v g++ >/dev/null 2>&1; then
  echo "g++ is required" >&2
  exit 1
fi
if [[ ! -x "$python_bin" ]]; then
  echo "Python executable not found: $python_bin" >&2
  exit 1
fi

echo "Installed packages:"
dpkg-query -W -f='${Package} ${Version}\n' \
	g++ libssl-dev python3-cryptography

printf 'C++ compiler: '
g++ --version | sed -n '1p'

cat > "$temporary_dir/setup-smoke.cpp" <<'CPP'
#include <iostream>
#include <string>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#if __cplusplus < 201703L
#error C++17 support is required
#endif

#ifndef __GLIBCXX__
#error libstdc++ headers are required
#endif

#if OPENSSL_VERSION_MAJOR != 3
#error OpenSSL 3 headers are required
#endif

int main() {
	const auto values = std::vector<std::string>{"C++17", "libstdc++"};
	if (values.size() != 2) {
		return 1;
	}

	auto context = EVP_MD_CTX_new();
	if (context == nullptr) {
		return 2;
	}
	EVP_MD_CTX_free(context);

	std::cout << "C++17 and libstdc++: available (__GLIBCXX__="
		<< __GLIBCXX__ << ")\n";
	std::cout << "OpenSSL headers: " << OPENSSL_VERSION_TEXT << "\n";
	std::cout << "OpenSSL linked runtime major: "
		<< OPENSSL_version_major() << "\n";
	return (OPENSSL_version_major() == 3) ? 0 : 3;
}
CPP

g++ -std=c++17 -Wall -Wextra -Werror \
	"$temporary_dir/setup-smoke.cpp" -lcrypto \
	-o "$temporary_dir/setup-smoke"
"$temporary_dir/setup-smoke"

"$python_bin" - <<'PY'
from importlib.metadata import version
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

assert callable(Ed25519PrivateKey.from_private_bytes)
assert callable(Ed25519PrivateKey.sign)
assert callable(Ed25519PublicKey.from_public_bytes)
assert callable(Ed25519PublicKey.verify)

print(f"Python: {sys.version.split()[0]}")
print(f"cryptography: {version('cryptography')}")
print("Ed25519 private/public key APIs: available")
PY
