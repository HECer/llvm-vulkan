#!/usr/bin/env python3
"""Download a checksum-pinned LLVM source snapshot and extract it safely.

This fetches *source*, not a binary LLVM installation. It does not update the
lock, follow moving branches, execute upstream code, or overwrite a destination.
Python with tarfile's data extraction filter is required (3.12+, or a patched
3.11 release). Network, SHA mismatch, and unsafe archives are hard errors.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
import tarfile
import tempfile
import urllib.request


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, destination: Path, expected: str) -> None:
    if not url.startswith("https://"):
        raise ValueError("source URL must use HTTPS")
    if destination.exists():
        if sha256(destination) != expected:
            raise ValueError(f"existing archive checksum mismatch: {destination}")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".llvm-download-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            request = urllib.request.Request(url, headers={"User-Agent": "llvm-vulkan-source/0.1"})
            with urllib.request.urlopen(request, timeout=60) as response:
                shutil.copyfileobj(response, output, length=1024 * 1024)
        if sha256(Path(temporary)) != expected:
            raise ValueError("downloaded archive checksum mismatch; destination unchanged")
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)


def extract(archive: Path, destination: Path, pin: dict) -> dict:
    if destination.exists():
        raise FileExistsError(f"destination already exists; refusing overwrite: {destination}")
    if not hasattr(tarfile, "data_filter"):
        raise RuntimeError("safe extraction requires Python tarfile.data_filter (Python 3.12+)")
    digest = sha256(archive)
    if digest != pin["archive_sha256"]:
        raise ValueError(f"archive SHA256 mismatch: expected {pin['archive_sha256']}, got {digest}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".llvm-extract-", dir=destination.parent) as temporary:
        staging = Path(temporary)
        with tarfile.open(archive, "r:*") as source:
            members = source.getmembers()
            total_bytes = 0
            roots = set()
            for member in members:
                parts = PurePosixPath(member.name).parts
                if not parts or PurePosixPath(member.name).is_absolute() or ".." in parts:
                    raise ValueError(f"unsafe archive entry: {member.name!r}")
                roots.add(parts[0])
                total_bytes += member.size
            if len(roots) != 1 or len(members) > 500_000 or total_bytes > 12 * 1024**3:
                raise ValueError("unexpected archive layout or size")
            # The data filter rejects special files and links outside staging,
            # strips unsafe permissions, and checks the resolved path at write time.
            source.extractall(staging, members=members, filter="data")
        extracted = staging / next(iter(roots))
        version_file = extracted / "cmake/Modules/LLVMVersion.cmake"
        if not version_file.is_file() or sha256(version_file) != pin["version_file_sha256"]:
            raise ValueError("LLVM source version file does not match the pinned snapshot")
        if not (extracted / "llvm/CMakeLists.txt").is_file():
            raise ValueError("archive is not an LLVM monorepo source tree")
        result = {
            "commit": pin["commit"],
            "version": pin["version"],
            "archive_sha256": digest,
            "archive": str(archive.resolve()),
            "source": str(destination.resolve()),
            "files_in_archive": len(members),
            "uncompressed_bytes": total_bytes,
        }
        (extracted / ".llvm-vulkan-source.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        os.replace(extracted, destination)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=["main", "release"], default="main")
    parser.add_argument("--lock", type=Path, default=Path(__file__).resolve().parents[1] / "upstream-lock.json")
    parser.add_argument("--destination", type=Path, default=Path("third_party/llvm-project"))
    parser.add_argument("--archive", type=Path, help="use an existing archive instead of downloading")
    parser.add_argument("--cache", type=Path, default=Path(".cache/llvm-sources"))
    parser.add_argument("--print", action="store_true", dest="print_only", help="print the exact source pin without downloading")
    args = parser.parse_args()
    pin = json.loads(args.lock.read_text(encoding="utf-8"))[args.channel]
    if args.print_only:
        print(json.dumps(pin, indent=2))
        return 0
    archive = args.archive or args.cache / pin["archive_filename"]
    if args.archive is None:
        download(pin["archive_url"], archive, pin["archive_sha256"])
    print(json.dumps(extract(archive, args.destination, pin), indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, RuntimeError, tarfile.TarError) as error:
        print(f"fetch_llvm: {error}", file=sys.stderr)
        raise SystemExit(1)
