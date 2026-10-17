# RifeLove

RifeLove is an offline Windows video frame interpolation application in active development. Its complete product scope is recorded in [the Master Plan](docs/MASTER_PLAN.md); this repository currently contains the first tested core foundations, not a finished video processor.

## Current state

- Exact rational time arithmetic and a VFR-to-CFR output timeline planner.
- A persistent SQLite task store with validated state transitions and interrupted-task recovery.
- A JSON-lines CLI for planning an output timeline and inspecting/recovering tasks.
- CMake/Ninja builds and automated unit tests. Qt desktop shell is enabled automatically when Qt 6 Quick is present.

Media decoding/encoding, RIFE inference, CUDA/TensorRT integration, the desktop workflow, and packaging remain roadmap work. The software does not yet generate interpolated video.

## Build

Use CMake 3.25+, Ninja, a C++20 compiler, and network access for the pinned SQLite and GoogleTest source archives on the first configure. The local development presets select MinGW; the CI build validates the native MSVC compiler. Configure, build, and test with:

```powershell
cmake --preset windows-dev
cmake --build --preset windows-dev
ctest --preset windows-dev
```

The core and CLI build without Qt or NVIDIA SDKs. A release build can be configured with `cmake --preset windows-release`.

## CLI

`rifelove-cli timeline --fps 60/1 --end 2/1 --pts 0/1,1/24,1/12,1/8` emits one JSON object per output frame. The PTS values are seconds relative to the clip origin; `--end` is the exclusive clip end. This command plans timestamps and source-frame pairs; it does not decode or interpolate pixels.

`rifelove-cli tasks <database-path>` lists tasks and marks tasks left in `running` as `interrupted` before listing. `rifelove-cli task-create <database-path> <source> <output>` persists a new queued task.

## Licensing

The application source is licensed under MIT. SQLite is public domain, and GoogleTest is Apache-2.0; both are fetched at pinned versions and are not copied into the repository. FFmpeg, Qt, NVIDIA libraries, RIFE code and weights require separate version/build-specific redistribution review before packaging. No model weights or vendor runtime binaries are included.
