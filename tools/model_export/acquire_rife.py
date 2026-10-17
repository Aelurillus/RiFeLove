"""Acquire only pinned, allow-listed RIFE model files; no third-party code is executed."""

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import urllib.request
import uuid
import zipfile


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "models/manifests/rife-4.25.source.json"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked_bytes(path, expected):
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Invalid SHA-256 value in model manifest")
    if path.stat().st_size > 128 * 1024 * 1024:
        raise ValueError("Pinned model source file exceeds its size limit")
    data = path.read_bytes()
    if digest(data) != expected:
        raise ValueError(f"SHA-256 mismatch: {path}")
    return data


def cached_file(cache, item, allow_download):
    path = cache / item["cacheName"]
    if not path.is_file():
        if not allow_download:
            raise FileNotFoundError(f"Dependency is not cached: {path}; supply it locally or explicitly enable --allow-download")
        cache.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".partial")
        try:
            with urllib.request.urlopen(item["url"], timeout=60) as response, temporary.open("xb") as output:
                size = 0
                while chunk := response.read(1024 * 1024):
                    size += len(chunk)
                    if size > 128 * 1024 * 1024:
                        raise ValueError("Model source download exceeded its size limit")
                    output.write(chunk)
            checked_bytes(temporary, item["sha256"])
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
    checked_bytes(path, item["sha256"])
    return path


def acquire(destination, cache, manifest, allow_download=False):
    archive = cached_file(cache, manifest["archive"], allow_download)
    approved = {}
    with zipfile.ZipFile(archive) as package:
        for item in manifest["archiveFiles"]:
            entry = package.getinfo(item["entry"])
            if entry.file_size != item["bytes"]:
                raise ValueError(f"Unexpected model file size: {item['entry']}")
            data = package.read(entry)
            if digest(data) != item["sha256"]:
                raise ValueError(f"Model archive entry checksum mismatch: {item['entry']}")
            approved[item["destination"]] = data
    for item in manifest["additionalFiles"]:
        path = cached_file(cache, item, allow_download)
        approved[item["destination"]] = checked_bytes(path, item["sha256"])
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in approved.items():
        if Path(name).name != name:
            raise ValueError("Model manifest destination must be a flat allow-listed filename")
        path = destination / name
        if path.exists():
            if digest(path.read_bytes()) != digest(data):
                raise ValueError(f"Existing model file was modified; refusing to replace: {path}")
        else:
            with path.open("xb") as output:
                output.write(data)
    (destination / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=ROOT / "models/weights/rife4.25-reviewed")
    parser.add_argument("--cache", type=Path, default=ROOT / "work/downloads")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--allow-download", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    destination = acquire(args.destination, args.cache, manifest, args.allow_download)
    print(json.dumps({"event": "model_sources_verified", "model_id": manifest["modelId"], "path": str(destination), "license": manifest["license"]}))


if __name__ == "__main__":
    main()
