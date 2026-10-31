# Experimental automatic Gaussian-noise conditioning

Status, 2026-10-31: persisted core opt-in with an explicit native control;
**experimental, default off, not general restoration or a release acceptance**.
Existing saved jobs keep their explicitly selected fixed noise sigma.
No new model is downloaded, bundled, selected or licensed by this change.

## Actual model contract

The existing `fastdvdnet-rgb-f32-nchw16-v1` adapter consumes five scene-bounded
RGB frames and one Gaussian-noise map, packed as 16 FP32 NCHW channels. Its
backend accepts an explicit sigma of 5 through 55 in 8-bit units; zero returns
the original center frame without model inference. It preserves center PTS,
duration, index and color metadata. Those capabilities support an estimated
noise-conditioning input. They do not establish compression repair, lost-detail
reconstruction or general deblurring.

`PipelineSettings::automaticTemporalDenoise` now enables content-dependent
conditioning in the real source-rate temporal stage. It defaults to absent.
The existing `temporalDenoiseSigma255` fixed setting retains its meaning when
the automatic opt-in is absent. Automatic settings are persisted in job schema
9 and bound to the checkpoint manifest. Native controls expose this opt-in after
an explanation and only when a validated FastDVDnet is selected.

## Estimator and conservative controls

`src/restoration/noise_estimate.cpp` estimates an additive Gaussian sigma from
the actual unpadded, nonlinear normalized RGB image. Its separable 3x3
`[1,-2,1]` filter removes constant and linear gradients; its coefficient norm
is six, so division by six preserves the standard deviation of independent
Gaussian samples. A channel-wise median absolute coefficient is scaled by the
Gaussian median-absolute constant; the median of the three channel estimates
is used for a single conditioning map.

At most 16,384 non-overlapping supports per channel are retained, using at most
192 KiB of sample elements plus vector/container overhead. Traversal is
deterministic and cancellation-aware. It reuses the RGB preparation already
performed by the source scene analysis and does not allocate another full
prepared frame. Replicated image padding is excluded. Saturated supports are
excluded; insufficient support coverage and a strongly non-Gaussian absolute
coefficient distribution lower confidence. The estimator does not silently
clamp out-of-domain observations to maximum model strength.

`AutomaticDenoiseController` applies the existing exact-PTS `DynamicController`
to sigma/55, with exponential smoothing and a maximum sigma change per source
second. Scene cuts clear the prior estimate. Relative adjustment is limited to
the supported denoise effect. Below five levels the backend receives zero;
uncertain or out-of-domain observations receive zero immediately. Zero means
an actual bypass, not an inference call at an unqualified conditioning level.
Zero-confidence observations never update the smoothing state, including when
the configured minimum-confidence threshold is explicitly zero. A later trusted
observation therefore cannot inherit a fabricated zero estimate from the gap.
That confidence protection may cause a visible strength transition and is not
a claim that flicker has been eliminated.

Source observations are held only until their corresponding temporal window
is emitted. The pipeline checks the center-frame index before applying each
observation, including clip draining and the old scene's pending tail at a cut.
No future-scene estimate is attached to a preceding frame. Results expose
whether automatic mode was active and how many source frames were analyzed,
trusted and conditioned with nonzero sigma. `serializeResult` and processing
reports expose these under `automaticGaussianDenoise`, labeled as current-run
source-frame counts. A confident observation is not necessarily conditioned:
clean images can be confidently estimated at zero.

## Persisted identity and deterministic replay

Job schema 9 always writes `automaticTemporalDenoise`. Fixed mode writes
`null`; enabled automatic mode writes exactly these six fields:

```json
{
  "algorithmVersion": 1,
  "replayPolicy": "software-source-pts-from-start-v1",
  "relativeAdjustment": 0.0,
  "smoothingSeconds": 0.35,
  "maximumSigmaChangePerSecond": 27.5,
  "minimumConfidence": 0.6
}
```

