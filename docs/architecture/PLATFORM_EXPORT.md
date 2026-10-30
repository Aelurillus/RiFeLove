# Platform export profiles

This document defines local export presets for creator uploads. RifeLove does not upload videos or connect to platform accounts; the user reviews the file and uploads it themselves. A platform preset controls the local output encode and never changes the source file.

## Two outputs, two purposes

- **High-frame-rate master:** preserve the requested local target such as 120, 240, 300, 360, 480 or 600 FPS. This file is for local playback, editing and archival. It is independent of any platform's upload or playback limits.
- **Platform copy:** use the selected platform's documented compatibility range, container, color tags, dimensions and bitrate guidance. RifeLove defaults platform copies to 60 FPS; it offers 120 FPS only as an explicitly experimental Bilibili option based on historical official documentation, with no current playback guarantee until an account upload is tested.

Platforms may transcode the submitted file. A high bitrate cannot prevent that transcode; the preset should avoid unnecessary generation loss by matching the intended output rate, aspect ratio, resolution, color metadata and codec.

## Verified platform guidance

### YouTube

The current YouTube upload guidance recommends matching the recording frame rate, lists 24/25/30/48/50/60 as common, and says other rates are accepted. It recommends MP4, progressive H.264, 4:2:0, variable bitrate and BT.709 for SDR. For high-frame-rate SDR uploads, the recommended video bitrates are 12 Mbps at 1080p, 24 Mbps at 1440p and 53–68 Mbps at 2160p. These are upload recommendations, not a guarantee of the exact playback transcode. See [YouTube recommended upload encoding settings](https://support.google.com/youtube/answer/1722171?hl=en).

The preset keeps an existing 300+ FPS master separate from the YouTube copy. The Studio preset uses the official high-frame-rate bitrate by the current source height, with 1080p as the no-video fallback. Queue applies one bitrate to all selected files and uses the official 1080p reference, so mixed resolutions may need a manual adjustment. The official upload page says that other rates are acceptable, but YouTube's playback help gives 1080p120 as an example that is listed as 1080p60; therefore RifeLove uses 60 FPS as the default platform-copy policy until a higher-rate upload is tested end to end. This is a product compatibility policy, not a claim that YouTube rejects every higher-rate upload.

The local preset uses a two-second GOP (`2 × output FPS`) and `Auto` encoder selection. That is a practical local starting point, not a claim that it exactly matches YouTube's half-frame-rate GOP recommendation or that hardware NVENC will be available. On this workstation, `Auto` may select software H.264 after NVENC preflight fails.

### Bilibili

Bilibili's official quality announcement documents 720p60 and 1080p60 playback tiers. The page is an older announcement and includes historical limits, so its old bitrate numbers must not be treated as current upload rules. See [Bilibili's 60 FPS quality announcement](https://www.bilibili.com/blackboard/activity-quality2pc.html).

Bilibili also published an official 4K120 activity page. This confirms a historical 120 FPS workflow, but does not establish current eligibility or playback behavior for every account. The 120 FPS option is therefore labeled experimental and needs a fresh upload test through the user's account. No official source reviewed here establishes 300+ FPS upload or playback support. See [Bilibili's 4K120 activity page](https://www.bilibili.com/blackboard/activity-4K120FPS-PC.html).

The current 60 and 120 FPS profiles select MP4 but leave video encoding on `Auto`; they do not guarantee H.264. Both use local bitrate estimates, not current platform rules. Before relying on either, check Creator Center and inspect the selected file's actual codec plus a sample upload through the user's account tier. H.264 MP4 remains a compatibility recommendation, not a locked implementation setting.

### Douyin

The official Open Platform upload API documents MP4 and WebM, recommends 720p or higher and describes a 128 MiB / 15 minute constraint for that API workflow. It does not document an upload FPS ceiling on this page. These API-specific limits must not be applied to ordinary Creator Center uploads. See [Douyin Open Platform video upload](https://open.douyin.com/platform/resource/docs/openapi/video-management/douyin/create/upload).

The initial local preset should preserve the input aspect ratio unless the user explicitly selects portrait framing, recommend H.264 MP4 for broad compatibility, and offer 30 and 60 FPS. Do not crop landscape video automatically. The current preset selects MP4 with `Auto` video encoding, so it does not guarantee H.264. The API page does not publish an FPS ceiling; it lists a 128 MiB/15-minute file requirement and separately permits chunked uploads up to 4 GB. These rules apply to that API workflow, not automatically to ordinary Creator Center uploads. The frame-rate, size and playback limits still need verification through the exact upload path the user uses.

## Encoding behavior

- Preserve the original aspect ratio by default. A portrait preset may offer a previewable crop, never a silent crop.
- Use progressive SDR BT.709 with explicit color metadata when the source is SDR. Keep HDR handling separate until a full HDR export path is validated.
- Preserve source audio and its timing when the container supports it. For a platform copy, use AAC when a transcode is required; YouTube's current upload guidance specifies AAC-LC or Opus at 48 kHz, while the other platform pages reviewed here do not publish an equivalent complete audio matrix.
- Use encoder quality controls and platform-specific bitrate guidance. Do not upscale only to trigger a different platform transcode; spatial upscaling remains an explicit restoration choice.
- Display the final resolution, frame rate, codec, estimated output size and any unverified platform assumption before writing the file.
- Keep the high-frame-rate master and platform copy as separately named outputs so the user does not mistake a platform copy for the 300+ FPS master. The current UI selects one local output profile at a time; it does not automatically render both files.

## Current implementation boundary

The current profile picker writes one local MP4 job at a time. It sets the target rate, a local bitrate estimate, a two-second GOP and `Auto` video-encoder selection. It does not upload files, set output resolution or crop, or force AAC audio; compatible source audio is preserved by the media pipeline. The profile does not guarantee the platform's final codec, GOP, stream bitrate or playback tier. Inspect the finished file with `ffprobe` before uploading. The platform-copy labels describe intended use, not a successful platform upload.

## Remaining qualification work

1. Add platform profiles to Studio and Queue and persist the chosen profile locally. Implemented: YouTube 60 FPS, Bilibili 60 FPS and experimental 120 FPS, and Douyin 30/60 FPS presets; all produce local MP4 settings and never upload from RifeLove.
2. Test H.264/AAC container compatibility and metadata on each platform's current upload flow.
3. Upload short, consented samples through real Bilibili, Douyin and YouTube accounts; inspect the processed playback stream and compare it with the local file.
4. Re-check official platform rules before each release. Do not infer a 300+ FPS playback guarantee from accepting a high-frame-rate upload file.
