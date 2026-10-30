"""Run a small, CPU-only BasicVSR++ evaluation on a local compressed video.

This is a research validation utility. It deliberately does not touch the
RifeLove core, model cache, packaging, or user media. Frames are decoded from
the source video through an existing local FFmpeg binary and kept in memory;
the optional PNG outputs are written only to the requested evaluation folder.

The BasicVSR++ network is loaded from the isolated CPU reference experiment
under ``work/basicvsrpp-eval``. Its torchvision deformable-convolution
substitute is not a proof of MMCV numerical equivalence or product readiness.
The default input video has no aligned clean reference, so the script reports
no-reference and input-preservation metrics. An optional aligned reference can
be supplied for a controlled local transcode experiment; those numbers remain
proxy metrics rather than a product-quality claim.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image


ROOT = Path(__file__).resolve().parents[2]
REFERENCE_DIR = ROOT / "work" / "basicvsrpp-eval"
DEFAULT_VIDEO = (
    ROOT
    / "work"
    / "temporal-restoration"
    / "realbasicvsr"
    / "data"
    / "demo_001.mp4"
)
DEFAULT_FFMPEG = ROOT / "work" / "codec-ffmpeg-build" / "ffmpeg-install" / "bin" / "ffmpeg.exe"
DEFAULT_FFPROBE = DEFAULT_FFMPEG.with_name("ffprobe.exe")


def _load_reference_module():
    if str(REFERENCE_DIR) not in sys.path:
        sys.path.insert(0, str(REFERENCE_DIR))
    import run_cpu_reference as reference  # type: ignore

    return reference


def decode_rgb24(ffmpeg: Path, video: Path, frames: int) -> torch.Tensor:
    """Decode ``frames`` RGB frames into [1,T,3,H,W] without writing media."""
    probe = subprocess.run(
        [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(video),
         "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        check=True,
        capture_output=True,
    )
    first = np.frombuffer(probe.stdout, dtype=np.uint8)
    if first.size == 0 or first.size % 3:
        raise RuntimeError("FFmpeg did not return a decodable RGB frame")

    # Decode one PNG frame solely to obtain dimensions without depending on
    # ffprobe being installed beside the selected FFmpeg binary.
    png_probe = subprocess.run(
        [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(video),
         "-frames:v", "1", "-f", "image2pipe", "-vcodec", "png", "-"],
        check=True,
        capture_output=True,
    )
    with Image.open(io.BytesIO(png_probe.stdout)) as image:
        width, height = image.size
    expected = width * height * 3
    if first.size != expected:
        raise RuntimeError(f"Unexpected RGB frame size {first.size}; expected {expected}")

    raw = subprocess.run(
        [str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(video),
         "-frames:v", str(frames), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        check=True,
        capture_output=True,
    ).stdout
    array = np.frombuffer(raw, dtype=np.uint8)
    expected_total = frames * expected
    if array.size != expected_total:
        raise RuntimeError(
            f"FFmpeg returned {array.size} bytes, expected {expected_total} "
            f"for {frames} frames"
        )
    array = array.reshape(frames, height, width, 3)
    return torch.from_numpy(array.copy()).permute(0, 3, 1, 2).float().div_(255.0).unsqueeze(0)


def probe_media(ffprobe: Path | None, video: Path) -> dict:
    if ffprobe is None or not ffprobe.is_file():
        return {"available": False}
    completed = subprocess.run(
        [str(ffprobe), "-v", "error", "-show_streams", "-show_format", "-of", "json", str(video)],
        check=True,
        capture_output=True,
        text=True,
        errors="replace",
    )
    raw = json.loads(completed.stdout)
    streams = []
    for stream in raw.get("streams", []):
        if stream.get("codec_type") == "video":
            streams.append({
                key: stream.get(key)
                for key in (
                    "codec_name", "profile", "width", "height", "pix_fmt",
                    "r_frame_rate", "avg_frame_rate", "duration", "nb_frames",
                    "bit_rate", "color_range", "color_space", "color_primaries",
                    "color_transfer",
                )
                if key in stream
            })
    return {
        "available": True,
        "streams": streams,
        "format": {
            key: raw.get("format", {}).get(key)
            for key in ("format_name", "duration", "size", "bit_rate", "tags")
            if key in raw.get("format", {})
        },
    }


def _laplacian_energy(sequence: torch.Tensor) -> float:
    gray = (
        sequence[:, :, 0:1] * 0.299
        + sequence[:, :, 1:2] * 0.587
        + sequence[:, :, 2:3] * 0.114
    )
    # [1,T,1,H,W] -> [T,1,H,W]
    gray = gray.reshape(-1, 1, gray.shape[-2], gray.shape[-1])
    kernel = torch.tensor(
        [[0.0, 1.0, 0.0], [1.0, -4.0, 1.0], [0.0, 1.0, 0.0]],
        dtype=gray.dtype,
    ).reshape(1, 1, 3, 3)
    response = F.conv2d(gray, kernel, padding=1)
    return float(response.square().mean().item())


def _temporal_difference(sequence: torch.Tensor) -> float:
    if sequence.shape[1] < 2:
        return 0.0
    return float((sequence[:, 1:] - sequence[:, :-1]).abs().mean().item())


def _pairwise_temporal_change(input_seq: torch.Tensor, output_seq: torch.Tensor) -> float:
    if input_seq.shape[1] < 2:
        return 0.0
    input_delta = (input_seq[:, 1:] - input_seq[:, :-1]).abs()
    output_delta = (output_seq[:, 1:] - output_seq[:, :-1]).abs()
    return float((output_delta - input_delta).abs().mean().item())


def _psnr(candidate: torch.Tensor, reference: torch.Tensor) -> float:
    mse = float((candidate - reference).square().mean().item())
    return float(-10.0 * torch.log10(torch.tensor(max(mse, 1e-12))).item())


def _global_ssim(candidate: torch.Tensor, reference: torch.Tensor) -> float:
    """A global SSIM proxy, useful here without adding a metric dependency."""
    x = candidate.reshape(-1).double()
    y = reference.reshape(-1).double()
    c1 = 0.01**2
    c2 = 0.03**2
    mu_x = x.mean()
    mu_y = y.mean()
    var_x = x.var(unbiased=False)
    var_y = y.var(unbiased=False)
    cov_xy = ((x - mu_x) * (y - mu_y)).mean()
    value = ((2 * mu_x * mu_y + c1) * (2 * cov_xy + c2)) / (
        (mu_x.square() + mu_y.square() + c1) * (var_x + var_y + c2)
    )
    return float(value.item())


def _write_preview(sequence: torch.Tensor, folder: Path, prefix: str) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    for index, frame in enumerate(sequence[0]):
        pixels = (frame.clamp(0, 1).permute(1, 2, 0).mul(255).round().byte().numpy())
        Image.fromarray(pixels).save(folder / f"{prefix}_{index:02d}.png")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--video", type=Path, default=DEFAULT_VIDEO)
    parser.add_argument(
        "--reference-video",
        type=Path,
        default=None,
        help="Optional aligned high-quality reference for PSNR/SSIM comparison.",
    )
    parser.add_argument("--ffmpeg", type=Path, default=DEFAULT_FFMPEG)
    parser.add_argument("--ffprobe", type=Path, default=DEFAULT_FFPROBE)
    parser.add_argument("--frames", type=int, default=5)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT / "work" / "basicvsrpp-eval" / "real-media-eval",
    )
    parser.add_argument("--write-preview", action="store_true")
    args = parser.parse_args()
    if args.frames < 2:
        raise ValueError("--frames must be at least 2 for temporal metrics")
    if not args.video.is_file():
        raise FileNotFoundError(args.video)
    if not args.ffmpeg.is_file():
        raise FileNotFoundError(args.ffmpeg)

    reference = _load_reference_module()
    torch.set_num_threads(4)
    print(f"decode: {args.video}", flush=True)
    source = decode_rgb24(args.ffmpeg, args.video, args.frames)
    reference_video = None
    if args.reference_video is not None:
        if not args.reference_video.is_file():
            raise FileNotFoundError(args.reference_video)
        reference_video = decode_rgb24(args.ffmpeg, args.reference_video, args.frames)
        if reference_video.shape != source.shape:
            raise RuntimeError(
                f"reference shape {list(reference_video.shape)} does not match "
                f"input shape {list(source.shape)}"
            )

    model_cls = reference.load_official_network()
    model = model_cls(
        mid_channels=128,
        num_blocks=25,
        is_low_res_input=False,
        spynet_pretrained=str(reference.SPYNET),
        cpu_cache_length=100,
    )
    checkpoint = torch.load(reference.CHECKPOINT, map_location="cpu", weights_only=True)
    checkpoint_state = checkpoint["state_dict"] if "state_dict" in checkpoint else checkpoint
    state = {
        key.removeprefix("generator."): value
        for key, value in checkpoint_state.items()
        if key.startswith("generator.")
    }
    missing, unexpected = model.load_state_dict(state, strict=False)
    if missing or unexpected:
        raise RuntimeError(f"checkpoint mismatch: missing={missing}, unexpected={unexpected}")

    model.eval()
    started = time.perf_counter()
    with torch.inference_mode():
        restored = model(source).clamp(0, 1)
    elapsed = time.perf_counter() - started

    input_l1_output = float((restored - source).abs().mean().item())
    source_metrics = {
        "temporal_abs_delta": _temporal_difference(source),
        "laplacian_energy": _laplacian_energy(source),
    }
    restored_metrics = {
        "temporal_abs_delta": _temporal_difference(restored),
        "laplacian_energy": _laplacian_energy(restored),
    }
    limitations = [
        "the reference is a local high-quality proxy, not an original camera/master source",
        "CPU torchvision deformable convolution is a research substitute for MMCV and is not numerical-equivalence proof",
        "metrics are preliminary and cannot establish perceptual quality or Topaz-level behavior",
        "this result does not qualify the model for RifeLove packaging or ONNX/TensorRT deployment",
    ]
    if reference_video is None:
        limitations[0] = "demo_001.mp4 has no aligned clean reference; PSNR/SSIM improvement is not measured"
    report = {
        "status": "passed_cpu_real_media_reference",
        "video": str(args.video),
        "video_sha256": hashlib.sha256(args.video.read_bytes()).hexdigest(),
        "ffmpeg": str(args.ffmpeg),
        "source_probe": probe_media(args.ffprobe, args.video),
        "frames": args.frames,
        "input_shape": list(source.shape),
        "output_shape": list(restored.shape),
        "device": "cpu",
        "elapsed_seconds": elapsed,
        "seconds_per_frame": elapsed / args.frames,
        "input_vs_output_mean_abs": input_l1_output,
        "input_metrics": source_metrics,
        "output_metrics": restored_metrics,
        "temporal_delta_change_abs": _pairwise_temporal_change(source, restored),
        "laplacian_energy_ratio": restored_metrics["laplacian_energy"]
        / max(source_metrics["laplacian_energy"], 1e-12),
        "model": "BasicVSR++ NTIRE decompression track 1",
        "operator": "torchvision.ops.deform_conv2d CPU substitute",
        "limitations": limitations,
    }
    if reference_video is not None:
        report["reference_video"] = str(args.reference_video)
        report["reference_video_sha256"] = hashlib.sha256(
            args.reference_video.read_bytes()
        ).hexdigest()
        report["reference_probe"] = probe_media(args.ffprobe, args.reference_video)
        report["reference_metrics"] = {
            "input_psnr_db": _psnr(source, reference_video),
            "output_psnr_db": _psnr(restored, reference_video),
            "input_global_ssim": _global_ssim(source, reference_video),
            "output_global_ssim": _global_ssim(restored, reference_video),
        }
        report["per_frame_reference_metrics"] = [
            {
                "frame": index,
                "input_psnr_db": _psnr(source[:, index:index + 1], reference_video[:, index:index + 1]),
                "output_psnr_db": _psnr(restored[:, index:index + 1], reference_video[:, index:index + 1]),
                "input_global_ssim": _global_ssim(source[:, index:index + 1], reference_video[:, index:index + 1]),
                "output_global_ssim": _global_ssim(restored[:, index:index + 1], reference_video[:, index:index + 1]),
            }
            for index in range(args.frames)
        ]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    result_path = args.output_dir / "basicvsrpp-real-media-result.json"
    result_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.write_preview:
        _write_preview(source, args.output_dir / "input", "input")
        _write_preview(restored, args.output_dir / "output", "output")
    print(json.dumps(report, indent=2))
    print(f"result: {result_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
