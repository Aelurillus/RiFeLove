# Platform export profiles

This document defines local export presets for creator uploads. RifeLove does not upload videos or connect to platform accounts; the user reviews the file and uploads it themselves. A platform preset controls the local output encode and never changes the source file.

## Two outputs, two purposes

- **High-frame-rate master:** preserve the requested local target such as 120, 240, 300, 360, 480 or 600 FPS. This file is for local playback, editing and archival. It does not promise that a social platform will preserve playback above 60 FPS.
- **Platform copy:** use the selected platform's currently documented compatibility range, container, color tags, dimensions and bitrate guidance. The first implementation should cap platform copies at 60 FPS unless current official documentation and an upload test establish a higher playback rate.

An upload platform transcodes the submitted file. A high bitrate cannot prevent that transcode; the preset should avoid unnecessary generation loss by matching the intended output rate, aspect ratio, resolution, color metadata and codec.

## Verified platform guidance

### YouTube

The current YouTube upload guidance recommends matching the recording frame rate, lists 24/25/30/48/50/60 as common, and says other rates are accepted. It recommends MP4, progressive H.264, 4:2:0, variable bitrate and BT.709 for SDR. For high-frame-rate SDR uploads, the recommended video bitrates are 12 Mbps at 1080p, 24 Mbps at 1440p and 53–68 Mbps at 2160p. These are upload recommendations, not a guarantee of the exact playback transcode. See [YouTube recommended upload encoding settings](https://support.google.com/youtube/answer/1722171?hl=en).

The preset should keep an existing 300+ FPS master separate from the YouTube copy. Until a YouTube upload is tested end to end, use the documented 48–60 FPS guidance for the platform copy and make the selected output rate visible before processing.

### Bilibili

Bilibili's official quality announcement documents 720p60 and 1080p60 playback tiers. The page is an older announcement and includes historical limits, so its old bitrate numbers must not be treated as current upload rules. See [Bilibili's 60 FPS quality announcement](https://www.bilibili.com/blackboard/activity-quality2pc.html).

The initial preset should target 1080p60 H.264 MP4 as a compatibility option, while leaving bitrate configurable and labeling the current upload limits as unverified. Before publishing a stronger preset, check the current Creator Center rules and upload a sample through the user's actual account tier.

### Douyin

The official Open Platform upload API documents MP4 and WebM, recommends 720p or higher and describes a 128 MiB / 15 minute constraint for that API workflow. It does not document an upload FPS ceiling on this page. These API-specific limits must not be applied to ordinary Creator Center uploads. See [Douyin Open Platform video upload](https://open.douyin.com/platform/resource/docs/openapi/video-management/douyin/create/upload).

The initial local preset should preserve the input aspect ratio unless the user explicitly selects portrait framing, export H.264 MP4, and offer 30 and 60 FPS. Do not crop landscape video automatically. The frame-rate and size limits still need verification through the exact upload path the user uses.

## Encoding behavior

- Preserve the original aspect ratio by default. A portrait preset may offer a previewable crop, never a silent crop.
- Use progressive SDR BT.709 with explicit color metadata when the source is SDR. Keep HDR handling separate until a full HDR export path is validated.
- Preserve source audio and its timing when the container supports it. For a platform copy, use AAC when a transcode is required.
- Use encoder quality controls and platform-specific bitrate guidance. Do not upscale only to trigger a different platform transcode; spatial upscaling remains an explicit restoration choice.
- Display the final resolution, frame rate, codec, estimated output size and any unverified platform assumption before writing the file.
- Keep the high-frame-rate master and platform copy as separately named outputs so the user does not mistake a 60 FPS platform copy for the 300+ FPS master.

## Remaining qualification work

1. Add platform profiles to Studio and Queue and persist the chosen profile locally.
2. Test H.264/AAC container compatibility and metadata on each platform's current upload flow.
3. Upload short, consented samples through real Bilibili, Douyin and YouTube accounts; inspect the processed playback stream and compare it with the local file.
4. Re-check official platform rules before each release. Do not infer a 300+ FPS playback guarantee from accepting a high-frame-rate upload file.
