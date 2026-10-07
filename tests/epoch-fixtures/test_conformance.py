import json
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


FIXTURE_DIR = Path(__file__).resolve().parent
REPO_ROOT = FIXTURE_DIR.parents[1]


class EpochConformanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.cpp_binary = Path(cls.build.name) / "epoch-fixture-cpp"
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(FIXTURE_DIR / "cpp_impl.cpp"),
                "-lcrypto",
                "-o",
                str(cls.cpp_binary),
            ],
            check=True,
            cwd=FIXTURE_DIR,
        )
        cls.python_result = json.loads(
            subprocess.check_output(
                [sys.executable, str(FIXTURE_DIR / "python_impl.py")],
                cwd=FIXTURE_DIR,
                text=True,
            )
        )
        cls.cpp_result = json.loads(
            subprocess.check_output(
                [str(cls.cpp_binary)],
                cwd=FIXTURE_DIR,
                text=True,
            )
        )

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def test_rfc_known_answers_precede_protocol_vectors(self):
        self.assertEqual(
            [row["test"] for row in self.python_result["rfc"]],
            ["1", "2", "3", "1024"],
        )
        for row in self.python_result["rfc"]:
            self.assertEqual(row["public_key_matches"], True)
            self.assertEqual(row["signature_matches"], True)
            self.assertEqual(row["signature_verifies"], True)
        self.assertEqual(self.python_result["rfc"][-1]["message_bytes"], 1023)

    def test_both_implementations_independently_match_vectors_and_cases(self):
        self.assertEqual(self.python_result, self.cpp_result)
        self.assertEqual(
            set(self.python_result["vectors"]),
            {"S", "M", "C0", "C"},
        )
        self.assertEqual(
            self.python_result["vectors"],
            json.loads((FIXTURE_DIR / "protocol-vectors.json").read_text()),
        )
        for row in self.python_result["vectors"].values():
            self.assertTrue(row["canonical_hex"])
            self.assertEqual(len(row["sha256"]), 64)
            canonical_bytes = bytes.fromhex(row["canonical_hex"])
            self.assertEqual(row["sha256"], hashlib.sha256(canonical_bytes).hexdigest())
            message = row["domain"].encode("ascii") + b"\0" + canonical_bytes
            self.assertEqual(row["message_hex"], message.hex())
            self.assertEqual(len(row["signature_hex"]), 128)

    def test_noncanonical_and_semantic_cases_reach_their_rejection_stage(self):
        cases = self.python_result["cases"]
        expected = {
            "duplicate_key_S",
            "unknown_key_S",
            "leading_zero_build_M",
            "number_build_M",
            "overflow_build_M",
            "escaped_string_M",
            "trailing_newline_M",
            "signed_duplicate_S",
            "signed_epoch_skip_R",
            "signed_conflicting_epoch_R",
            "signed_unlinked_known_epoch_R",
        }
        self.assertTrue(expected.issubset(cases))
        for name in expected:
            self.assertTrue(cases[name]["rejected"], name)
            self.assertTrue(cases[name]["signature_valid"], name)
            self.assertTrue(cases[name]["parse_reached"], name)

    def test_wrong_role_matrix_uses_valid_signatures_then_rejects_authority(self):
        expected = {
            "S_by_K0",
            "S_by_K1",
            "S_by_ledger",
            "M_by_R",
            "M_by_ledger",
            "C_by_R",
            "C_by_K0",
            "C_by_K1",
            "M_by_K1_before_authorization",
            "M_by_K0_after_revocation",
        }
        cases = self.python_result["cases"]
        self.assertEqual(cases["wrong_role_matrix"], sorted(expected))
        for name in expected:
            self.assertTrue(cases[name]["rejected"], name)
            self.assertTrue(cases[name]["signature_valid"], name)
            self.assertTrue(cases[name]["parse_reached"], name)
            self.assertEqual(cases[name]["stage"], "authority", name)
        self.assertEqual(cases["online_signed_epoch_skip"]["stage"], "authority")
        self.assertEqual(cases["signed_epoch_skip_R"]["stage"], "sequence")
        self.assertEqual(cases["signed_conflicting_epoch_R"]["stage"], "conflict")
        self.assertEqual(cases["signed_unlinked_known_epoch_R"]["stage"], "sequence")
        self.assertFalse(cases["M_by_K0_when_authorized"]["rejected"])
        self.assertTrue(cases["M_by_K0_when_authorized"]["signature_valid"])
        self.assertEqual(cases["M_by_K0_when_authorized"]["stage"], "accepted")

    def test_manifest_signer_matches_public_key_authorized_by_epoch_statement(self):
        for implementation in (self.python_result, self.cpp_result):
            case = implementation["cases"].get("M_key_id_public_key_mismatch")
            self.assertIsNotNone(case)
            self.assertTrue(case["rejected"])
            self.assertTrue(case["signature_valid"])
            self.assertTrue(case["parse_reached"])
            self.assertEqual(case["stage"], "authority")

    def test_linked_epoch_transitions_retain_key_history_and_revocations(self):
        expected_stages = {
            "S_reauthorizes_cumulative_revocation": "revocation",
            "S_rebinds_historical_key_id": "key_history",
        }
        for implementation_name, implementation in (
            ("Python", self.python_result),
            ("C++", self.cpp_result),
        ):
            cases = implementation["cases"]
            for name, stage in expected_stages.items():
                with self.subTest(implementation=implementation_name, name=name):
                    self.assertIn(name, cases)
                    self.assertTrue(cases[name]["rejected"])
                    self.assertTrue(cases[name]["signature_valid"])
                    self.assertTrue(cases[name]["parse_reached"])
                    self.assertEqual(cases[name]["stage"], stage)
            valid = cases["S_accepts_linked_epoch_2"]
            self.assertFalse(valid["rejected"])
            self.assertTrue(valid["signature_valid"])
            self.assertTrue(valid["parse_reached"])
            self.assertEqual(valid["stage"], "accepted")

    def test_signed_wrong_type_manifest_fields_are_schema_rejections(self):
        for implementation_name, implementation in (
            ("Python", self.python_result),
            ("C++", self.cpp_result),
        ):
            cases = implementation["cases"]
            for name in ("M_asset_name_number", "M_channel_array"):
                with self.subTest(implementation=implementation_name, name=name):
                    self.assertIn(name, cases)
                    self.assertTrue(cases[name]["rejected"])
                    self.assertTrue(cases[name]["signature_valid"])
                    self.assertTrue(cases[name]["parse_reached"])
                    self.assertEqual(cases[name]["stage"], "schema")

    def test_signed_wrong_type_statement_and_checkpoint_fields_are_schema_rejections(self):
        expected = (
            "S_repo_number",
            "S_previous_digest_array",
            "S_key_public_key_number",
            "C_repo_number",
            "C_ledger_head_digest_number",
        )
        for implementation_name, implementation in (
            ("Python", self.python_result),
            ("C++", self.cpp_result),
        ):
            cases = implementation["cases"]
            for name in expected:
                with self.subTest(implementation=implementation_name, name=name):
                    self.assertIn(name, cases)
                    self.assertTrue(cases[name]["rejected"])
                    self.assertTrue(cases[name]["signature_valid"])
                    self.assertTrue(cases[name]["parse_reached"])
                    self.assertEqual(cases[name]["stage"], "schema")

    def test_signature_domain_ordering_and_one_byte_tampering(self):
        domain_cases = {
            "S_signed_with_M_domain",
            "S_signed_with_C_domain",
            "M_signed_with_S_domain",
            "M_signed_with_C_domain",
            "C_signed_with_S_domain",
            "C_signed_with_M_domain",
        }
        field_stages = {
            "S": {
                "allocation_checkpoint_sha256": ("signature", False),
                "authorized_package_keys": ("schema", True),
                "format": ("schema", True),
                "from_epoch": ("sequence", True),
                "kind": ("schema", True),
                "previous_statement_sha256": ("sequence", True),
                "product": ("schema", True),
                "repo": ("schema", True),
                "repo_id": ("schema", True),
                "revoked_key_ids": ("schema", True),
                "to_epoch": ("sequence", True),
            },
            "M": {
                "arch": ("schema", True),
                "asset_name": ("schema", True),
                "asset_sha256": ("signature", False),
                "asset_size": ("signature", False),
                "build": ("eligibility", True),
                "channel": ("schema", True),
                "commit": ("signature", False),
                "epoch_statement_sha256": ("authority", True),
                "format": ("schema", True),
                "key_epoch": ("authority", True),
                "key_id": ("authority", True),
                "min_os": ("signature", False),
                "product": ("schema", True),
                "repo": ("schema", True),
                "version": ("signature", False),
            },
            "C": {
                "current_epoch": ("signature", False),
                "epoch_statement_sha256": ("signature", False),
                "event_cursor": ("signature", False),
                "high_water_build": ("signature", False),
                "kind": ("schema", True),
                "ledger_head_sha256": ("signature", False),
                "prev_sha256": ("signature", False),
                "product": ("schema", True),
                "protection_digest_sha256": ("signature", False),
                "repo": ("schema", True),
                "repo_id": ("schema", True),
                "schema": ("schema", True),
                "sequence": ("signature", False),
            },
        }
        for implementation_name, implementation in (
            ("Python", self.python_result),
            ("C++", self.cpp_result),
        ):
            cases = implementation["cases"]
            with self.subTest(implementation=implementation_name):
                self.assertNotIn("wrong_domain_S", cases)
                for name in domain_cases:
                    self.assertIn(name, cases)
                    self.assertTrue(cases[name]["rejected"], name)
                    self.assertFalse(cases[name]["signature_valid"], name)
                    self.assertFalse(cases[name]["parse_reached"], name)
                    self.assertEqual(cases[name]["stage"], "signature", name)
                self.assertFalse(cases["old_v1_domain_for_M"]["signature_valid"])
                self.assertFalse(cases["old_v1_domain_for_M"]["parse_reached"])
                self.assertEqual(cases["old_v1_domain_for_M"]["stage"], "signature")
                self.assertFalse(cases["unsigned_one_byte_flip_M"]["signature_valid"])
                self.assertFalse(cases["unsigned_one_byte_flip_M"]["parse_reached"])

                expected_total = 0
                expected_signed = 0
                expected_unsigned = 0
                for kind, fields in field_stages.items():
                    self.assertEqual(cases[f"field_flip_cases_{kind}"], len(fields))
                    expected_total += len(fields)
                    expected_signed += sum(1 for _, signed in fields.values() if signed)
                    expected_unsigned += sum(1 for _, signed in fields.values() if not signed)
                    for field, (stage, signed) in fields.items():
                        name = f"field_flip_{kind}_{field}"
                        with self.subTest(implementation=implementation_name, field=name):
                            self.assertIn(name, cases)
                            self.assertTrue(cases[name]["rejected"], name)
                            self.assertEqual(cases[name]["signature_valid"], signed, name)
                            self.assertEqual(cases[name]["parse_reached"], signed, name)
                            self.assertEqual(cases[name]["stage"], stage, name)
                self.assertEqual(cases["field_flip_cases"], expected_total)
                self.assertEqual(cases["field_flip_cases_signed"], expected_signed)
                self.assertEqual(cases["field_flip_cases_unsigned"], expected_unsigned)
                self.assertEqual((expected_total, expected_signed, expected_unsigned), (39, 25, 14))

    def test_epoch_recovery_preserves_allocator_high_water(self):
        self.assertEqual(
            self.python_result["state"],
            {
                "client_accepts_epoch1_build101_from_epoch0_max": True,
                "global_h_before_reservation": "100",
                "global_h_after_statement": "100",
                "global_h_after_real_reservation": "101",
                "legitimate_h_max_permanently_stops": True,
                "checkpoint_chain_valid": True,
            },
        )

    def test_test_keys_do_not_escape_fixture_directory_or_enter_build_paths(self):
        vectors = json.loads((FIXTURE_DIR / "rfc8032-vectors.json").read_text())
        public_keys = [bytes.fromhex(row["public_key"]) for row in vectors.values()]
        for path in REPO_ROOT.rglob("*"):
            if not path.is_file() or ".git" in path.parts or FIXTURE_DIR in path.parents:
                continue
            contents = path.read_bytes()
            for public_key in public_keys:
                self.assertNotIn(public_key.hex().encode(), contents, str(path))
            relative = path.relative_to(REPO_ROOT).as_posix()
            if path.name == "CMakeLists.txt" or path.suffix == ".cmake" or relative.startswith("Telegram/SourceFiles/"):
                self.assertNotIn(b"epoch-fixtures", contents, str(path))


if __name__ == "__main__":
    unittest.main()
