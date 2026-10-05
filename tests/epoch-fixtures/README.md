# Epoch fixture toolchain

This directory is reserved for isolated epoch protocol fixtures. Its contents
are not referenced by application CMake targets.

The setup smoke can run before any protocol fixture exists. From the
repository root, build its isolated Ubuntu 26.04 toolchain image and run it
with the repository mounted read-only:

~~~bash
docker build --tag epoch-fixture-toolchain:local tests/epoch-fixtures
docker run --rm --network none \
  --user "$(id -u):$(id -g)" \
  --cap-drop=ALL \
  --security-opt=no-new-privileges \
  --read-only \
  --tmpfs /tmp:rw,exec,nosuid,nodev,size=64m,mode=1777 \
  --mount "type=bind,source=$PWD,target=/workspace,readonly" \
  --workdir /workspace \
  epoch-fixture-toolchain:local \
  bash tests/epoch-fixtures/setup-smoke.sh /usr/bin/python3
~~~

The smoke compiles and links a temporary C++17 program against OpenSSL 3,
checks the compiler's libstdc++ headers, and verifies that Python can import
the cryptography Ed25519 key APIs. The script removes its temporary build
files when it exits. The container runs as the caller's UID, has no network,
and can write only to its temporary filesystem.

The dedicated GitHub Actions workflow runs the same setup on an ephemeral
Ubuntu 26.04 runner. It has only repository read permission and does not build
the application, use secrets, or publish artifacts. The run records package
and tool versions with the smoke output. These checks establish tool
availability only; they do not claim protocol conformance.
