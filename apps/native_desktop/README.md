# RifeLove native Windows desktop

This directory contains the native Windows entry point for the primary desktop application. It uses Win32 controls for keyboard and screen-reader semantics, Direct2D for the visual surface, and DirectWrite for high-DPI Chinese text. It has no Qt or QML dependency.

The window is deliberately wired to the existing application service instead of emulating processing:

- source selection calls `ProcessingService::inspect`, which probes FFmpeg streams, CUDA and encoder contexts;
- the start button builds a real `ProcessingJob` and calls `ProcessingService::submit`;
- progress, completion, cancellation and failure are marshalled from the worker thread through a private window message;
- closing the window requests cancellation and the service destructor joins the worker before process exit;
- local model and ONNX Runtime directories are checked before the start button is enabled.

The following capabilities remain migration work and are shown as explicit gaps in the UI until they are connected:

- task queue history and SQLite queue controls;
- model catalog, version switching and engine-cache management;
- comparison preview and frame stepping;
- batch folder import, pause/resume and checkpoint recovery;
- accessibility metadata beyond the native control semantics (custom cards still need a dedicated UI Automation provider);
- packaging and installer integration for the native executable and its reviewed runtime/model bundles.

Build with `RIFELOVE_BUILD_NATIVE_DESKTOP=ON` on Windows. The target is `rifelove-native-desktop` and its output name is `RifeLoveNative.exe`. A full processing workflow is enabled only when `rifelove_application` is available, which requires the existing media and ONNX CUDA build options.