Unknown versions, unknown policies, extra fields and invalid settings are
rejected. Full job comparison binds these fields together with the existing
source, model, adapter and runtime identities. Changing any parameter rejects
checkpoint reuse. Schema 8 and older fixed-mode jobs may migrate only to a
fixed-mode job with automatic settings absent; historical fixed output cannot
be reused as automatic output. An unverified restoration checkpoint is rejected
before output publication. The pipeline relies on the queue's verified manifest
contract, rather than treating a bare frame offset as enough resume information.

Automatic mode uses software source decoding to avoid a later NVDEC/software
fallback changing the samples used by the estimator. Neural inference still
runs on the selected GPU backend. Each fresh execution decodes the source from
the beginning, replays its original PTS, reconstructs scene-bounded temporal
windows, and rebuilds estimation/smoothing state. Already committed output
segments are reused, but decoding and restoration of the source prefix are
replayed. This is intentionally not fast seeking to the checkpoint. It avoids
serializing incomplete temporal windows or guessing an earlier smoother state.
Source decode acceleration is exposed as `software` in the result. Any future
change to this policy must version the replay identity and requalify recovery.

The result's `automaticGaussianDenoise` object contains `active`,
`algorithmVersion`, `analyzedSourceFrames`, `confidentSourceFrames`,
`conditionedSourceFrames` and `countsScope: "current_execution"`. Counts include
the replayed prefix and are not newly encoded output-frame counts. Processing
reports carry the same object.

## Native integration contract

The native UI now sets
`PipelineSettings::automaticTemporalDenoise = AutomaticDenoiseSettings{...}`
through `applyExportControls` after explicit opt-in. Clearing the optional
restores the existing fixed-sigma mode. With FastDVDnet selected, the current
native fixed setting is sigma 25; automatic off does not disable that model.
Choose the off entry in the optional denoise selector to bypass the temporal
stage entirely. Automatic parameter defaults and validation are:

| Field | Default | Valid range / units |
| --- | --- | --- |
| `relativeAdjustment` | `0.0` | finite, [-1, 1], relative control |
| `smoothingSeconds` | `0.35` | finite, strictly positive, source seconds |
| `maximumSigmaChangePerSecond` | `27.5` | finite, strictly positive, sigma levels/source second |
| `minimumConfidence` | `0.6` | finite, [0, 1], heuristic threshold |

The application, not the user, supplies the algorithm version and replay policy
through serialization. Automatic mode requires a selected source-rate
FastDVDnet model meeting the existing adapter and supported SDR/color contract;
it does not select or install a model itself. The independent fixed sigma is
retained in job configuration but is not the active strength while automatic
mode is enabled. The native button opens an experimental Gaussian-denoise
explanation: Yes enables, No returns to fixed mode and Cancel retains the prior
setting. It explains intended-grain/detail loss, the separate compression/detail
limits, forced software source decode and prefix replay on resume. It defaults
off, including on a new application instance with a saved temporal-model default.
Removing the selected temporal model clears the flag; selecting a model again
does not silently re-enable it. Changing the controls requires a new output job
rather than reinterpretation of an existing checkpoint.

Executed Release native processing fixtures on 2026-10-31 verify default-off,
eligible-model opt-in, removing/reselecting the model and the model/settings
round-trip through `serializeJob`/`parseJob`. They check fixed sigma 25 against
the actual native settings and summary, automatic mode replacing that label,
switching back to still-active fixed denoise and the separate no-model/off
state. These fixture tests establish the
native setting boundary, not real-video denoising quality, GPU throughput,
packaged-app acceptance or multi-hour recovery. The source-rate quality/recovery
evidence below is separate from native HWND/control validation.

## Automated protocol and recovery regressions

