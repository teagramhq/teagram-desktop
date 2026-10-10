# Epoch fixture conformance

The files in this directory are isolated protocol fixtures. No application
source or CMake target references them. The independent C++17/OpenSSL and
Python implementations derive their own bytes and signatures from the RFC
test seeds, then compare complete results in `test_conformance.py`.

## Reproduce

The isolated toolchain was introduced by PR #98 at setup revision
`82d8a5f41e81305880bd91e840cfa2150e651683`, merged into `dev` as
`e3f6c8f9db908061df11a5db4233ece2aa5ba195`. Its setup smoke establishes tool
availability only; the conformance command below supplies the protocol proof.

From the repository root, build the documented Ubuntu 26.04 toolchain image:

~~~bash
docker build --tag epoch-fixture-toolchain:local tests/epoch-fixtures
~~~

Run the setup smoke and focused conformance suite with no network, dropped
capabilities, a read-only repository mount, and the caller's UID:

~~~bash
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

docker run --rm --network none \
  --user "$(id -u):$(id -g)" \
  --cap-drop=ALL \
  --security-opt=no-new-privileges \
  --read-only \
  --tmpfs /tmp:rw,exec,nosuid,nodev,size=64m,mode=1777 \
  --mount "type=bind,source=$PWD,target=/workspace,readonly" \
  --workdir /workspace \
  epoch-fixture-toolchain:local \
  /usr/bin/python3 -m unittest discover \
    -s tests/epoch-fixtures -p 'test_*.py' -v
~~~

The setup smoke checks the installed C++17 compiler, libstdc++ headers,
OpenSSL 3 headers/runtime, and Python Ed25519 APIs. The focused suite compiles
the C++ verifier in a temporary directory, runs both implementations, and
compares their RFC results, protocol vectors, rejection stages, and state
outcomes. `protocol-vectors.json` is checked against the independently
generated results on every run.

## Evidence

`PROTOCOL.md` pins the closed schemas, canonical encoding, domain bytes,
fixture trust map, vector artifacts, and checked outcomes. The dedicated
GitHub Actions workflow runs the same setup and conformance commands on an
ephemeral Ubuntu 26.04 runner. It has repository read permission only and does
not build the application, use secrets, or publish artifacts.
