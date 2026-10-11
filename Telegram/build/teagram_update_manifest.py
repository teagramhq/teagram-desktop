#!/usr/bin/env python3

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


MAX_UINT64 = (1 << 64) - 1
MAX_MANIFEST_SIZE = 16 * 1024


def canonical_build(value):
    if not re.fullmatch(r"0|[1-9][0-9]*", value):
        raise ValueError("build must be a canonical unsigned decimal value")
    if int(value) > MAX_UINT64:
        raise ValueError("build exceeds uint64")
    return value


def archive_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as archive:
        for chunk in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def create_package(args):
    build = canonical_build(args.build)
    if build == "0":
        raise ValueError("build must be greater than zero")
    if args.channel not in ("dev", "main"):
        raise ValueError("channel must be dev or main")
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        raise ValueError("commit must be a 40-character lowercase SHA")
    if not re.fullmatch(r"[0-9]+\.[0-9]+(?:\.[0-9]+)?", args.min_os):
        raise ValueError("min-os must be a numeric macOS version")

    archive = Path(args.archive)
    if not archive.is_file() or archive.is_symlink():
        raise ValueError("archive must be a regular file")
    asset_name = f"Teagram-macOS-arm64-{build}.zip"
    if archive.name != asset_name:
        raise ValueError("archive name does not match the build identity")
    asset_size = archive.stat().st_size
    if asset_size == 0:
        raise ValueError("archive is empty")

    manifest = {
        "format": 1,
        "product": "io.teagram.desktop",
        "repo": "teagramhq/teagram-desktop",
        "arch": "arm64",
        "channel": args.channel,
        "build": build,
        "commit": args.commit,
        "asset_name": asset_name,
        "asset_size": str(asset_size),
        "asset_sha256": archive_digest(archive),
        "min_os": args.min_os,
    }
    manifest_bytes = json.dumps(
        manifest,
        ensure_ascii=True,
        separators=(",", ":"),
    ).encode("ascii")
    if len(manifest_bytes) > MAX_MANIFEST_SIZE:
        raise ValueError("manifest exceeds the verifier size limit")

    with tempfile.TemporaryDirectory(prefix="teagram-update-signature-") as temporary:
        message = Path(temporary) / "message.bin"
        message.write_bytes(b"teagram-update-v1\0" + manifest_bytes)
        result = subprocess.run(
            [
                "openssl",
                "pkeyutl",
                "-sign",
                "-rawin",
                "-inkey",
                args.signing_key,
                "-in",
                str(message),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    if result.returncode != 0:
        raise ValueError("OpenSSL could not sign the update manifest")
    if len(result.stdout) != 64:
        raise ValueError("OpenSSL produced an invalid Ed25519 signature")

    output = Path(args.output_dir)
    output.mkdir(parents=True, exist_ok=True)
    (output / "teagram-update.json").write_bytes(manifest_bytes)
    (output / "teagram-update.sig").write_bytes(result.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True)
    parser.add_argument("--build", required=True)
    parser.add_argument("--channel", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--min-os", required=True)
    parser.add_argument("--signing-key", required=True)
    parser.add_argument("--output-dir", required=True)
    try:
        create_package(parser.parse_args())
    except (OSError, ValueError) as error:
        print(f"teagram update manifest: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
