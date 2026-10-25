# Generated-frame encoding and transcode

`VideoMuxWriter` is a native C++20, model-independent writer. It takes owned pixel planes, a rational presentation timestamp and a positive rational duration for each generated frame. It replaces one selected video stream and copies all other compatible reference-input streams. The [RIFE pipeline](PIPELINE.md) now submits generated frames through this same writer.

## CLI

```powershell
.\build\windows-dev\rifelove-cli.exe transcode source.mp4 output.mp4 --encoder libopenh264 --bitrate 8000000 --progress
.\build\windows-dev\rifelove-cli.exe transcode source.mp4 output.mp4 --encoder libkvazaar
.\build\windows-dev\rifelove-cli.exe transcode source.mkv output.mkv --encoder libaom-av1 --bitrate 0 --option crf=35 --option cpu-used=8
.\build\windows-dev\rifelove-cli.exe transcode source.mkv lossless.mkv --encoder ffv1
.\build\windows-dev\rifelove-cli.exe media-capabilities source.mkv --fps 60000/1001
```

`media-capabilities` uses the selected video stream's dimensions and pixel format to open candidate encoder contexts for H.264 NVENC, software H.264, HEVC and AV1. Its JSON separates FFmpeg registration from successful context initialization and includes failure reasons. It does not submit a frame, so it is a useful driver/profile preflight but not proof of a complete encode.

`transcode` keeps input video PTS and derives each frame duration from the following PTS, holding at most two decoded frames in its adapter. The final frame must have a reliable declared duration; otherwise it fails and asks for an explicit timeline boundary. Use the separate `interpolate` command for FPS conversion and real AI interpolation.

The default encoder setting is `h264_nvenc`; the examples explicitly choose software encoders. A selected encoder must actually open successfully. Unknown options are rejected rather than ignored, and an unavailable hardware encoder does not silently choose a different backend.

Additional controls are `--pixel-format`, `--bitrate`, `--bframes`, `--option KEY=VALUE`, and `--allow-precision-loss`. Bit-depth reduction is off by default. The explicit precision-loss flag can permit SDR reduction; it does not permit HDR precision reduction or act as tone mapping. Same-color layout conversions such as RGB channel packing or planar/semi-planar YUV are supported. RGB/YUV color conversion requires the future explicit color-managed stage.

With `--progress`, JSON Lines report frames submitted, current rational PTS and processing speed. Completion reports encoder, frame/packet counts, first PTS, exclusive video end, elapsed seconds and processing FPS. The elapsed measurement includes input/encoder initialization, decode/encode work, auxiliary copy, finalization, structural verification and publication. It describes this local transcode run, not an AI inference benchmark or a supported-hardware performance target.

## Current evidence

The local suite covers software H.264 (`libopenh264`), HEVC (`libkvazaar`), AV1 (`libaom-av1`) and FFV1. It verifies decoded PTS, encoder B-frame drain, VFR, copied AAC packet timing, multiple audio/subtitle tracks and chapters. MP4/MOV remux and transcode fixtures compare both AAC tracks' packet hashes and decoded samples, preserve language labels and chapters, and account for the chapter data track these containers create. ISO 639-2/T language aliases are normalized to the ISO 639-2/B form required by the MOV/MP4 muxer (`zho` becomes `chi`). FFV1 re-encoding is compared through decoded-frame hashes; 10-bit PQ tags/precision are checked through FFV1 and AV1 encode/decode. Unit tests feed generated frames at `60000/1001` for 1000 frames and check every decoded PTS plus the exact last-frame duration. Those repeated synthetic pixels are encoder test data, not a replacement for RIFE.

Output uses the same exclusive temporary reservation and no-replace publication as remux. Failure/cancellation closes resources and removes partial files. Pre-publication checks verify stream presence, requested depth and known color tags; publication is refused if they changed. A writer closed by failure cannot later publish a partial output.

## Actual local NVENC incompatibility

