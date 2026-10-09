import ast
import hashlib
import os
import pathlib
import re
import subprocess
import tempfile
import unittest


PREPARE = pathlib.Path(__file__).with_name("prepare.py")


def download_script():
    tree = ast.parse(PREPARE.read_text(encoding="utf-8"))
    call = next(
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "stage"
        and node.args
        and isinstance(node.args[0], ast.Constant)
        and node.args[0].value == "libiconv"
    )
    commands = ast.literal_eval(call.args[1])
    lines = commands.splitlines()
    start = next(
        i for i, line in enumerate(lines)
        if line.strip() == "VERSION=1.18"
    )
    end = next(
        i for i, line in enumerate(lines)
        if line.strip() == "rm -rf libiconv-$VERSION"
    )
    script = "\n".join(
        line[4:] if line.startswith("    ") else line
        for line in lines[start:end]
    )
    pinned = re.search(r"(?m)^SHA256=([a-f0-9]+)$", script)
    if not pinned:
        raise AssertionError("libiconv stage has no pinned SHA256")
    return script, pinned.group(1)


class LibiconvDownloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script, cls.original_hash = download_script()

    def run_download(self, mode, preserve_partial=False):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            calls_file = root / "calls"
            tar_marker = root / "tar-input"
            archive_bytes = b"verified archive"
            expected_hash = hashlib.sha256(archive_bytes).hexdigest()
            script = self.script.replace(self.original_hash, expected_hash)
            script = "set -e\n" + script + "\ntar -xvzf libiconv.tar.gz\n"

            wget = bin_dir / "wget"
            wget.write_text(
                "#!/bin/sh\n"
                "output=\n"
                "while [ \"$#\" -gt 0 ]; do\n"
                "  if [ \"$1\" = \"-O\" ]; then\n"
                "    output=$2\n"
                "    shift 2\n"
                "  else\n"
                "    shift\n"
                "  fi\n"
                "done\n"
                "count=0\n"
                "if [ -f \"$CALLS_FILE\" ]; then\n"
                "  count=$(cat \"$CALLS_FILE\")\n"
                "fi\n"
                "count=$((count + 1))\n"
                "printf '%s' \"$count\" > \"$CALLS_FILE\"\n"
                "if [ \"$FAKE_WGET_MODE\" = partial-failure ]; then\n"
                "  printf 'partial archive' > \"$output\"\n"
                "  exit 1\n"
                "fi\n"
                "if [ \"$count\" -eq 1 ]; then\n"
                "  printf 'corrupt archive' > \"$output\"\n"
                "elif [ \"$count\" -eq 2 ]; then\n"
                "  printf 'verified archive' > \"$output\"\n"
                "else\n"
                "  printf 'partial archive' > \"$output\"\n"
                "  exit 1\n"
                "fi\n"
                "exit 0\n",
                encoding="utf-8",
            )
            wget.chmod(0o755)
            tar = bin_dir / "tar"
            tar.write_text(
                "#!/bin/sh\ncat libiconv.tar.gz > \"$TAR_MARKER\"\n",
                encoding="utf-8",
            )
            tar.chmod(0o755)
            rm = bin_dir / "rm"
            rm.write_text(
                "#!/bin/sh\n"
                "if [ \"$PRESERVE_PARTIAL\" = 1 ] "
                "&& [ \"$1\" = -f ] "
                "&& [ \"$2\" = libiconv.tar.gz ]; then\n"
                "  exit 0\n"
                "fi\n"
                "exec /bin/rm \"$@\"\n",
                encoding="utf-8",
            )
            rm.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env["PATH"]
            env["CALLS_FILE"] = str(calls_file)
            env["TAR_MARKER"] = str(tar_marker)
            env["FAKE_WGET_MODE"] = mode
            env["PRESERVE_PARTIAL"] = "1" if preserve_partial else "0"
            result = subprocess.run(
                ["bash", "-c", script],
                cwd=root,
                env=env,
                capture_output=True,
                text=True,
            )
            calls = calls_file.read_text(encoding="utf-8") if calls_file.exists() else ""
            tar_input = tar_marker.read_bytes() if tar_marker.exists() else None
            archive_exists = (root / "libiconv.tar.gz").exists()
            return result, calls, tar_input, archive_exists

    def test_rejects_partial_download_after_all_mirrors_fail(self):
        result, calls, tar_input, archive_exists = self.run_download(
            "partial-failure"
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, "4")
        self.assertIsNone(tar_input)
        self.assertFalse(archive_exists)

    def test_rechecks_checksum_after_last_mirror_failure(self):
        result, calls, tar_input, archive_exists = self.run_download(
            "partial-failure",
            preserve_partial=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, "4")
        self.assertIsNone(tar_input)
        self.assertTrue(archive_exists)

    def test_uses_later_mirror_with_the_pinned_checksum(self):
        result, calls, tar_input, _ = self.run_download("fallback")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, "2")
        self.assertEqual(tar_input, b"verified archive")


if __name__ == "__main__":
    unittest.main()