Release-target unit tests verify deterministic synthetic Gaussian inputs at
sigma 5, 15 and 25 within one 8-bit level, clean linear images, ignored padding,
structured patterns, clipping, small-image uncertainty, storage bounds,
cancellation, invalid tensors, source-PTS smoothing, cut resets and exact user
adjustment endpoints. A production-pipeline test runs FFmpeg decode/encode,
CUDA upload/interpolation/download and output validation with a recording
temporal backend. Its noisy first scene receives
nonzero conditioning; the clean second scene receives zero from its first
frame. All 12 original source PTS, 24 output frames and final duration match
the fixed-setting run. Separate tests reject an absent temporal model and
unbound checkpoint configuration, and verify cancellation leaves no published
or partial output. An interruption test stops before and after a scene boundary,
constructs a fresh controller/backend, and resumes from committed output
segments. Its recording backend deliberately changes pixels according to the
requested sigma: both all source PTS/sigma requests and final decoded output
pixels/PTS must exactly match the uninterrupted run. These protocol fixtures
make no neural-quality claim.

Serialization tests round-trip all independent parameters, reject incompatible
versions/settings, and reject every changed automatic parameter or fixed-mode
history when matching automatic checkpoints.

## Actual local CUDA evidence, 2026-10-31

The native validation executable was run with the existing local evaluation
pack `models/cache/fastdvdnet-clipped-five-frame-sdr8-eval`, graph SHA-256
`b84e570aebc32610b36189ab933cf9378a64cb4985c367fd723b12e454ceb260`,
ONNX Runtime CUDA 1.22.0 on a GTX 1660 SUPER. Eight synthetic 192x128 clips,
six frames per case, were evaluated serially. The report records each raw
estimate, heuristic confidence, actual conditioning sigma, scene cut, provider
events and output/reference error. This is a small synthetic validation, not
a real-video corpus or a representative performance baseline.

| Synthetic case | Observation and true-backend result |
| --- | --- |
| Clean smooth ramps | Estimate 0, sigma 0; output byte-exact to input; no model operator events |
| Known Gaussian noise, sigma 15 | Raw estimate 14.770-15.253, smoothed sigma 15.003-15.056; PSNR 24.615 to 43.690 dB; 858 CUDA provider events |
| Noisy-to-clean cut at frame 3 | Sigma about 15 before cut, exactly 0 from the first clean frame; clean frames byte-exact; 429 CUDA provider events |
| Clean fine texture | Raw estimate about 44.3, confidence 0.485-0.498 below 0.6; sigma 0 and byte-exact output; no model operator events |
| Clean HUD-like lines/glyph strokes | Estimate 0, sigma 0, byte-exact output; no model operator events |
| Intended Gaussian film grain | **False positive:** confidence 1, sigma about 8.86-8.89, model triggered and removed intended grain; output-versus-intended-original PSNR 29.314 dB, changed MSE 76.149 |
| Saturated checker | Confidence 0, sigma 0, byte-exact output; no model operator events |
| Synthetic 8x8 block edges | Sigma 0 and byte-exact output; this is not evidence of actual codec-compression repair |

The native executable's sigma-zero path was checked with the real loaded
FastDVDnet backend: equality is not merely the behavior of a test double.
Provider profiling found CUDA model operators for conditioned frames and none
for all-bypassed cases. The absence of operator events is not a claim of zero
backend initialization or resource cost. Grain is a demonstrated semantic false
positive even with confidence 1. Confidence must never be presented as a
probability that an image contains unwanted noise.

A separate real persistent-queue test uses local RIFE 4.25 and FastDVDnet, both
reporting `onnxruntime_cuda`. It creates a short 12-source-frame noisy/clean-cut
video with FLAC audio, runs an uninterrupted reference, starts a test-owned
queue process, waits for a persisted paused checkpoint, kills that process,
and recovers the SQLite task in a fresh process. The saved schema-9 identity is
checked before recovery. All 24 final decoded frames and PTS match the reference
exactly, PCM audio matches the original/reference, the original source hash is
unchanged, and the task finishes. Four committed output frames were reused and
20 newly processed; analyzed/confident/conditioned source counts were 12/12/6
in both complete executions. Output end-exclusive time was exactly `2/5` s.
This tests real queue recovery on tiny synthetic footage; it does not qualify
multi-hour reliability or every driver/backend combination. TensorRT was not
qualified by these runs.

