import copy
import hashlib
import json
import re
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)


ROOT = Path(__file__).resolve().parent
RFC = json.loads((ROOT / "rfc8032-vectors.json").read_text())
ROLE_IDS = {"R": "1", "K0": "2", "K1": "3", "L": "1024"}
DOMAIN_S = "teagram-key-epoch-v1"
DOMAIN_M = "teagram-update-v2"
DOMAIN_C = "teagram-ledger-checkpoint-v1"
DOMAIN_OLD = "teagram-update-v1"
DOMAINS = {"S": DOMAIN_S, "M": DOMAIN_M, "C": DOMAIN_C}
MAX_OBJECT = 16 * 1024
MAX_UINT64 = (1 << 64) - 1
ZERO = "0" * 64
REPO = "teagramhq/teagram-desktop"
PRODUCT = "io.teagram.desktop"
REPO_ID = "1332987415"
COMMIT = "b14d386727689a20de057385799954960ea841e4"


def raw_hex(test_id, field):
    return bytes.fromhex(RFC[test_id][field])


def private_key(test_id):
    return Ed25519PrivateKey.from_private_bytes(raw_hex(test_id, "seed"))


def public_key(test_id):
    return Ed25519PublicKey.from_public_bytes(raw_hex(test_id, "public_key"))


