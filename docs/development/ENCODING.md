# Generated-frame encoding and transcode

`VideoMuxWriter` is a native C++20, model-independent writer. It takes owned pixel planes, a rational presentation timestamp and a positive rational duration for each generated frame. It replaces one selected video stream and copies all other compatible reference-input streams. It is ready to receive frames from the future interpolation pipeline; it does not generate those frames itself.

## CLI

```powershell
.\build\windows-dev\rifelove-cli.exe transcode source.mp4 output.mp4 --encoder libopenh264 --bitrate 8000000 --progress
.\build\windows-dev\rifelove-cli.exe transcode source.mp4 output.mp4 --encoder libkvazaar
.\build\windows-dev\rifelove-cli.exe transcode source.mkv output.mkv --encoder libaom-av1 --bitrate 0 --option crf=35 --option cpu-used=8
.\build\windows-dev\rifelove-cli.exe transcode source.mkv lossless.mkv --encoder ffv1
```

`transcode` keeps input video PTS and derives each frame duration from the following PTS, holding at most two decoded frames in its adapter. The final frame must have a reliable declared duration; otherwise it fails and asks for an explicit timeline boundary. There is no FPS-changing or AI interpolation mode yet.

The default encoder setting is `h264_nvenc`; the examples explicitly choose software encoders. A selected encoder must actually open successfully. Unknown options are rejected rather than ignored, and an unavailable hardware encoder does not silently choose a different backend.

Additional controls are `--pixel-format`, `--bitrate`, `--bframes`, `--option KEY=VALUE`, and `--allow-precision-loss`. Bit-depth reduction is off by default. The explicit precision-loss flag can permit SDR reduction; it does not permit HDR precision reduction or act as tone mapping. Same-color layout conversions such as RGB channel packing or planar/semi-planar YUV are supported. RGB/YUV color conversion requires the future explicit color-managed stage.

With `--progress`, JSON Lines report frames submitted, current rational PTS and processing speed. Completion reports encoder, frame/packet counts, first PTS, exclusive video end, elapsed seconds and processing FPS. The elapsed measurement includes input/encoder initialization, decode/encode work, auxiliary copy, finalization, structural verification and publication. It describes this local transcode run, not an AI inference benchmark or a supported-hardware performance target.

## Current evidence

The local suite covers software H.264 (`libopenh264`), HEVC (`libkvazaar`), AV1 (`libaom-av1`) and FFV1. It verifies decoded PTS, encoder B-frame drain, VFR, copied AAC packet timing, multiple audio/subtitle tracks and chapters. FFV1 re-encoding is compared through decoded-frame hashes; 10-bit PQ tags/precision are checked through FFV1 and AV1 encode/decode. Unit tests feed generated frames at `60000/1001` for 1000 frames and check every decoded PTS plus the exact last-frame duration. Those repeated synthetic pixels are encoder test data, not a replacement for RIFE.

Output uses the same exclusive temporary reservation and no-replace publication as remux. Failure/cancellation closes resources and removes partial files. Pre-publication checks verify stream presence, requested depth and known color tags; publication is refused if they changed. A writer closed by failure cannot later publish a partial output.

## Actual local NVENC incompatibility

On 2026-10-17, `nvidia-smi` reported GTX 1660 SUPER, driver `560.94` and 6144 MiB. A real `h264_nvenc` encode with the pinned FFmpeg SDK failed before producing packets: the SDK build required NVENC API `13.0` while the installed driver exposed `12.2`; FFmpeg reported driver `570.0` as its minimum for this build. This is not a successful NVENC result. AV1 hardware support cannot be inferred from an `av1_nvenc` name in the registered-encoder list.

The final application still needs a compatible FFmpeg/NVENC build/driver matrix, runtime device capabilities and explicit fallback policy. The driver has not been changed. A compatible older NVENC interface can be selected in a future audited build rather than assuming that all users must update their driver.

## Remaining acceptance

The CPU writer copies owned input planes to encoder frames and uses a separate demux handle for auxiliary streams. GPU surfaces, shared demux and buffer-pool optimization are pending. Full RGB/YUV/transfer/tone-mapping handling, dynamic HDR/Dolby Vision and ICC workflows are not validated. Static mastering/content-light construction has code but still needs real metadata fixtures. The full source time-base/audio offset/container matrix, missing-PTS recovery, disk-full injection and multi-hour tests remain required. No RIFE backend, CUDA inference, desktop workflow or complete release is claimed by these encoding tests.
