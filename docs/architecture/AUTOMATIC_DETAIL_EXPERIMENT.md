# Experimental automatic deblur pipeline

This is a development-only, default-off path. It estimates directional soft
edges from each original source frame, smooths the control using exact source
PTS, and requests a capped residual blend from a separately selected
same-resolution deblur model. Low-confidence frames, scene-cut starts, and
cross-scene holds bypass the blend. Interpolated frames use the lower strength
of their two source endpoints. It does not infer lost detail or reproduce a
commercial product's model.

The existing output-rate restoration slot is used for the deblur model, so this
path cannot currently run alongside super-resolution. Exposure-synthesis motion
blur is explicitly rejected when the automatic detail path is enabled. There
is currently no native desktop control for this experiment; ordinary jobs leave
it off. Model eligibility and the declared color profile remain mandatory.

Schema 13 persists the optional settings, analyzer version and software-source
decode/replay policy. On checkpoint resume, the source is replayed from the
beginning to rebuild the controller, and the model/runtime, settings and
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
false positives, temporal quality, combined SR/blur workflows, performance and
model redistribution rights remain open before presenting a user-facing mode.