A follow-up real queue crash/recovery run on 2026-10-31 rebuilt the independent
x86asm-FFmpeg driver with content-bound media identity. RIFE and FastDVDnet both
reported `onnxruntime_cuda`. Fresh reference and interrupted tasks recorded the
same version-1 `mediaPipelineIdentity` in the schema-2 checkpoint manifest;
each of the four core DLL hashes was independently checked against the selected
FFmpeg SDK bytes, and the writer source digest matched the current source.
After terminating only the test-owned paused child, a fresh process accepted its
own manifest, retained its exact serialized contents and reused two unchanged
committed segments (4 output frames), then processed 20 frames. All 24 decoded
frame pixels/PTS and decoded PCM audio matched the uninterrupted output, source
bytes were unchanged, and automatic source counts remained 12/12/6. Evidence:
`work/verification/automatic-denoise-checkpoint-media-identity-x86asm-20261031/summary.json`
and `saved-checkpoint-manifest.json`. This is another tiny synthetic CUDA
recovery regression, not a broader quality or stability qualification.

## Reproducing local evidence

Both Python tools are development/validation helpers, not runtime dependencies.
They require the already available local models and dependencies and refuse to
overwrite an existing report directory. Choose a new output suffix for reruns.
No model downloads or redistribution are performed.

```powershell
# Configure these to existing local SDK/runtime directories before running.
# RIFELOVE_FFMPEG_ROOT contains bin/ffmpeg.exe; RIFELOVE_ONNX_ROOT contains lib/.
# RIFELOVE_TENSORRT_RUNTIME_DIR is the already validated combined runtime folder.
$env:PATH = "$env:RIFELOVE_FFMPEG_ROOT\bin;$env:RIFELOVE_ONNX_ROOT\lib;" + $env:PATH

work/model-tools/Scripts/python.exe tools/validation/automatic_denoise_quality.py `
  --validator build/windows-release/rifelove-validate-temporal-restoration.exe `
  --pack models/cache/fastdvdnet-clipped-five-frame-sdr8-eval `
  --runtime "$env:RIFELOVE_TENSORRT_RUNTIME_DIR" `
  --output work/verification/automatic-denoise-20261031 `
  --provider cuda

work/model-tools/Scripts/python.exe tools/validation/automatic_denoise_checkpoint.py `
  --queue build/windows-release/rifelove-queue-check.exe `
  --ffmpeg "$env:RIFELOVE_FFMPEG_ROOT/bin/ffmpeg.exe" `
  --rife-pack models/cache/rife-4.25-fp32-sdr8-v3 `
  --temporal-pack models/cache/fastdvdnet-clipped-five-frame-sdr8-eval `
  --runtime "$env:RIFELOVE_TENSORRT_RUNTIME_DIR" `
  --input-rgb work/verification/automatic-denoise-20261031/noisy_to_clean_scene_cut/input.rgb `
  --output work/verification/automatic-denoise-checkpoint-20261031
```

Reports live in the ignored `work/verification` tree:
`automatic-denoise-20261031/summary.json` and
`automatic-denoise-checkpoint-20261031/summary.json`.

## Remaining limits

Nonlinear texture can share the same statistics as noise. Correlated noise,
chroma-only noise, intended grain, clipping, compression damage and motion are
not reliably classified by this spatial estimator. The demonstrated grain
failure alone prevents enabling this mode by default. Real noisy/clean video
pairs, actual codec-compression cases, perceptual and temporal-flicker review,
representative-resolution performance, broader GPU/backend recovery tests and
model-weight redistribution rights remain unqualified. This is additive
Gaussian-noise conditioning, not general restoration, lost-detail recovery,
deblurring, or a Topaz-equivalent enhancement system.
