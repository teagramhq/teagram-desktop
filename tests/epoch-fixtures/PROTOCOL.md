# Fixture protocol and vector report

This is executable fixture evidence only. It does not define production key
custody, integrate an updater, or grant authority to any RFC test key.

## Canonical wire form

Each signed object is a closed-schema JSON object capped at 16,384 bytes. The
detached Ed25519 signature is exactly 64 bytes. The verifier checks size and
signature against a trusted key before parsing. It verifies the exact byte
string `domain || 0x00 || payload`; it does not normalize the payload before
verification.

After parsing, both implementations reserialize the value and require exact
byte equality. Canonical objects have no whitespace, use byte-sorted object
keys, and use no escapes. Keys and strings contain only printable ASCII
`0x20` through `0x7e`, excluding quote (`0x22`) and backslash (`0x5c`). Strings
are nonempty and at most 128 bytes, with `version` and `min_os` limited to 32.
Unknown fields, duplicate keys, numbers outside the listed constants, booleans,
nulls, arrays in an unsupported location, and any alternate byte encoding are
rejected. Hex is lowercase only. Every counter is a decimal string from 0
through `18446744073709551615`, with no leading zero except `"0"`.

| Object | Allowed fields and constraints |
| --- | --- |
| S, epoch statement | `allocation_checkpoint_sha256`: 64 lowercase hex; `authorized_package_keys`: 1 to 4 objects with exactly `algorithm="Ed25519"`, `id`, and 64 lowercase hex `public_key`; `format`: JSON integer `1`; `from_epoch`, `to_epoch`: uint64 decimal strings; `kind="teagram-key-epoch"`; `previous_statement_sha256`: 64 lowercase hex; `product="io.teagram.desktop"`; `repo="teagramhq/teagram-desktop"`; `repo_id="1332987415"` (the GitHub numeric repository ID as a decimal string); `revoked_key_ids`: zero to 4 key IDs. Key IDs match `[a-z0-9][a-z0-9-]{0,31}`. Both arrays are sorted and unique by ID; authorized and revoked IDs are disjoint. |
| M, ordinary package manifest | `arch="arm64"`; `asset_name` matches `Teagram-macOS-arm64-[0-9]+\.zip`; `asset_sha256`: 64 lowercase hex; `asset_size`, `build`, `key_epoch`: uint64 decimal strings; `channel`: `dev` or `main`; `commit`: 40 lowercase hex; `epoch_statement_sha256`: 64 lowercase hex; `format`: JSON integer `2`; `key_id`: key ID; `min_os`, `version`: printable ASCII strings of at most 32 bytes; `product="io.teagram.desktop"`; `repo="teagramhq/teagram-desktop"`. |
| C, ledger checkpoint | `current_epoch`, `event_cursor`, `high_water_build`, `sequence`: uint64 decimal strings; `epoch_statement_sha256`, `ledger_head_sha256`, `prev_sha256`, `protection_digest_sha256`: 64 lowercase hex; `kind="teagram-ledger-checkpoint"`; `product="io.teagram.desktop"`; `repo="teagramhq/teagram-desktop"`; `repo_id="1332987415"` (the GitHub numeric repository ID as a decimal string); `schema`: JSON integer `1`. |

`S` is authorized only by recovery role R. It advances exactly one epoch and
must link to the previous statement. Each accepted statement replaces the
active package-key map while the verifier retains the full key-ID/public-key
history and cumulative revoked-ID set across transitions. A historical key ID
cannot be rebound to another public key, and a revoked ID cannot be
reauthorized even when a later statement omits it from its own revocation list.
`M` must be signed by the exact public key listed for its key ID in the
accepted S for that epoch. `C` is authorized only by ledger role L. The fixture
trust map has four distinct keys and never exceeds four entries.

| Domain | Exact bytes before the NUL separator | Authorized signer |
| --- | --- | --- |
| S | `teagram-key-epoch-v1` | R |
| M | `teagram-update-v2` | authorized K[epoch] |
| C | `teagram-ledger-checkpoint-v1` | L |

The `teagram-update-v1` plus NUL domain is also tested and cannot authenticate
an M object under the v2 verifier.

## Published fixture keys and vectors

Only RFC 8032 section 7.1 test material is used:

| Fixture role | RFC vector |
| --- | --- |
| R, recovery | TEST 1 |
| K[0], package | TEST 2 |
| K[1], package | TEST 3 |
| L, ledger | TEST 1024 |

