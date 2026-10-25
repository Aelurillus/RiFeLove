# Local FFmpeg media engine

The C++ media engine calls libavformat, libavcodec, libavutil and libswscale directly. The FFmpeg command-line tools are only used by development probes and the integration suite to generate synthetic fixtures and independently inspect results. No video is uploaded or processed remotely.

## SDK and provenance

The tested local SDK is BtbN's Windows x64 LGPL shared build `n8.1.3-18-g46d899d7a0-20261016`. Its historical release tag, exact file name, URL and SHA-256 are recorded in `tools/development/ffmpeg-sdk.lock.json`. The checksum was compared against the historical release's `checksums.sha256` file. The floating `latest` archive has a different archive checksum and must not be substituted.

FFmpeg's official [download page](https://ffmpeg.org/download.html) links to [BtbN's Windows builds](https://github.com/BtbN/FFmpeg-Builds). The package reports LGPL version 3 or later (`ffmpeg -L`), `--enable-shared`, and neither `--enable-gpl` nor `--enable-nonfree`. This development selection is not a completed redistribution audit. A distributable build still needs the corresponding FFmpeg/dependency sources, configuration, license texts, notices and a verified installation strategy. The build-recipe repository's MIT license is not the binary package's license.

The current machine's SDK is under `C:\itzming\dependences\ffmpeg-8.1.3\`. It is not tracked by Git. To unpack a locally cached, checksum-verified archive elsewhere:

```powershell
.\tools\development\setup-ffmpeg.ps1
```

If that archive has not been supplied locally, an explicit `-AllowDownload` performs SDK acquisition. It does not push project data. The script prints the local SDK root to pass to the normal local checker:

```powershell
.\tools\development\check.ps1 -FFmpegRoot '<local SDK root>'
.\tools\development\check.ps1 -Preset windows-release -FFmpegRoot '<local SDK root>'
```

`RIFELOVE_BUILD_MEDIA=ON` enables the engine, and `RIFELOVE_FFMPEG_ROOT` points to the SDK. CMake copies the SDK DLLs next to the local development executables. Header/runtime major versions are checked when opening input. The tested ABI is FFmpeg 8.1; other FFmpeg major versions are not in the validated compatibility matrix.

## Current behavior

- `media-probe INPUT`: JSON media/track time bases, rates, dimensions, metadata, audio channels, subtitles, chapters, rotation, color and available static HDR side data.
- `media-capabilities INPUT [--fps RATE]`: opens source-profile contexts for H.264 NVENC/software, HEVC and AV1, separating FFmpeg registration from runtime initialization. No frames are encoded; the actual processing job remains the final capability check.
- `media-scan INPUT [--frames] [--stream INDEX]`: native software decode, decoder reordering/drain, original pixel format/bit depth, presentation timestamps and observed cadence. It holds only reference-counted frames requested by a callback. It does not convert decoded frames to 8-bit RGB.
- The Comparison Viewer uses `MediaEngine::decodeFrameNearTime` to seek to a key frame, decode in presentation order, select the nearest exact PTS and report adjacent decoded PTS values. Its bounded RGBA conversion uses the explicitly tagged SDR color path; HDR and unknown color are rejected until a display transform is validated. This powers exact paused frame stepping for both CFR and VFR media while normal synchronized playback remains on Qt Multimedia.
- `remux INPUT OUTPUT`: stream copy into MP4, MOV or MKV; metadata, stream parameters, side data, dispositions and chapters are copied. Incompatible streams are reported instead of silently dropped. Output is written to a unique temporary file, finalized, structurally checked and then published without replacing an existing file.
- `transcode INPUT OUTPUT`: the selected video is decoded and re-encoded at its original PTS; compatible auxiliary streams are copied. The public frame writer also accepts generated frames at explicit timestamps. See [encoding](ENCODING.md) for supported settings, evidence and limits.

Inputs must be local regular files and libavformat's input protocol whitelist contains only `file`. APIs accept `std::stop_token` and check it around decode, packet I/O and output publication. Cancellation from the CLI's console control handler and interrupted-output recovery remain future work.

## Timestamp and color details

PTS is represented in rational seconds from the input stream time base. Missing timestamps are reported; no synthetic CFR timeline is silently invented. Decoder best-effort timestamps handle presentation reordering. Observed CFR classification permits at most one input time-base tick of quantization around the nominal rate; otherwise differing intervals are reported as VFR. This avoids treating the alternating 41/42 ms Matroska timestamps for 23.976 fps as variable cadence. A full timestamp scan is required for classification; stream rate metadata alone is not conclusive.

Decoded frame storage owns original planes and side data. Shared ownership keeps retained frames valid after the decoder closes. HDR metadata reading and stream copy do not constitute a color-managed HDR interpolation pipeline. Future encoding must propagate color properties on frames as well as encoder contexts: the fixture investigation found that output color options alone did not retain primaries/transfer tags, while explicit frame `setparams` did.

## Validation and remaining work

The integration suite generates 10-bit PQ FFV1 video, two AAC tracks, SRT captions, chapter metadata, MPEG-4 B frames and VFR video. It checks native probe results, frame counts, actual decoded PTS against ffprobe, quantized CFR/VFR classification, compressed packet hashes and timestamps, audio packet timing, metadata retention, Unicode paths, MP4/MOV/MKV round trips, incompatible streams, corrupt input and no-overwrite output behavior. Unit tests cover decoder-buffer lifetime and cancellation.

This is not the complete Phase 2 gate. Full color conversion, timestamp reconstruction for missing-PTS sources, full HDR mastering metadata fixtures, rotation fixtures, broad codec/container coverage, disk-full injection and multi-hour A/V validation remain required. Structural remux validation does not prove all compressed payloads are uncorrupted. The writer accepts real RIFE-generated frames through the integrated pipeline, but this media-engine document's synthetic writer tests still exercise timing and encoding rather than interpolation quality.
