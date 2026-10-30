# Platform export profiles

This document defines local export presets for creator uploads. RifeLove does not upload videos or connect to platform accounts; the user reviews the file and uploads it themselves. A platform preset controls the local output encode and never changes the source file.

## Two outputs, two purposes

- **High-frame-rate master:** preserve the requested local target such as 120, 240, 300, 360, 480 or 600 FPS. This file is for local playback, editing and archival. It is independent of any platform's upload or playback limits.
- **Platform copy:** use a local encoding preset whose suggestions are distinguished from verified platform requirements. YouTube, Bilibili and Douyin have 60 FPS options; Douyin also offers 30 FPS. Bilibili 120 FPS remains an explicitly historical experimental option, with no current eligibility or playback guarantee until an account upload is tested.

Platforms may transcode the submitted file. A high bitrate cannot prevent that transcode; the preset should avoid unnecessary generation loss by matching the intended output rate, aspect ratio, resolution, color metadata and codec.

## Verified platform guidance

### YouTube

The current YouTube upload guidance recommends matching the recording frame rate, lists 24/25/30/48/50/60 as common, and says other rates are accepted. It recommends MP4, progressive H.264, 4:2:0, variable bitrate and BT.709 for SDR. For high-frame-rate SDR uploads, the recommended video bitrates are 12 Mbps at 1080p, 24 Mbps at 1440p and 53–68 Mbps at 2160p. These are upload recommendations, not a guarantee of the exact playback transcode. See [YouTube recommended upload encoding settings](https://support.google.com/youtube/answer/1722171?hl=en).

The local SDR bitrate helper follows YouTube's standard/high-rate table. At 24/25/30 FPS it selects 5 Mbps for 720p, 8 Mbps for 1080p, 16 Mbps for 1440p, 45 Mbps for 2160p, and 160 Mbps for 8K (the upper end of YouTube's published 8K range). At 48/50/60 FPS it selects 7.5, 12, 24, 68 and 240 Mbps respectively. Values between those resolutions use the corresponding published row. Rates above 60 FPS use the high-rate bitrate estimate only; they do not imply playback support. The YouTube encoder GOP is set to half the frame rate in frames (30 frames at 60 FPS), with a closed GOP. The 300+ FPS master remains a separate local output. RifeLove uses 60 FPS as the default YouTube platform-copy policy because YouTube's playback help gives 1080p120 as an example that is listed as 1080p60; this is a compatibility policy, not a claim that YouTube rejects every higher-rate upload.

The Bilibili and Douyin local presets use a two-second GOP and `Auto` encoder selection because the reviewed upload pages do not publish a current GOP requirement for these workflows. `Auto` may select software H.264 when NVENC preflight fails.

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

## Native Win32 preset behavior

The native picker creates one local output at a time. Selecting a platform copy
does not upload anything, connect an account, or promise a platform playback tier.
The displayed frame rate is the current output setting, including manual changes;
60 FPS with natural exposure is still a 60 FPS file, not 144/240 FPS playback.

| Initial selection | Target / container | Natural exposure | Encoding policy |
| --- | --- | --- | --- |
| Local master | User rate / MKV | Off by default; editable | User encoder; independent of platform copies |
| YouTube copy | 60 FPS / MP4 | 180 degrees, 4 samples | Auto; SDR bitrate table; half-rate GOP |
| Bilibili copy | 60 FPS / MP4 | 180 degrees, 4 samples | Auto; local bitrate estimate; two-second GOP |
| Bilibili historical experiment | 120 FPS / MP4 | Off by default; editable | Auto; local estimate; account testing required |
| Douyin copy | 30 FPS / MP4 | Off by default; editable | Auto; local bitrate estimate; two-second GOP |
| Douyin copy | 60 FPS / MP4 | 180 degrees, 4 samples | Auto; local bitrate estimate; two-second GOP |