Each program independently derives all four public keys from the published
seeds, compares them byte-for-byte with the RFC values, signs each original
RFC message, and verifies the published signature. TEST 1024 uses its complete
1023-byte message. These known answers are checked before either program
constructs protocol vectors.

Source: [RFC 8032, section 7.1](https://www.rfc-editor.org/rfc/rfc8032.html#section-7.1).

`rfc8032-vectors.json` holds the published test inputs inside this fixture
directory. `protocol-vectors.json` records the byte-exact canonical payload,
SHA-256, NUL-prefixed message hex, domain, signer, and Ed25519 signature for
S, M, and both the bootstrap C0 and post-reservation C checkpoints. Both
programs independently generate all four rows; the suite compares their
complete result objects and checks the recorded vectors against that output.

## Checked outcomes

The isolated suite checks:

- RFC TEST 1, 2, 3, and 1024 known answers; all public derivations, signatures,
  and verifications match, including TEST 1024's 1023-byte message.
- Identical C++ and Python protocol vectors and cases, including canonical
  bytes, digests, exact domain-prefixed message bytes, and signatures.
- Ten valid-signature wrong-role objects: S by K[0], K[1], and L; M by R and L;
  C by R, K[0], and K[1]; M by K[1] before authorization; and M by revoked
  K[0] after epoch 1. Each reaches parsing with a valid signature, then fails
  at the authority check. An additional K[0]-signed epoch-0 manifest is
  accepted while K[0] is authorized.
- An accepted R-signed S that maps `k1` to the K[0] public key, paired with a
  correctly K[1]-signed M naming `k1`, reaches parsing with a valid signature
  and fails authority because the verifying key differs from the accepted
  ID-to-public-key map.
- A linked, correctly R-signed epoch 1-to-2 S cannot reauthorize cumulative
  revoked ID `k0` or rebind historical ID `k1` to K[0]'s public key; both
  signature-valid objects reach parsing and fail their transition checks. A
  linked statement retaining the original `k1` public key is accepted.
- Correctly K[1]-signed canonical M objects with numeric `asset_name` and
  array-valued `channel` are rejected at schema validation after signature
  verification; neither implementation aborts on the wrong JSON type.
- Correctly R-signed S objects with numeric `repo`, array-valued digest, and
  numeric nested package public key, plus correctly L-signed C objects with
  numeric `repo` or ledger digest, reach parsing and return schema rejection
  in both implementations.
- Correctly signed duplicate keys fail canonical equality; an unknown field,
  leading-zero/number/overflow counters, escaped text, and a trailing newline
  fail after signature verification. Wrong-domain and v1-domain signatures
  fail before parsing.
- The online-signed skip fails authority. A separate R-signed 0-to-2 S fails
  sequencing, and a conflicting same-epoch R-signed S fails conflict checks.
- A re-signed one-byte change in every top-level S, M, and C field reaches
  signature verification and parsing, then is rejected (11 S fields, 15 M
  fields, and 13 C fields). A raw unsigned M-byte flip fails the signature.
- The epoch-0/build-`UINT64_MAX` client accepts the R-authorized sequential
  epoch-1 statement and K[1]-signed build 101. Global H remains 100 when the
  statement is accepted, advances to 101 only for the real reservation, and a
  reservation at `UINT64_MAX` is permanently refused. The C0-to-C checkpoint
  chain and both signatures verify.
- No RFC test public key occurs outside this fixture directory, and no CMake
  file or `Telegram/SourceFiles` path references the fixtures.

The Python implementation uses `cryptography`; the C++ implementation uses
OpenSSL 3 EVP directly. Neither reads the other's generated payload or
signature. Setup and reproduction commands are in `README.md`.

## Observed local run

On 2026-10-07, the documented isolated setup smoke exited 0 with g++ 15.2.0,
OpenSSL headers/runtime 3.5.5 (linked runtime major 3), Python 3.14.4, and
cryptography 46.0.5. The documented focused unittest command exited 0: 11
tests passed in 4.896 seconds. The run verified all four RFC known answers,
byte-identical C++/Python results for S, M, C0 and C, all ten signed wrong-role
cases, the signed manifest key-ID/public-key mismatch, both linked-transition
history/revocation rejections and their valid successor, both signed
wrong-type M schema rejections, the wrong-type S/C schema rejections, all 39
signed field mutations, the
epoch-1/build-101 recovery outcome, and the fixture key-containment check.
This is local protocol-conformance evidence; the earlier setup-only PR #98
check is not counted as conformance.
