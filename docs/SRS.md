# RifeLove software requirements specification

Version 0.1 implementation baseline. The authoritative complete scope is [the original Master Plan V1.0](MASTER_PLAN_V1.0.zh-CN.md). All P0, P1 and P2 items remain required for the requested final product. Local-only development defers remote hosting/publishing; it does not remove product functionality.

| ID | Required behavior and acceptance evidence | Current state |
| --- | --- | --- |
| R01 | Windows 10/11 x64 desktop and CLI; normal import-to-output workflow tested on supported Windows/GPU matrix | CLI foundation; Qt shell unbuilt |
| R02 | C++20, Qt6/QML, FFmpeg libraries, CUDA, RIFE, ONNX CUDA, TensorRT, SQLite, CMake/Ninja, vcpkg, spdlog; Python only for development | Partial native core and FFmpeg integration |
| R03 | File/drop/folder batch import; accurate video, rate/VFR/HDR, audio and subtitle inspection | Native probe and scan implemented; desktop/batch import pending |
| R04 | Integer/non-integer/fractional targets, VFR input, CFR and advanced source-like output; unchanged duration with validated final-frame/audio boundary | Rational planner and scan tested; processing output modes pending |
| R05 | Real multi-model RIFE at arbitrary timestep, Fast/Balanced/Quality/Custom presets | Not implemented |
| R06 | PyTorch reference, ONNX export/operator/shape/timestep checks, ONNX CUDA numerical parity, TensorRT/FP16 validation | Not implemented |
| R07 | Async inference, CUDA streams, cancellation/error contracts and output-buffer reuse | Decoder cancellation/ownership only; inference interfaces pending |
| R08 | Model download/checksum/version/import/manifest/license/capability management; no weights or local engines in Git | Not implemented |
| R09 | GPU/driver/VRAM/compute/codec detection; safe precision/backend selection with truthful unavailable states | Not implemented |
| R10 | NVDEC/NVENC, software fallbacks, FP16, VRAM pool/budget, bounded concurrency and OOM handling | Not implemented |
| R11 | Version/GPU/model/options keyed TensorRT cache, profiles, streams/graphs and optional overlapping tiling evaluated by repeatable benchmarks | Not implemented |
| R12 | MP4/MKV/MOV and H.264/HEVC/AV1 according to actual hardware; CRF/CQ/bitrate/expert encoder settings | Stream-copy muxing tested; encoding pending |
| R13 | Preserve compatible multi-audio/subtitles, chapters, language, rotation and metadata; explicit incompatibility and sync checks | Stream copy tested on synthetic subset; full matrix pending |
| R14 | Preserve bit depth/HDR and perform correct color conversions; explicitly validated SDR/HDR pipeline | Original decode precision and tags retained; conversion/interpolation pending |
| R15 | Cut detection and protection; abnormal motion, occlusion, duplicate, animation, HUD/text, flicker, border/seam policies | Not implemented |
| R16 | Queue, pause/resume/cancel/retry/history, persistent configuration/model/file identity and progress | Task record state machine only |
| R17 | Crash ownership detection and safe segment-boundary checkpoint/resume/stitching; multi-hour and fault-injection tests | Explicit interrupted-state maintenance only |
| R18 | Home, Studio, Comparison Viewer, Queue and Model Center; simple normal controls and explained/resettable advanced settings | Placeholder shell only |
| R19 | Preview/split comparison, synchronized playback, frame stepping and artifact navigation | Not implemented |
| R20 | CLI batch automation and JSON progress; processing speed distinguished from output FPS | JSON inspection commands only |
| R21 | Artifact/performance reports and diagnostics; truthful telemetry and end-to-end speed measurements | Local development test reports only |
| R22 | Model/pre/postprocessing extension interfaces; zh-CN, zh-TW, en and ja UI | Not implemented |
| R23 | Resolution/FPS/content/codec/color/length/failure matrix; PSNR/SSIM/LPIPS with ground truth and blind/temporal review otherwise | Unit and small synthetic media suite only |
| R24 | Controlled compatibility success target, no unreported sync errors, detectable crash states, repeatable benchmarks | Release goals unproven |
| R25 | Complete dependency/model/runtime license and redistribution audit; no known severe security/data-loss issues | Pinned SDK provenance recorded; full audit pending |
| R26 | Installer/portable package, signature plan, install/uninstall/first-run/model lifecycle tests, SHA-256, source/build/user/developer/security docs and support matrix | Not implemented |
| R27 | Separate app/model updates with integrity/version checking and rollback; release/maintenance process | Not implemented; remote publication deferred |

## Final completion gate

All seven Master Plan delivery groups remain unaccepted: complete desktop/CLI, real RIFE GPU interpolation, complete media/color, queue/recovery/models, preview/quality/encoding, tests/benchmarks/docs, and installable compliant continuously maintainable distribution. Passing the current foundation suite proves only the behaviors explicitly covered above. The goal remains active until every required capability and release gate has direct evidence.