def canonical(value):
    return json.dumps(
        value,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def domain_message(domain, payload):
    return domain.encode("ascii") + b"\0" + payload


def digest(payload):
    return hashlib.sha256(payload).hexdigest()


def sign(test_id, domain, payload):
    return private_key(test_id).sign(domain_message(domain, payload))


def check_rfc_vectors():
    result = []
    for test_id in ("1", "2", "3", "1024"):
        private = private_key(test_id)
        derived = private.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
        expected_public = raw_hex(test_id, "public_key")
        message = raw_hex(test_id, "message")
        signature = private.sign(message)
        expected_signature = raw_hex(test_id, "signature")
        public_key(test_id).verify(signature, message)
        result.append(
            {
                "test": test_id,
                "public_key_matches": derived == expected_public,
                "signature_matches": signature == expected_signature,
                "signature_verifies": True,
                "message_bytes": len(message),
            }
        )
    return result


def role_public_keys():
    result = {}
    for role, test_id in ROLE_IDS.items():
        result[role] = raw_hex(test_id, "public_key")
    if len(set(result.values())) != 4:
        raise ValueError("fixture role keys must be distinct")
    if len(result) > 4:
        raise ValueError("fixture role key map exceeds four keys")
    return result


def signed_by(domain, payload, signature, test_id):
    public_key(test_id).verify(signature, domain_message(domain, payload))
    return True


def find_signer(domain, payload, signature):
    if len(payload) > MAX_OBJECT:
        return None, None, "size"
    if len(signature) != 64:
        return None, None, "signature_length"
    for role, test_id in ROLE_IDS.items():
        try:
            public_key(test_id).verify(signature, domain_message(domain, payload))
            return role, raw_hex(test_id, "public_key").hex(), "ok"
        except Exception:
            pass
    return None, None, "signature"


def is_ascii_string(value, max_bytes=128):
    if not isinstance(value, str):
        return False
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError:
        return False
    return (
        0 < len(encoded) <= max_bytes
        and all(0x20 <= byte <= 0x7E and byte not in (0x22, 0x5C) for byte in encoded)
    )


def check_tree_ascii(value):
    if isinstance(value, str):
        return is_ascii_string(value)
    if isinstance(value, list):
        return all(check_tree_ascii(item) for item in value)
    if isinstance(value, dict):
        return all(is_ascii_string(key) and check_tree_ascii(item) for key, item in value.items())
    if isinstance(value, float) or value is None or isinstance(value, bool):
        return False
    return isinstance(value, int)


def schema_error(kind, value):
    schemas = {
        "S": {
            "allocation_checkpoint_sha256",
            "authorized_package_keys",
            "format",
            "from_epoch",
            "kind",
            "previous_statement_sha256",
            "product",
            "repo",
            "repo_id",
            "revoked_key_ids",
            "to_epoch",
        },
        "M": {
            "arch",
            "asset_name",
            "asset_sha256",
            "asset_size",
            "build",
            "channel",
            "commit",
            "epoch_statement_sha256",
            "format",
            "key_epoch",
            "key_id",
            "min_os",
            "product",
            "repo",
            "version",
        },
        "C": {
            "current_epoch",
            "epoch_statement_sha256",
            "event_cursor",
            "high_water_build",
            "kind",
            "ledger_head_sha256",
            "prev_sha256",
            "product",
            "protection_digest_sha256",
            "repo",
            "repo_id",
            "schema",
            "sequence",
        },
    }
    if not isinstance(value, dict):
        return "schema"
    expected = schemas[kind]
    if set(value) != expected:
        return "schema"
    if not check_tree_ascii(value):
        return "schema"

    if kind == "S":
        for field in (
            "kind",
            "repo",
            "repo_id",
            "product",
            "from_epoch",
            "to_epoch",
            "previous_statement_sha256",
            "allocation_checkpoint_sha256",
        ):
            if not isinstance(value[field], str):
                return "schema"
        if value["kind"] != "teagram-key-epoch" or value["format"] != 1:
            return "schema"
        if value["repo"] != REPO or value["repo_id"] != REPO_ID or value["product"] != PRODUCT:
            return "schema"
        if not valid_uint64(value["repo_id"]):
            return "schema"
        if not valid_uint64(value["from_epoch"]) or not valid_uint64(value["to_epoch"]):
            return "schema"
        if not is_digest(value["previous_statement_sha256"]):
            return "schema"
        if not is_digest(value["allocation_checkpoint_sha256"]):
            return "schema"
        keys = value["authorized_package_keys"]
        revoked = value["revoked_key_ids"]
        if not isinstance(keys, list) or not 1 <= len(keys) <= 4:
            return "schema"
        if not isinstance(revoked, list) or len(revoked) > 4:
            return "schema"
        ids = []
        for key in keys:
            if not isinstance(key, dict) or set(key) != {"algorithm", "id", "public_key"}:
                return "schema"
            if any(not isinstance(key[field], str) for field in ("algorithm", "id", "public_key")):
                return "schema"
            if key["algorithm"] != "Ed25519" or not valid_key_id(key["id"]):
                return "schema"
            if not is_lower_hex(key["public_key"], 64):
                return "schema"
            ids.append(key["id"])
        if ids != sorted(set(ids)):
            return "schema"
        if any(not valid_key_id(key_id) for key_id in revoked):
            return "schema"
        if revoked != sorted(set(revoked)) or set(ids).intersection(revoked):
            return "schema"
    elif kind == "M":
        for field in (
            "repo",
            "product",
            "arch",
            "channel",
            "asset_sha256",
            "commit",
            "epoch_statement_sha256",
            "key_id",
            "asset_name",
            "version",
            "min_os",
        ):
            if not isinstance(value[field], str):
                return "schema"
        if value["format"] != 2:
            return "schema"
        if value["repo"] != REPO or value["product"] != PRODUCT or value["arch"] != "arm64":
            return "schema"
        if value["channel"] not in ("dev", "main"):
            return "schema"
        if not valid_uint64(value["build"]) or not valid_uint64(value["key_epoch"]):
            return "schema"
        if not valid_uint64(value["asset_size"]):
            return "schema"
        if not is_lower_hex(value["asset_sha256"], 64) or not is_lower_hex(value["commit"], 40):
            return "schema"
        if not is_digest(value["epoch_statement_sha256"]) or not valid_key_id(value["key_id"]):
            return "schema"
        if not re.fullmatch(r"Teagram-macOS-arm64-[0-9]+\.zip", value["asset_name"]):
            return "schema"
        if not is_ascii_string(value["version"], 32) or not is_ascii_string(value["min_os"], 32):
            return "schema"
    elif kind == "C":
        for field in (
            "current_epoch",
            "epoch_statement_sha256",
            "event_cursor",
            "high_water_build",
            "kind",
            "ledger_head_sha256",
            "prev_sha256",
            "product",
            "protection_digest_sha256",
            "repo",
            "repo_id",
            "sequence",
        ):
            if not isinstance(value[field], str):
                return "schema"
        if value["kind"] != "teagram-ledger-checkpoint" or value["schema"] != 1:
            return "schema"
        if value["repo"] != REPO or value["repo_id"] != REPO_ID or value["product"] != PRODUCT:
            return "schema"
        for field in ("repo_id", "sequence", "event_cursor", "high_water_build", "current_epoch"):
            if not valid_uint64(value[field]):
                return "schema"
        for field in (
            "prev_sha256",
            "ledger_head_sha256",
            "epoch_statement_sha256",
            "protection_digest_sha256",
        ):
            if not is_digest(value[field]):
                return "schema"
    return "ok"


def valid_uint64(value):
    if not isinstance(value, str) or not re.fullmatch(r"0|[1-9][0-9]{0,19}", value):
        return False
    return int(value) <= MAX_UINT64


def is_lower_hex(value, length):
    return isinstance(value, str) and re.fullmatch(rf"[0-9a-f]{{{length}}}", value) is not None


def is_digest(value):
    return is_lower_hex(value, 64)


def valid_key_id(value):
    return isinstance(value, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]{0,31}", value) is not None


def package_key_map(statement):
    return {
        entry["id"]: entry["public_key"]
        for entry in statement["authorized_package_keys"]
    }


def manifest_context(statement, statement_payload, installed=(0, MAX_UINT64)):
    return {
        "epoch": statement["to_epoch"],
        "authorized_package_keys": package_key_map(statement),
        "statement_digest": digest(statement_payload),
        "installed": installed,
    }


def advance_statement_context(context, statement, statement_payload):
    next_context = dict(context)
    known_statements = dict(context.get("known_statements", {}))
    known_statements[statement["to_epoch"]] = digest(statement_payload)
    key_history = dict(context.get("key_history", {}))
    key_history.update(package_key_map(statement))
    cumulative_revocations = set(context.get("cumulative_revocations", set()))
    cumulative_revocations.update(statement["revoked_key_ids"])
    next_context.update(
        {
            "floor": statement["to_epoch"],
            "previous_statement_sha256": digest(statement_payload),
            "known_statements": known_statements,
            "key_history": key_history,
            "cumulative_revocations": cumulative_revocations,
        }
    )
    return next_context


def verify_object(kind, domain, raw, signature, context=None):
    result = {
        "rejected": True,
        "signature_valid": False,
        "parse_reached": False,
        "stage": "signature",
    }
    expected_domain = DOMAINS.get(kind)
    if expected_domain is None:
        raise ValueError(f"unknown object kind: {kind}")
    if domain != expected_domain:
        result["stage"] = "domain"
        return result
    signer, signer_public_key, status = find_signer(expected_domain, raw, signature)
    if status != "ok":
        result["stage"] = status
        return result
    result["signature_valid"] = True
    result["parse_reached"] = True
    try:
        value = json.loads(raw.decode("utf-8"))
    except Exception:
        result["stage"] = "parse"
        return result
    try:
        round_trip = canonical(value)
    except Exception:
        result["stage"] = "canonical"
        return result
    if round_trip != raw:
        result["stage"] = "canonical"
        return result
    if not check_tree_ascii(value):
        result["stage"] = "schema"
        return result
    try:
        status = schema_error(kind, value)
    except Exception:
        result["stage"] = "schema"
        return result
    if status != "ok":
        result["stage"] = status
        return result
    if (
        (kind == "S" and signer != "R")
        or (kind == "C" and signer != "L")
        or (kind == "M" and signer not in ("K0", "K1"))
    ):
        result["stage"] = "authority"
        return result
    if kind == "S" and context is not None:
        floor = int(context["floor"])
        from_epoch = int(value["from_epoch"])
        to_epoch = int(value["to_epoch"])
        if from_epoch != floor or from_epoch == MAX_UINT64 or to_epoch != from_epoch + 1:
            result["stage"] = "sequence"
            return result
        if value["previous_statement_sha256"] != context["previous_statement_sha256"]:
            result["stage"] = "sequence"
            return result
        known = context.get("known_statements", {})
        existing = known.get(str(to_epoch))
        if existing is not None and existing != digest(raw):
            result["stage"] = "conflict"
            return result
        history = context.get("key_history", {})
        revoked = context.get("cumulative_revocations", set())
        for key_id, public_key in package_key_map(value).items():
            if key_id in revoked:
                result["stage"] = "revocation"
                return result
            previous_key = history.get(key_id)
            if previous_key is not None and previous_key != public_key:
                result["stage"] = "key_history"
                return result
    if kind == "M":
        context = context or {
            "epoch": "1",
            "authorized_package_keys": {"k1": raw_hex("3", "public_key").hex()},
        }
        authorized_package_keys = context.get("authorized_package_keys", {})
        if (
            value["key_epoch"] != context["epoch"]
            or authorized_package_keys.get(value["key_id"]) != signer_public_key
        ):
            result["stage"] = "authority"
            return result
        if value["epoch_statement_sha256"] != context.get("statement_digest"):
            result["stage"] = "authority"
            return result
        installed = context.get("installed", (0, MAX_UINT64))
        candidate = (int(value["key_epoch"]), int(value["build"]))
        if candidate <= installed:
            result["stage"] = "eligibility"
            return result
        if context.get("channel") == "main" and value["channel"] == "dev":
            result["stage"] = "eligibility"
            return result
    result["rejected"] = False
    result["stage"] = "accepted"
    return result


def make_objects():
    pub_k1 = raw_hex("3", "public_key").hex()
    c0 = {
        "current_epoch": "0",
        "epoch_statement_sha256": ZERO,
        "event_cursor": "0",
        "high_water_build": "100",
        "kind": "teagram-ledger-checkpoint",
        "ledger_head_sha256": digest(b"fixture-ledger-bootstrap"),
        "prev_sha256": ZERO,
        "product": PRODUCT,
        "protection_digest_sha256": digest(b"fixture-protection-v1"),
        "repo": REPO,
        "repo_id": REPO_ID,
        "schema": 1,
        "sequence": "0",
    }
    c0_bytes = canonical(c0)
    c0_digest = digest(c0_bytes)
    s = {
        "allocation_checkpoint_sha256": c0_digest,
        "authorized_package_keys": [
            {"algorithm": "Ed25519", "id": "k1", "public_key": pub_k1}
        ],
        "format": 1,
        "from_epoch": "0",
        "kind": "teagram-key-epoch",
        "previous_statement_sha256": ZERO,
        "product": PRODUCT,
        "repo": REPO,
        "repo_id": REPO_ID,
        "revoked_key_ids": ["k0"],
        "to_epoch": "1",
    }
    s_bytes = canonical(s)
    s_digest = digest(s_bytes)
    m = {
        "arch": "arm64",
        "asset_name": "Teagram-macOS-arm64-101.zip",
        "asset_sha256": digest(b"test-only-package-bytes"),
        "asset_size": "4096",
        "build": "101",
        "channel": "dev",
        "commit": COMMIT,
        "epoch_statement_sha256": s_digest,
        "format": 2,
        "key_epoch": "1",
        "key_id": "k1",
        "min_os": "13.0",
        "product": PRODUCT,
        "repo": REPO,
        "version": "7.0.9",
    }
    m_bytes = canonical(m)
    c1 = {
        "current_epoch": "1",
        "epoch_statement_sha256": s_digest,
        "event_cursor": "1",
        "high_water_build": "101",
        "kind": "teagram-ledger-checkpoint",
        "ledger_head_sha256": digest(b"fixture-reservation-101"),
        "prev_sha256": c0_digest,
        "product": PRODUCT,
        "protection_digest_sha256": c0["protection_digest_sha256"],
        "repo": REPO,
        "repo_id": REPO_ID,
        "schema": 1,
        "sequence": "1",
    }
    return {"S": s, "M": m, "C0": c0, "C": c1}, {
        "S": s_bytes,
        "M": m_bytes,
        "C0": c0_bytes,
        "C": canonical(c1),
    }


def vector_rows(objects, payloads):
    result = {}
    config = {
        "S": ("teagram-key-epoch-v1", "1", "R"),
        "M": ("teagram-update-v2", "3", "K1"),
        "C0": ("teagram-ledger-checkpoint-v1", "1024", "L"),
        "C": ("teagram-ledger-checkpoint-v1", "1024", "L"),
    }
    for name, payload in payloads.items():
        domain, signer_test, signer = config[name]
        signature = sign(signer_test, domain, payload)
        result[name] = {
            "domain": domain,
            "signer": signer,
            "canonical_hex": payload.hex(),
            "sha256": digest(payload),
            "message_hex": domain_message(domain, payload).hex(),
            "signature_hex": signature.hex(),
        }
    return result


def case(kind, domain, payload, signer_test, context=None, signature=None):
    signature = signature if signature is not None else sign(signer_test, domain, payload)
    outcome = verify_object(kind, domain, payload, signature, context)
    outcome["signature_valid"] = outcome["signature_valid"] and signed_by(
        domain,
        payload,
        signature,
        signer_test,
    )
    return outcome


def mutate_first_string(value):
    if isinstance(value, str):
        return "\x7f" + value[1:], True
    if isinstance(value, list):
        for index, item in enumerate(value):
            changed, did_change = mutate_first_string(item)
            if did_change:
                copied = list(value)
                copied[index] = changed
                return copied, True
    if isinstance(value, dict):
        for key in sorted(value):
            changed, did_change = mutate_first_string(value[key])
            if did_change:
                copied = dict(value)
                copied[key] = changed
                return copied, True
    return value, False


def signed_field_flip_cases(kind, domain, value, signer_test, context):
    result = []
    for field in sorted(value):
        changed = copy.deepcopy(value)
        item = changed[field]
        if isinstance(item, int) and not isinstance(item, bool):
            changed[field] = item + 1
        else:
            mutated, did_change = mutate_first_string(item)
            if not did_change:
                raise ValueError(f"no mutable string in {kind}.{field}")
            changed[field] = mutated
        payload = canonical(changed)
        outcome = case(kind, domain, payload, signer_test, context)
        if not outcome["rejected"] or not outcome["signature_valid"] or not outcome["parse_reached"]:
            raise AssertionError(f"signed field flip was not rejected after verification: {kind}.{field}")
        result.append(outcome)
    return len(result)


def duplicate_payload(payload, key):
    value = json.loads(payload)
    encoded = canonical(value).decode("ascii")
    token = json.dumps(key) + ":"
    position = encoded.index(token) + len(token)
    end = encoded.index(",", position)
    pair = encoded[encoded.rfind(",", 0, position) + 1 : end]
    return (encoded[: end + 1] + pair + "," + encoded[end + 1 :]).encode("ascii")


def run_cases(objects, payloads):
    context0 = {
        "epoch": "0",
        "authorized_package_keys": {"k0": raw_hex("2", "public_key").hex()},
        "statement_digest": ZERO,
        "installed": (0, MAX_UINT64),
    }
    s = objects["S"]
    m = objects["M"]
    c = objects["C"]
    context1 = manifest_context(s, payloads["S"])
    cases = {}

    duplicate = duplicate_payload(payloads["S"], "format")
    cases["duplicate_key_S"] = case("S", DOMAIN_S, duplicate, "1")
    cases["signed_duplicate_S"] = dict(cases["duplicate_key_S"])

    unknown = dict(s)
    unknown["unknown_field"] = "x"
    cases["unknown_key_S"] = case("S", DOMAIN_S, canonical(unknown), "1")
    bad_statement = copy.deepcopy(s)
    bad_statement["repo"] = 101
    cases["S_repo_number"] = case("S", DOMAIN_S, canonical(bad_statement), "1")
    bad_statement = copy.deepcopy(s)
    bad_statement["previous_statement_sha256"] = ["0" * 64]
    cases["S_previous_digest_array"] = case("S", DOMAIN_S, canonical(bad_statement), "1")
    bad_statement = copy.deepcopy(s)
    bad_statement["authorized_package_keys"][0]["public_key"] = 101
    cases["S_key_public_key_number"] = case("S", DOMAIN_S, canonical(bad_statement), "1")
    bad_checkpoint = copy.deepcopy(c)
    bad_checkpoint["repo"] = 101
    cases["C_repo_number"] = case("C", DOMAIN_C, canonical(bad_checkpoint), "1024")
    bad_checkpoint = copy.deepcopy(c)
    bad_checkpoint["ledger_head_sha256"] = 101
    cases["C_ledger_head_digest_number"] = case("C", DOMAIN_C, canonical(bad_checkpoint), "1024")

    bad = dict(m)
    bad["build"] = "0101"
    cases["leading_zero_build_M"] = case("M", DOMAIN_M, canonical(bad), "3", context1)
    bad = dict(m)
    bad["build"] = 101
    cases["number_build_M"] = case("M", DOMAIN_M, canonical(bad), "3", context1)
    bad = dict(m)
    bad["build"] = "18446744073709551616"
    cases["overflow_build_M"] = case("M", DOMAIN_M, canonical(bad), "3", context1)
    bad = dict(m)
    bad["asset_name"] = 101
    cases["M_asset_name_number"] = case("M", DOMAIN_M, canonical(bad), "3", context1)
    bad = dict(m)
    bad["channel"] = ["dev"]
    cases["M_channel_array"] = case("M", DOMAIN_M, canonical(bad), "3", context1)

    escaped = payloads["M"].replace(b'"channel":"dev"', b'"channel":"d\\u0065v"')
    cases["escaped_string_M"] = case("M", DOMAIN_M, escaped, "3", context1)
    cases["trailing_newline_M"] = case("M", DOMAIN_M, payloads["M"] + b"\n", "3", context1)
    cases["signed_duplicate_S"] = case("S", DOMAIN_S, duplicate, "1")

    wrong_domain_signature = sign("1", DOMAIN_S, payloads["S"])
    cases["wrong_domain_S"] = case(
        "S",
        DOMAIN_M,
        payloads["S"],
        "1",
        signature=wrong_domain_signature,
    )
    cases["old_v1_domain_for_M"] = case(
        "M",
        DOMAIN_M,
        payloads["M"],
        "3",
        context1,
        signature=sign("3", DOMAIN_OLD, payloads["M"]),
    )
    changed = bytearray(payloads["M"])
    changed[changed.index(b'"channel":"dev"') + len(b'"channel":"d')] ^= 1
    cases["unsigned_one_byte_flip_M"] = case(
        "M",
        DOMAIN_M,
        bytes(changed),
        "3",
        context1,
        signature=sign("3", DOMAIN_M, payloads["M"]),
    )

    wrong_role = {
        "S_by_K0": ("S", DOMAIN_S, payloads["S"], "2", None),
        "S_by_K1": ("S", DOMAIN_S, payloads["S"], "3", None),
        "S_by_ledger": ("S", DOMAIN_S, payloads["S"], "1024", None),
        "M_by_R": ("M", DOMAIN_M, payloads["M"], "1", context1),
        "M_by_ledger": ("M", DOMAIN_M, payloads["M"], "1024", context1),
        "C_by_R": ("C", DOMAIN_C, payloads["C"], "1", None),
        "C_by_K0": ("C", DOMAIN_C, payloads["C"], "2", None),
        "C_by_K1": ("C", DOMAIN_C, payloads["C"], "3", None),
        "M_by_K1_before_authorization": ("M", DOMAIN_M, payloads["M"], "3", context0),
    }
    package0_manifest = dict(m)
    package0_manifest.update(
        {
            "build": "101",
            "epoch_statement_sha256": ZERO,
            "key_epoch": "0",
            "key_id": "k0",
        }
    )
    context0_authorized = {
        "epoch": "0",
        "authorized_package_keys": {"k0": raw_hex("2", "public_key").hex()},
        "statement_digest": ZERO,
        "installed": (0, 100),
    }
    cases["M_by_K0_when_authorized"] = case(
        "M", DOMAIN_M, canonical(package0_manifest), "2", context0_authorized
    )
    old_manifest = dict(m)
    old_manifest.update(
        {
            "build": "101",
            "epoch_statement_sha256": ZERO,
            "key_epoch": "0",
            "key_id": "k0",
        }
    )
    wrong_role["M_by_K0_after_revocation"] = (
        "M",
        DOMAIN_M,
        canonical(old_manifest),
        "2",
        context1,
    )
    mismatch_statement = copy.deepcopy(s)
    mismatch_statement["authorized_package_keys"][0]["public_key"] = raw_hex(
        "2", "public_key"
    ).hex()
    mismatch_statement["revoked_key_ids"] = []
    mismatch_payload = canonical(mismatch_statement)
    if case("S", DOMAIN_S, mismatch_payload, "1")["rejected"]:
        raise AssertionError("fixture mismatch statement must be R-authorized")
    mismatch_context = manifest_context(mismatch_statement, mismatch_payload)
    mismatch_manifest = dict(m)
    mismatch_manifest["epoch_statement_sha256"] = mismatch_context["statement_digest"]
    cases["M_key_id_public_key_mismatch"] = case(
        "M", DOMAIN_M, canonical(mismatch_manifest), "3", mismatch_context
    )
    for name, (kind, domain, payload, signer_test, context) in wrong_role.items():
        cases[name] = case(kind, domain, payload, signer_test, context)
    cases["wrong_role_matrix"] = sorted(wrong_role)

    skip = dict(s)
    skip.update({"from_epoch": "0", "to_epoch": "2"})
    cases["online_signed_epoch_skip"] = case("S", DOMAIN_S, canonical(skip), "2")
    sequence_context = {
        "floor": "0",
        "previous_statement_sha256": ZERO,
        "known_statements": {"1": digest(payloads["S"])},
    }
    cases["signed_epoch_skip_R"] = case(
        "S", DOMAIN_S, canonical(skip), "1", sequence_context
    )

    conflict = dict(s)
    conflict["allocation_checkpoint_sha256"] = digest(b"different-signed-checkpoint")
    conflict_payload = canonical(conflict)
    conflict_context = {
        "floor": "0",
        "previous_statement_sha256": ZERO,
        "known_statements": {"1": digest(payloads["S"])},
    }
    conflict_outcome = case(
        "S", DOMAIN_S, conflict_payload, "1", conflict_context
    )
    cases["signed_conflicting_epoch_R"] = conflict_outcome

    initial_statement_context = {
        "floor": "0",
        "previous_statement_sha256": ZERO,
        "known_statements": {},
        "key_history": {"k0": raw_hex("2", "public_key").hex()},
        "cumulative_revocations": set(),
    }
    initial_statement = case(
        "S", DOMAIN_S, payloads["S"], "1", initial_statement_context
    )
    if initial_statement["rejected"]:
        raise AssertionError("fixture epoch 1 statement must advance from the baseline")
    statement_context1 = advance_statement_context(
        initial_statement_context, s, payloads["S"]
    )
    linked = copy.deepcopy(s)
    linked.update(
        {
            "from_epoch": "1",
            "to_epoch": "2",
            "previous_statement_sha256": digest(payloads["S"]),
            "revoked_key_ids": [],
        }
    )
    revoked_key = copy.deepcopy(linked)
    revoked_key["authorized_package_keys"] = [
        {
            "algorithm": "Ed25519",
            "id": "k0",
            "public_key": raw_hex("2", "public_key").hex(),
        }
    ]
    cases["S_reauthorizes_cumulative_revocation"] = case(
        "S", DOMAIN_S, canonical(revoked_key), "1", statement_context1
    )
    rebound_key = copy.deepcopy(linked)
    rebound_key["authorized_package_keys"] = [
        {
            "algorithm": "Ed25519",
            "id": "k1",
            "public_key": raw_hex("2", "public_key").hex(),
        }
    ]
    cases["S_rebinds_historical_key_id"] = case(
        "S", DOMAIN_S, canonical(rebound_key), "1", statement_context1
    )
    cases["S_accepts_linked_epoch_2"] = case(
        "S", DOMAIN_S, canonical(linked), "1", statement_context1
    )

    flip_counts = {}
    flip_counts["signed_one_byte_flip_fields_S"] = signed_field_flip_cases(
        "S", DOMAIN_S, s, "1", None
    )
    flip_counts["signed_one_byte_flip_fields_M"] = signed_field_flip_cases(
        "M", DOMAIN_M, m, "3", context1
    )
    flip_counts["signed_one_byte_flip_fields_C"] = signed_field_flip_cases(
        "C", DOMAIN_C, c, "1024", None
    )
    flip_counts["signed_one_byte_flip_fields"] = sum(flip_counts.values())

    oversized = payloads["M"] + (b" " * (MAX_OBJECT + 1 - len(payloads["M"])))
    cases["oversized_object"] = case("M", DOMAIN_M, oversized, "3", context1)
    short_sig = sign("3", DOMAIN_M, payloads["M"])[:-1]
    cases["short_signature"] = case("M", DOMAIN_M, payloads["M"], "3", context1, short_sig)

    result = {name: cases[name] for name in sorted(cases)}
    result.update(flip_counts)
    return result


def checkpoint_chain_valid(c0, c1, payloads):
    return (
        not verify_object(
            "C", DOMAIN_C, payloads["C0"], sign("1024", DOMAIN_C, payloads["C0"])
        )["rejected"]
        and not verify_object(
            "C", DOMAIN_C, payloads["C"], sign("1024", DOMAIN_C, payloads["C"])
        )["rejected"]
        and c0["sequence"] == "0"
        and c1["sequence"] == "1"
        and c1["prev_sha256"] == digest(payloads["C0"])
        and c0["high_water_build"] == "100"
        and c1["high_water_build"] == "101"
        and c1["current_epoch"] == "1"
        and c1["epoch_statement_sha256"] == digest(payloads["S"])
    )


def reserve_build(high_water):
    if high_water == MAX_UINT64:
        return None
    return high_water + 1


def state_result(payloads, s, m):
    before = 100
    statement = verify_object(
        "S",
        DOMAIN_S,
        payloads["S"],
        sign("1", DOMAIN_S, payloads["S"]),
        {
            "floor": "0",
            "previous_statement_sha256": ZERO,
            "known_statements": {},
        },
    )
    after_statement = before if not statement["rejected"] else MAX_UINT64
    reserved_value = reserve_build(after_statement)
    reserved = reserved_value if reserved_value is not None else MAX_UINT64
    context = manifest_context(s, payloads["S"])
    accepted = not verify_object(
        "M",
        DOMAIN_M,
        payloads["M"],
        sign("3", DOMAIN_M, payloads["M"]),
        context,
    )["rejected"]
    return {
        "client_accepts_epoch1_build101_from_epoch0_max": accepted,
        "global_h_before_reservation": str(before),
        "global_h_after_statement": str(after_statement),
        "global_h_after_real_reservation": str(reserved),
        "legitimate_h_max_permanently_stops": reserve_build(MAX_UINT64) is None,
    }


def run():
    role_public_keys()
    rfc = check_rfc_vectors()
    if not all(row["public_key_matches"] and row["signature_matches"] for row in rfc):
        raise AssertionError("RFC 8032 known-answer check failed")
    objects, payloads = make_objects()
    vectors = vector_rows(objects, payloads)
    cases = run_cases(objects, payloads)
    state = state_result(payloads, objects["S"], objects["M"])
    if not checkpoint_chain_valid(objects["C0"], objects["C"], payloads):
        raise AssertionError("checkpoint chain fixture failed")
    state["checkpoint_chain_valid"] = True
    return {"rfc": rfc, "vectors": vectors, "cases": cases, "state": state}


if __name__ == "__main__":
    print(json.dumps(run(), separators=(",", ":"), sort_keys=True))
