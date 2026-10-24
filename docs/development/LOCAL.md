# Local development

The current workflow uses `C:\itzming\rifelove`, a local Git repository, local builds and local task data. It has no configured remote. Remote repository setup, hosted CI and publishing are deferred; the CI file in `templates/` is reference material only.

## Build and check

Run from PowerShell:

```powershell
C:\itzming\rifelove\tools\development\check.ps1
C:\itzming\rifelove\tools\development\check.ps1 -Preset windows-release
```

The script works from any current directory. It uses the existing dependency source cache and disables dependency downloads unless explicitly enabled. It invokes the CMake configure/build/test presets and writes:

- `build/<preset>/local-check.json`: result and exit code for configure, build and tests.
- `build/<preset>/test-report.xml`: JUnit test results.

Both are local generated files excluded from Git. SQLite, GoogleTest and application builds are kept under `build/`. Model weights and TensorRT caches must also remain outside tracked source files.

For a new offline build directory, source caches can be supplied using `FETCHCONTENT_SOURCE_DIR_SQLITE3` and `FETCHCONTENT_SOURCE_DIR_GOOGLETEST`. A machine without those sources must first receive copies of the pinned dependency sources or explicitly enable one-time dependency acquisition with `-AllowDependencyDownload`. The normal local check never opts into that download step.

Current presets use MinGW for the core and CLI. The optional native FFmpeg engine has a locally installed, pinned shared-library SDK; see [FFmpeg development](FFMPEG.md). Qt 6.12 and matching MinGW 15.1 MSVCRT now build the desktop locally in a separate directory. Production MSVC/vcpkg and installer validation remain pending. Python is optional for development integration/reference checks, and is not part of the user runtime.

## Task maintenance

The CLI accepts Windows Unicode arguments and emits UTF-8 JSON. Use an existing database to inspect tasks:

```powershell
.\build\windows-dev\rifelove-cli.exe tasks .\build\my-tasks.sqlite
```

Inspection opens the database read-only. It does not create a missing database or change task states. Once all workers have stopped, explicitly mark leftover running tasks as interrupted:

```powershell
.\build\windows-dev\rifelove-cli.exe tasks-recover .\build\my-tasks.sqlite
```

This operation identifies interrupted work; checkpoint resume is still unimplemented. Native decode, transcode and SDR8 RIFE processing are available through the [pipeline CLI](PIPELINE.md).
