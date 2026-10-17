# Foundation architecture decisions

- **Time values:** reduced signed 64-bit rational seconds and rational FPS. Arithmetic and output-count overflow fail explicitly. Input PTS must be strictly increasing; callers must supply an exclusive clip end because a final packet timestamp alone does not define the last frame duration.
- **Output sampling:** timestamps are `origin + output_index / target_rate`, emitted while less than the exclusive end. Source timestamps map directly to source frames. Intermediate timestamps map to neighboring VFR frames with an exact rational timestep. Times after the last source PTS and before the clip end use an explicit terminal hold.
- **Task persistence:** SQLite WAL + FULL synchronous mode stores paths, timestamps, state and error. State changes follow a checked state machine. Startup recovery changes `running` to `interrupted` and clearly says checkpoint resume is not implemented. A frame counter is not a valid segment checkpoint.
- **Inference/media boundary:** future async decoder, encoder, quality analyzer and interpolation backends belong behind core interfaces and application services. UI and CLI must not own FFmpeg/CUDA contexts.
- **Dependency provenance:** SQLite 3.50.4 source archive is pinned by SHA-256; GoogleTest 1.17.0 is pinned by its upstream commit. They are fetched into the build tree, not vendored. Do not ship the build artifact until Qt, FFmpeg and NVIDIA library configuration, model rights and notices have been reviewed for the exact release build.

## Current acceptance scope

Unit tests assert rational arithmetic/order/overflow, VFR 60 fps scheduling, exact source-frame recognition, terminal-frame behavior, invalid PTS rejection, task state rules, persistence and interrupted-task discovery. They do not assert video decoding, pixels, AI quality, audio sync or GPU execution.
