# Native RIFE video pipeline

Implemented and tested locally on Windows 10 / GTX 1660 SUPER. This is a working increment toward the complete Master Plan, with deliberately narrow current color/model qualification.

## Run locally

Enable `RIFELOVE_BUILD_MEDIA` and `RIFELOVE_BUILD_ONNX_CUDA` with the pinned local SDKs described in [FFmpeg](FFMPEG.md), [GPU](GPU.md) and [RIFE models](RIFE_MODELS.md). Supply an exported, checksum-qualified model pack and native runtime DLL directory:

```powershell
.\build\windows-release\rifelove-cli.exe interpolate input.mkv output.mkv `
  --fps 60000/1001 `
  --model-dir models/cache/rife-4.25-fp32-sdr8-v2 `
  --runtime-dir C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0/lib `
  --encoder auto --progress
```

The application does not invoke Python or acquire models. It reads local files, keeps the source intact and refuses an existing output. JSON progress separates target FPS from measured processing FPS. Ctrl+C/Ctrl+Break requests cooperative cancellation; final and temporary outputs are absent after the tested cancellation path. Abrupt process-kill cleanup/checkpoint recovery is separate future work.

## Data and timing contracts

`core/pipeline/video_pipeline` coordinates media, quality and the backend without UI dependencies. It holds adjacent source frames, one pending output and exclusive reusable GPU output storage. The clock derives each output sample from an integer index and a rational rate, rather than accumulating floating-point intervals or retaining a whole-video frame plan. Source-coincident pixels are retained; other times use a real model timestep. Conservative cut/duplicate/abnormal-pair heuristics can hold the left source frame instead of running inference.

Output sample positions follow the requested CFR grid relative to the source-video origin, including nonzero origins. The final sample duration is clipped to the actual exclusive clip end so interpolation does not extend the video. A finite clip with a partial final frame may consequently have a container average-frame-rate field different from the requested sampling rate. Exact timing is rescaled to the container's time base; the report includes any quantization error.

The pipeline cross-checks decoded final-frame duration and the declared stream end, tolerating only source-time-base rounding. Missing or contradictory end evidence fails visibly. Before publication, it re-decodes the finalized temporary output and verifies frame count, increasing PTS, maximum PTS error (half an output tick), final boundary (one tick), known color and copied audio boundaries. Failed validation removes the temporary output.

Audio/subtitle payloads are copied. Matroska `DURATION` is parsed with integer rational arithmetic. CodecDelay/initial padding and the first compressed packet's skip-samples information are retained for AAC-to-Matroska conversion. A container-rounded final audio packet duration is normalized to its declared presentation boundary without changing payload, PTS/DTS or discard side data. A separate streaming probe currently reads to the first audio packet; late-start tracks need indexed probing optimization.

Desktop Studio's `auto` mode uses the source-profile check from video import: if NVENC could not open, it selects `libopenh264` before the job starts and keeps that fallback visible while RIFE runs. If preflight succeeds, the writer still attempts a real encoder open and can fall back on an encoder-open failure. The CLI's `auto` mode performs that runtime attempt directly. Other errors and explicit encoder choices fail visibly. This machine's current SDK/driver combination cannot encode with NVENC; all current successful video encodes use software codecs.

## Evidence

The Debug and Release suites cover fractional `60000/1001`, VFR-to-144, nonzero origins, B-frame input/output drain, Unicode paths, MP4/MKV/MOV, H.264/HEVC/AV1, multi-audio, AAC priming, subtitles, chapters, cancellation and output protection. Independent packet hashes/PTS/DTS and decoded audio samples are compared. A color-metadata-incompatible optional MPEG-4/MOV case is rejected before publication.

`Pipeline.PytorchReference` uses a development-only interpreter to build an independent original PyTorch/FFmpeg video oracle. In the initial 96×64 synthetic fixture, all source-coincident/terminal pixels match exactly; three AI frames have at most one 8-bit code difference, with maximum mean error 0.00010851 codes. Generated frames also differ materially from a simple crossfade. These are bounded synthetic numerical checks, not a visual-quality or full-resolution acceptance claim. Reports are local `build/<configuration>/video-reference.json` artifacts.

## Required remaining work

The current adapter accepts explicit BT.709 or BT.470BG/SMPTE 170M 8-bit SDR tags, applies the matching YUV matrix, and preserves the stream color contract. If an H.264 frame's BT.601-family VUI tags conflict with the container tags only through the approved matrix aliases or BT.709/SMPTE 170M SDR transfer pair, frame metadata is aligned to the container tags after matching pixel layout, primaries and range are verified. HDR, high depth, unknown color and other mismatches remain unsupported. Both synthetic regression and a short clip remuxed from the reported local video now pass real CUDA RIFE, output validation, frame-count/rate/color checks and decoded-audio hash comparison. Conservative scene metrics still require real-content tuning and visual regression review. Full 480p–4K, animation/game/text/occlusion, long-video and fault matrices remain open. A native/CLI [persistent queue](QUEUE.md) now exists; safe segment checkpoints remain required alongside TensorRT/FP16, multiple models, NVDEC/NVENC compatibility, a bounded VRAM pool/overlap, HDR/10-bit color management, comparison UI, model center, installation and redistribution audit.
