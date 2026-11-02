# Experimental automatic deblur pipeline

This is a development-only, default-off path. It estimates directional soft
edges from each original source frame, smooths the control using exact source
PTS, and requests a capped residual blend from a separately selected
same-resolution deblur model. Low-confidence frames, scene-cut starts, and
cross-scene holds bypass the blend. Interpolated frames use the lower strength
of their two source endpoints. It does not infer lost detail or reproduce a
commercial product's model.

An independent deblur slot can run before the existing output-rate
super-resolution stage. Legacy single-slot deblur remains readable.
Exposure-synthesis motion blur is explicitly rejected when the automatic
detail path is enabled. The native desktop exposes an independently selectable
local deblur model and an automatic detail control. Both are off at startup;
selecting a model with automatic detail off requests fixed full deblurring.
Model eligibility and the declared color profile remain mandatory.

Schema 14 adds an independent deblur identity and fixed deblur-before-SR order;
schema 13 persists the optional settings, analyzer version and software-source
decode/replay policy. On checkpoint resume, the source is replayed from the
beginning to rebuild the controller, and the model/runtime, order, settings and
analyzer identities must match. Older schema 12 jobs remain disabled by default.
Source analysis statistics include replay; output application counts describe
only newly written frames. Software source decode may cost performance.

`AutomaticDetailPipeline.SourcePtsSceneBypassAndReplayPreserveOutput` uses a
synthetic source, linear interpolation and a recording mock restoration backend
to check strengths, cut behavior, exact requested PTS and decoded pixel equality
between continuous and resumed runs. This is pipeline evidence, not real model
or perceptual validation. The separate local [NAFNet proxy experiment](../development/NAFNET_RECORDED_PROXY_QUALITY.md)
evaluates three recorded frames and an artificial blur; naturally soft detail
can still trigger the heuristic. Broader real footage, deliberate soft-focus
false positives, temporal quality, real two-model GPU workflows, combined
SR/motion-blur workflows, performance and model redistribution rights remain
open before the experimental mode can be qualified as a default or release feature.

## Local CUDA queue recovery check

On 2026-11-02, a development-only runner processed eight generated 384×256
BT.709 SDR frames with RIFE and NAFNet CUDA into 16 frames at 60 FPS. It stopped
after one committed two-frame segment, restarted the queue and reused those two
frames. The committed segment hashes, source hash and output PTS matched; six
output frames received a positive deblur request and the eight frames after the
synthetic scene cut matched the no-restoration baseline. The direct NAFNet probe
recorded 4,075 CUDA provider events. The continuous run took about 21.2 seconds,
including 14.0 seconds in restoration; the no-restoration baseline took 1.3
seconds. Process startup and model loading are included in wall times.

The recovered output was not bit-identical to the continuous run: 3 of
4,718,592 RGB channels differed, by at most one 8-bit level. Separate repeated
uninterrupted runs of the same source also produced sparse differences up to
three levels; their source of numerical variability has not been isolated.
The local test therefore bounds the comparison to three levels and at most
0.01% changed channels while retaining exact PTS and checkpoint hash checks.
This empirical threshold is specific to this small fixture, not a guarantee
for other devices or material. See `tools/validation/automatic_detail_gpu_e2e.py`
and its retained local report for the measurements. This is not real-footage,
audio, TensorRT, combined-super-resolution, multi-hour or visual-quality
qualification.

## Local CUDA dual-model check

An additional local test ran the same synthetic 384×256 source through RIFE,
NAFNet and Real-ESRGAN x2 on a GTX 1660 SUPER. The output had 16 frames at
60 FPS and 768×512; an instrumented run recorded 32 ordered stage events,
NAFNet followed by Real-ESRGAN at each exact output PTS. The saved checkpoint
identified both models and the fixed order. Direct model probes recorded CUDA
events for each model; the NAFNet probe also assigned some events to CPU.

The queued run took 44.4 seconds wall time, compared with 24.5 seconds for a
super-resolution-only control; total GPU memory sampled during the dual run
peaked at 2,862 MiB, including desktop and other processes. These numbers are
not a benchmark for larger clips. The result covers one short synthetic SDR
video, not real-footage quality, two-model checkpoint recovery, audio, HDR,
NVENC or other GPU configurations. The reproducible local check is
`tools/validation/dual_image_cuda_e2e.py`; it is a developer tool and does not
ship with the desktop app.
