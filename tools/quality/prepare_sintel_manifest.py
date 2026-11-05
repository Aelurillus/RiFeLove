#!/usr/bin/env python3
"""Create a provenance and frame-integrity manifest for the official 720p Sintel trailer."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any


ARCHIVE_NAME = "sintel_trailer-720-png.tar.gz"
ARCHIVE_SHA256 = "6324f7ea1a054d2e0b477c070c8f291b7a796926ba7199af51b1701d80c5911b"
ARCHIVE_BYTES = 482_665_536
# Xiph lists the archive but does not publish this digest; this repository pins the bytes it obtained.
FRAME_PREFIX = "sintel_trailer_2k_"
FRAME_COUNT = 1_253
WIDTH = 1_280
HEIGHT = 720
FRAME_RATE = "24/1"
FRAME_RATE_EVIDENCE = (
    "Official Xiph lossless Y4M counterpart header: "
    "YUV4MPEG2 C420jpeg W1280 H720 F24:1 Ip A1:1"
)
FRAME_RATE_SOURCE = "https://media.xiph.org/video/derf/y4m/sintel_trailer_2k_720p24.y4m"
SOURCE_URL = "https://media.xiph.org/sintel/sintel_trailer-720-png.tar.gz"
LICENSE_URL = "https://durian.blender.org/sharing/"
ATTRIBUTION = "© copyright Blender Foundation | durian.blender.org"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
COLOR_METADATA_CHUNKS = {b"sRGB", b"gAMA", b"cHRM", b"iCCP", b"cICP"}
FRAME_NAME = re.compile(r"^sintel_trailer_2k_(\d{4})\.png$")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def inspect_png(path: Path) -> dict[str, Any]:
    """Read the PNG header and color-signaling chunks without decoding pixel data."""
    file_size = path.stat().st_size
    with path.open("rb") as stream:
        if stream.read(8) != PNG_SIGNATURE:
            raise ValueError(f"{path.name}: invalid PNG signature")

        first_length = stream.read(4)
        first_type = stream.read(4)
        if first_length != b"\x00\x00\x00\x0d" or first_type != b"IHDR":
            raise ValueError(f"{path.name}: first chunk is not a 13-byte IHDR")
        ihdr = stream.read(13)
        stream.read(4)  # IHDR CRC is checked by the subsequent full decode pass.
        width = int.from_bytes(ihdr[0:4], "big")
        height = int.from_bytes(ihdr[4:8], "big")
        bit_depth = ihdr[8]
        color_type = ihdr[9]

        color_chunks: list[str] = []
        while True:
            chunk_length_bytes = stream.read(4)
            if len(chunk_length_bytes) != 4:
                raise ValueError(f"{path.name}: missing PNG IEND chunk")
            chunk_length = int.from_bytes(chunk_length_bytes, "big")
            chunk_type = stream.read(4)
            if len(chunk_type) != 4:
                raise ValueError(f"{path.name}: truncated PNG chunk header")
            if stream.tell() + chunk_length + 4 > file_size:
                raise ValueError(f"{path.name}: PNG chunk exceeds file size")
            if chunk_type in COLOR_METADATA_CHUNKS:
                color_chunks.append(chunk_type.decode("ascii"))
            stream.seek(chunk_length + 4, os.SEEK_CUR)  # Skip payload and CRC.
            if chunk_type == b"IEND":
                break

    return {
        "width": width,
        "height": height,
        "bitDepth": bit_depth,
        "colorType": color_type,
        "colorMetadataChunks": color_chunks,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True, help="official Xiph trailer PNG archive")
    parser.add_argument("--frames", type=Path, required=True, help="extracted directory containing the 1,253 PNG frames")
    parser.add_argument("--output", type=Path, required=True, help="new JSON manifest path")
    parser.add_argument("--force", action="store_true", help="replace an existing manifest")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    archive = args.archive.resolve(strict=True)
    frames_directory = args.frames.resolve(strict=True)
    output = args.output.resolve()

    if not archive.is_file() or archive.name != ARCHIVE_NAME:
        raise ValueError(f"archive must be the official {ARCHIVE_NAME} file")
    if archive.stat().st_size != ARCHIVE_BYTES:
        raise ValueError(f"archive size differs from the pinned Xiph asset ({ARCHIVE_BYTES} bytes)")
    archive_sha256 = sha256_file(archive)
    if archive_sha256 != ARCHIVE_SHA256:
        raise ValueError("archive SHA-256 does not match this repository's pinned Xiph archive digest")
    if not frames_directory.is_dir():
        raise ValueError("frames path must be a directory")

    paths = list(frames_directory.glob("*.png"))
    if len(paths) != FRAME_COUNT:
        raise ValueError(f"expected {FRAME_COUNT} PNG files, found {len(paths)}")

    by_number: dict[int, Path] = {}
    for path in paths:
        match = FRAME_NAME.fullmatch(path.name)
        if not match:
            raise ValueError(f"unexpected PNG filename: {path.name}")
        if path.is_symlink():
            raise ValueError(f"symbolic-link frame is not accepted: {path.name}")
        number = int(match.group(1))
        if number in by_number:
            raise ValueError(f"duplicate frame number: {number}")
        by_number[number] = path

    expected_numbers = set(range(1, FRAME_COUNT + 1))
    if set(by_number) != expected_numbers:
        missing = sorted(expected_numbers - set(by_number))
        raise ValueError(f"frame sequence is incomplete; first missing numbers: {missing[:8]}")

    frames: list[dict[str, Any]] = []
    common_format: dict[str, Any] | None = None
    all_color_chunks: set[str] = set()
    for number in range(1, FRAME_COUNT + 1):
        path = by_number[number]
        image_format = inspect_png(path)
        if image_format["width"] != WIDTH or image_format["height"] != HEIGHT:
            raise ValueError(f"{path.name}: expected {WIDTH}x{HEIGHT}, got {image_format['width']}x{image_format['height']}")
        if image_format["bitDepth"] != 8 or image_format["colorType"] != 2:
            raise ValueError(f"{path.name}: expected 8-bit RGB PNG, got bit depth {image_format['bitDepth']} and color type {image_format['colorType']}")
        if common_format is None:
            common_format = image_format
        elif image_format != common_format:
            raise ValueError(f"{path.name}: PNG format or color metadata differs from the first frame")
        all_color_chunks.update(image_format["colorMetadataChunks"])
        frames.append({
            "index": number,
            "file": path.name,
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        })

    if output.exists() and not args.force:
        raise FileExistsError(f"manifest already exists: {output}; pass --force to replace it")
    output.parent.mkdir(parents=True, exist_ok=True)

    manifest = {
        "schemaVersion": 1,
        "datasetId": "sintel-trailer-720p-xiph-png",
        "datasetKind": "synthetic-animation-regression",
        "source": {
            "title": "Sintel Trailer",
            "organization": "Blender Foundation / Durian Open Movie Project",
            "archiveUrl": SOURCE_URL,
            "archiveFile": ARCHIVE_NAME,
            "archiveBytes": ARCHIVE_BYTES,
            "archiveSha256": archive_sha256,
        },
        "license": {
            "id": "CC-BY-3.0",
            "url": LICENSE_URL,
            "attribution": ATTRIBUTION,
            "limitations": "Blender/Sintel/Durian logos, trademarks, and clearly non-Durian third-party material are excluded.",
        },
        "sequence": {
            "frameCount": FRAME_COUNT,
            "frameRate": FRAME_RATE,
            "frameRateSourceUrl": FRAME_RATE_SOURCE,
            "frameRateEvidence": FRAME_RATE_EVIDENCE,
            "width": WIDTH,
            "height": HEIGHT,
            "bitDepth": 8,
            "pixelFormat": "rgb24",
            "colorMetadataChunks": sorted(all_color_chunks),
            "colorInterpretation": "unspecified-in-source-pngs; do not infer primaries or transfer characteristics",
            "letterboxed": True,
        },
        "frames": frames,
        "limitations": [
            "Rendered animation, not camera-captured footage or real-video ground truth.",
            "Candidate training-set overlap is unknown.",
            "Scene-cut and active-picture crop boundaries require a separate reviewed split manifest.",
        ],
    }

    encoded = (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    with tempfile.NamedTemporaryFile("wb", dir=output.parent, prefix=f".{output.name}.", suffix=".tmp", delete=False) as temporary:
        temporary.write(encoded)
        temporary.flush()
        os.fsync(temporary.fileno())
        temporary_path = Path(temporary.name)
    try:
        os.replace(temporary_path, output)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()

    print(f"manifest={output}")
    print(f"frame_count={FRAME_COUNT}")
    print(f"archive_sha256={archive_sha256}")
    print(f"png_color_metadata_chunks={','.join(sorted(all_color_chunks)) or '(none)'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, FileExistsError, ValueError, OSError) as error:
        raise SystemExit(f"error: {error}")