Choose **Parameters** (`参数`) next to natural exposure to edit the shutter angle
and sample count, or turn the checkbox off. The master and each platform preset
remember their rate selection, exact custom rational text, encoder and exposure
controls independently for the current window session. Returning to the master
restores rates such as 300/360/480/600 FPS and the user's exposure settings, and
selects MKV again. Reopening a platform preset restores its manual changes rather
than reapplying defaults. These session choices are not saved across app restarts.

The Save dialog initially selects MP4 for platform copies and MKV for the master;
the user can still choose a different output path/container. The summary shows the
actual current extension, target FPS, selected encoder and requested bitrate/GOP.
Changing an encoder does not silently reselect Auto. Fractional rates remain exact
in job settings; only the bitrate/GOP policy uses an integer ceiling. The shared
`platformFrameRateCeiling` computes quotient plus nonzero remainder, avoiding an
overflow-prone numerator-plus-denominator calculation. Thus `30000/1001` selects
the 30 FPS policy and `60000/1001` the 60 FPS policy, with no hardcoded 60 FPS
fallback. Invalid or unrepresentable rates/GOP sizes fail instead of silently
substituting another setting. YouTube uses half the rounded policy rate (rounded
up to a whole frame); other local compatibility presets keep a two-second GOP.

Batch import receives the selected rate, encoder and exposure, selects MP4/MKV
from the preset, and calls `application::platformExportSettings` independently
for every probed source. Bitrate buckets use the **shorter edge of the actual
output size**, so 1920x1080 and 1080x1920 select the same 1080 tier. This is
RifeLove's orientation-independent local policy, not an additional claim about a
platform's portrait-upload eligibility. Without an explicit target size, the
helper uses the source dimensions multiplied by the selected spatial model's
scale. An explicit width/height pair overrides that generated size for the
bitrate calculation. The pipeline/import aspect-ratio and generated-size checks
remain responsible for whether such a target is actually supported; the policy
helper does not crop or resize frames. Requested bitrate is an encoder setting,
not a guaranteed measured bitrate or immunity from platform transcoding.

### Shared native/queue policy API

Native single-file `applyExportControls` now calls the same helper as batch
import after resolving source width/height, selected model scale and explicit
output target. Neither path maintains its own height-only bitrate table:

```cpp
const auto policy = application::platformExportSettings(
    profile, settings.targetRate, sourceWidth, sourceHeight,
    settings.encoding.outputWidth, settings.encoding.outputHeight,
    scaleNumerator, scaleDenominator);
settings.encoding.bitRate = std::int64_t{policy.bitRateKbps} * 1000;
settings.encoding.gopSize = policy.gopSizeFrames;
```

The returned `frameRateCeiling`, `outputWidth` and `outputHeight` describe policy
inputs for display/diagnostics. Do not replace the exact rational target FPS
with `frameRateCeiling`, and do not force computed dimensions into encoding just
to calculate bitrate. `unchanged` means skip the helper and preserve explicit
user encoding settings. Native HWND regression checks verify YouTube 60 FPS
uses the same 12 Mbps policy for portrait 1080x1920 and 4:3 1440x1080. These
control-setting checks do not perform an encode or a platform upload.

CPU-only regression coverage uses real FFV1 probe inputs at 1280x720 and
1080x1920, metadata-only RIFE/spatial-model fixtures and persisted SQLite queue
records. It covers both `30000/1001` and `60000/1001`, YouTube/other local
profiles and 1x/2x output scales across eight independent two-source imports.
Each record must retain its exact FPS and have its own expected bitrate/GOP,
overriding deliberately incorrect copied input settings. Additional helper tests
cover portrait/landscape equivalence, explicit scaled targets, fractional-pixel
dimensions, partial target pairs and integer overflow. Import does not start
processing; this evidence does not claim actual model inference, platform
upload, encoder throughput or visual-quality acceptance.

## Current implementation boundary