On this workstation, a real `h264_nvenc` open with the pinned FFmpeg SDK fails before producing packets: the SDK build requires NVENC API `13.0` while the installed driver exposes `12.2`; FFmpeg reports driver `570.0` as its minimum for this build. The source-profile preflight reports the failure and confirms source-compatible software encoders. Studio's auto mode chooses the first compatible encoder before starting; the CLI auto mode attempts NVENC and then falls back. The 8-bit SDR path validates software H.264 output; the synthetic 10-bit path validates AV1 fallback and bit-depth preservation. These are not successful NVENC results. AV1 hardware support cannot be inferred from an `av1_nvenc` name in the registered-encoder list.

The pipeline implements an explicit `auto` policy for callers without preflight: try NVENC, then report the first compatible software fallback if NVENC initialization or source-pixel-format validation fails. Studio first reports source-profile capabilities on import, then opens the selected encoder again with the requested output frame rate, time base, pixel format, bitrate, GOP and encoder options before model loading. Automatic mode checks a compatible software encoder with those same output settings and explains that video encoding is on the CPU while RIFE interpolation remains on CUDA. Explicit encoder choices stay strict. The app still needs a compatible FFmpeg/NVENC build/driver matrix and runtime device capabilities. The driver has not been changed.

## Desktop advanced encoding settings

Studio and Queue expose a target bitrate in kbit/s and a maximum GOP length. The output encoder receives those values through the same `VideoEncodingSettings` contract used by the CLI and native writer. The selected values are stored in local preferences and in each queued job, so a retry uses the original settings.

HEVC software encoding adds a constant-QP mode (0–51) through FFmpeg's `libkvazaar` `kvazaar-params` option. AV1 software encoding adds libaom CRF mode (0–63). Explicit H.264 NVENC adds VBR + CQ mode (0–51) using FFmpeg's `rc=vbr` and `cq` options. In these quality modes the target bitrate is set to zero so the codec uses its quality-based rate control. H.264 OpenH264 remains target-bitrate only. Encoder-specific choices are checked before a Studio task starts or a queue task is added. These quality numbers are encoder controls, not a promise of equal perceptual quality across codecs.

For a machine with a working NVENC driver, the equivalent CLI transcode options are `--encoder h264_nvenc --bitrate 0 --option rc=vbr --option cq=20`. The current workstation fails the source-profile NVENC preflight, so Studio blocks that explicit selection and recommends automatic/software encoding; queue import reports the per-file incompatibility instead of creating a doomed task. The options mapping is unit-tested, but successful hardware CQ output still needs a supported driver/GPU test.

The RIFE path accepts model-declared tagged 8/10-bit SDR source profiles and keeps the source pixel format for output. Automatic mode first attempts NVENC, then probes software H.264, HEVC and AV1 compatibility and selects the first encoder that accepts the source format. NVENC pixel-format rejection is treated as a recoverable compatibility result, just like a driver/API initialization failure. A synthetic tagged 10-bit RIFE integration case confirms that this machine falls back to libaom AV1 and preserves `yuv420p10le` plus color tags. If no encoder can preserve the source depth, the pipeline fails before interpolation instead of silently converting to 8-bit. HDR, user-selected precision conversion, real-content 10-bit quality and the broader encoder/GPU matrix remain unqualified.

## Remaining acceptance

The CPU writer copies owned input planes to encoder frames and uses a separate demux handle for auxiliary streams. GPU surfaces, shared demux and buffer-pool optimization are pending. Full RGB/YUV/transfer/tone-mapping handling, dynamic HDR/Dolby Vision and ICC workflows are not validated. Static mastering/content-light construction has code but still needs real metadata fixtures. The full source time-base/audio offset/container matrix, missing-PTS recovery, disk-full injection and multi-hour tests remain required. Separate native RIFE tests now exist; these encoding tests alone do not establish neural quality or full release acceptance.
