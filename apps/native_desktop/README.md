# RifeLove native Windows desktop

This is the primary Windows application. It uses Win32 controls for native
keyboard and screen-reader semantics, Direct2D for the visual surface, and
DirectWrite for high-DPI Chinese text. It has no Qt or QML dependency.

## Current workflow

- Source inspection probes FFmpeg streams, CUDA and the selected output encoder
  before a job can start.
- Processing uses the application services and persistent SQLite task queue.
  File and folder batches, progress, pause, stop, cancellation, startup
  reconciliation and safe checkpoint segments are connected to the desktop.
- Task History shows local jobs and exposes state-appropriate recovery actions:
  retry failed jobs, continue interrupted jobs only after checkpoint
  verification, or restart an interrupted job to a separate output path. Queue
  execution and checkpoint rules remain owned by `TaskQueue`.
- Model Center verifies and registers local packs, installs local ZIP packages,
  and downloads packages from user-supplied HTTPS mirrors. Network downloads
  have progress, cancellation, byte-range resume and archive SHA-256 checks;
  mirrors may provide ZIP and manifest URLs separately when the host does not
  support adjacent filenames. Both local and downloaded packages continue
  through per-file hashes, schema-v2 validation and atomic placement in the
  managed user model directory.
  The current UI does not ship a trusted catalog or publisher-signature root;
  a sidecar hash does not authenticate its publisher.
- Comparison opens local source and output frames for timestamp-based
  inspection. It can import completed quality reports, align candidates to
  shared exact review timestamps, and randomize supported multi-candidate
  blind reviews. Reviewers can record per-point naturalness and artifact
  categories after the media hashes match, then export anonymized JSON before
  explicitly revealing candidate identities. The revealed export includes
  candidate names, output hashes, source order and shuffle seed without local
  paths. Multi-candidate blind sessions also save order, ratings, reviewer code
  and selected position in a report-specific file encrypted for the current
  Windows user; importing that report offers to resume or start a new session.
  The encrypted resume file omits local paths and candidate names. Other review
  modes remain in memory until exported. Model selection, output rate, container
  and encoder settings are part of the native processing workflow.

The current Windows worktree compiled and linked the native target with
`cmake --build build/windows-dev --target rifelove-native-desktop --parallel 4`.
Packaged qualification, real report import, review-session restart, DPI and
interactive playback qualification are still pending.

## Remaining product work

- A trusted model catalog, signed publisher metadata, published model mirrors
  and engine-cache controls remain open. The current downloader accepts
  user-supplied HTTPS URLs and checks each ZIP against its adjacent manifest;
  this protects transfer integrity, not publisher identity.
- Automatic denoise, deblur and compression restoration remain optional,
  experimental and default-off. Representative real-footage quality, temporal
  artifact review and broad GPU/driver coverage remain open.
- Comparison ratings support one reviewer at a time, optional 1–5 confidence,
  per-point free notes and an optional unverified pseudonym code. Multi-candidate
  blind sessions resume from a Windows DPAPI-encrypted local state; exported
  JSON is still required to share or aggregate ratings. Paired sequential
  decoders now queue output frames against the latest source
  frame at or before each output presentation timestamp; runtime frame-rate
  qualification and multi-reviewer temporal blind review remain open. During
  playback, the status line reports source/result decode rates, synchronized
  pair throughput, UI frame updates and preview frames skipped to catch up.
- Custom Direct2D cards still need a dedicated UI Automation provider beyond
  the semantics exposed by the native child controls.
- Clean-machine packaging, installer and uninstall behavior, redistribution
  review, signing and release qualification remain open.

Build with `RIFELOVE_BUILD_NATIVE_DESKTOP=ON` on Windows. The target is
`rifelove-native-desktop` and its output name is `RifeLoveNative.exe`. A full
processing workflow is enabled when `rifelove_application` is available, which
requires the media and ONNX CUDA build options.