The platform picker selects one local output profile at a time and applies it to a single source or to each batch source. YouTube gets its current SDR bitrate recommendation and half-frame-rate GOP; Bilibili and Douyin get local compatibility bitrate estimates and a two-second GOP. Platform profiles initially select `Auto`; manual encoder changes remain available and are retained within the session. The picker does not upload files, set output resolution or crop, or force AAC audio; compatible source audio is preserved by the media pipeline. The profile does not guarantee the platform's final codec, stream bitrate or playback tier. When motion blur is enabled, RifeLove temporally integrates interpolated samples into the selected output frame rate; a 60 FPS upload can gain smoother-looking motion and natural exposure blur, but it cannot carry the actual temporal detail or playback cadence of 144/240 FPS. Inspect the finished file with `ffprobe` before uploading. The platform-copy labels describe intended use, not a successful platform upload.

## Remaining qualification work

### Exact exposure window implementation

The motion-blur pipeline now retains source frames by their exact rational PTS
until the **full shutter interval** is covered, rather than retaining only four
adjacent frames. At output time `t`, the interval is
`[t - angle/(720*rate), t + angle/(720*rate)]`; midpoint samples within that
interval are synthesized and accumulated in linear SDR light. Dense 240, 300,
360, 480 and 600 FPS sources therefore do not silently lose the requested blur
when converted to 60 FPS. The same planner uses actual timestamps for VFR and
fractional output rates. It waits for real lookahead and only holds edge frames
at the true start/end of the source. A cut, duplicate or abnormal-motion barrier
anywhere within the full shutter interval causes an explicit center/left hold,
including a barrier that falls between the quadrature sample positions.

History older than the next shutter's left bracket is discarded continuously,
including while decoding between output ticks. Prepared source-window float
storage is limited by `hostFloatBufferLimit` (512 MiB by default) and an absolute
4096-frame cap, independently of clip length. Decoder storage, the accumulator,
sample output and optional restoration stages have their own bounded buffers;
the float-window limit is not a promise of total process RAM usage. A shutter
that cannot fit fails with an explicit memory-budget error; it is never silently
reported as successful blur. This can require a larger budget for high-resolution,
very dense input or unusually low target FPS.

Serialized pipeline-result `motionBlur` / report `qualityDiagnostics.motionBlur`
separate integrated `blurredFrames` from `fallbackFrames` and `fallbackReasons`,
and report edge holds plus peak source-window frames/prepared bytes. These counts
cover the current execution only; verified checkpoint output is counted as reused,
not as newly blurred. Enabled job/checkpoint configurations bind
`motionBlur.algorithmVersion = 2`; segments from the former four-frame algorithm
cannot be mixed with the new exposure output. Existing completed files are not
changed. Legacy configurations can be read, but an enabled legacy checkpoint
requires a fresh task/output rather than reusing incompatible segments.

Added regression sources cover 240/300/360/480/600-to-60, 60000/1001 targets,
180/360-degree shutters, 4/16 samples, bounded streaming history, nonzero-origin
VFR, full-shutter lookahead, cuts/abnormal/duplicate barriers between sample
positions, true edge holds, report counters and checkpoint algorithm identity.
They are not a perceptual-quality claim. Run the Release unit and native media
validation suites before treating this implementation as qualified.

1. Add platform profiles to Studio and Queue and persist the chosen profile locally. Implemented: YouTube 60 FPS, Bilibili 60 FPS and experimental 120 FPS, and Douyin 30/60 FPS presets; all produce local MP4 settings and never upload from RifeLove.
2. Test H.264/AAC container compatibility and metadata on each platform's current upload flow.
3. Upload short, consented samples through real Bilibili, Douyin and YouTube accounts; inspect the processed playback stream and compare it with the local file.
4. Re-check official platform rules before each release. Do not infer a 300+ FPS playback guarantee from accepting a high-frame-rate upload file.
