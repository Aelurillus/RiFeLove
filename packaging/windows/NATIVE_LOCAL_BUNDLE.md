# RifeLove 原生 Windows 本机包

`package-native-local.ps1` 生成可双击启动的 Win32 原生本机目录。它只使用 `build/windows-release/RifeLoveNative.exe`，不使用 Qt/QML，也不把 `rifelove-cli.exe` 或其他命令行工具放入交付目录。

默认输出是 `out/RifeLoveNative-local-nvenc`。脚本先在同一目录的随机 staging 路径中复制和校验文件，再执行短启动检查，最后以目录移动完成切换。目标目录存在时，脚本会拒绝替换正在运行的 `RifeLoveNative.exe`；切换成功前旧目录会保留在随机 previous 路径，失败时恢复旧目录。`data` 和旧目录中未随包的用户模型会复制到新的活动目录，`data` 不进入 `SHA256SUMS.txt`，因此刷新不会把用户视频、任务数据库或日志变成发布内容。

默认包包含以下本机资源：

- `RifeLoveNative.exe`，以及其实际链接的 FFmpeg、MinGW 和 MSVC 运行库。
- 当前已验证的 FFmpeg 共享库：`avcodec-62.dll`、`avformat-62.dll`、`avutil-60.dll`、`swresample-6.dll`、`swscale-9.dll`。
- ONNX Runtime GPU、CUDA 12、cuDNN 9 和 TensorRT 10.9 的 DLL，放在 `runtime/`；运行库目录内的 DLL 全部记录在 `dependency-imports.json`。
- `models/rife-4.25-fp32-sdr8-v3` 作为默认插帧模型。
- 干净包包含一份 `models/rife-4.25-fp32-sdr8-v3`、一份 `models/realesrgan-x2plus-sdr8-v2` 和一份默认关闭的 `models/fastdvdnet-clipped-five-frame-sdr8-eval`。刷新已有包时，只有 `data/` 中的队列/检查点文件同时包含 `models/cache/rife-4.25-fp32-sdr8-v2`、RIFE ID/version 和匹配 SHA-256，才额外保留 `models/rife-4.25-fp32-sdr8-v2`；此目录保留原始 basename，并由 `models/legacy-models.json` 标记为隐藏，不参与默认模型选择。
- `models/realesrgan-x2plus-sdr8-v2`，供本机超分路径使用；当前原生窗口仍以 RIFE 插帧为主要工作流。
- `models/fastdvdnet-clipped-five-frame-sdr8-eval`，供本机 SDR8 源率五帧时序降噪路径使用；它不是压缩修复、去模糊或细节增强模型，当前仅作为本地评估包随包提供，默认关闭，必须由用户明确选择后才启用。
- `config/defaults.json`、许可证文本、来源/验证侧车和 SHA-256 清单。普通包使用 Windows 用户数据目录；复用打包脚本可通过显式 `-PortableMode` 在全新目录中生成带永久校验标记的便携验收包，见 [隔离 GUI 验收](NATIVE_PORTABLE_QA.md)。

在仓库根目录执行：

```powershell
pwsh -File packaging/windows/package-native-local.ps1
```

可用参数覆盖本机路径，例如：

```powershell
pwsh -File packaging/windows/package-native-local.ps1 `
  -BuildDirectory build/windows-release `
  -InferenceRuntimeDirectory C:/path/to/onnxruntime-win-x64-gpu-1.22.0/lib `
  -OutputDirectory out/RifeLoveNative-local-nvenc
```

推理运行库发现顺序为 `-InferenceRuntimeDirectory`、环境变量 `RIFELOVE_TENSORRT_RUNTIME_DIR`、选定构建的同名 CMake 缓存。MinGW 运行库发现顺序为 `-MinGWBinDirectory`、环境变量 `RIFELOVE_MINGW_BIN`、由该构建缓存与生成记录核定的实际编译器所在目录；不通过 PATH 搜索 `g++`，不再假定用户名、Scoop 或 TensorRT 的安装位置。缺失依赖时脚本给出对应参数/环境变量提示，环境变量无效不会静默退回其他安装。

显式路径/环境变量不会绕过身份检查：当前 GNU MinGW x64 编译器版本必须与生成的 `CMakeCXXCompiler.cmake` 一致。默认 `windows-release` 预设重复配置时可能把缓存重新写成 `g++`；此时仅接受所选构建生成记录中的绝对 `g++.exe`，并核对缓存名称与实际文件名，不通过当前 PATH 另找编译器。绝对缓存路径也必须与生成记录一致；缺失/重复记录、相对路径、非 GNU/MinGW 身份、`shims` 路径、`.shim` 侧车、脚本包装器和额外编译器参数均被拒绝。编译器自报的三份 MinGW DLL、所选目录、EXE 构建目录及 FFmpeg SDK 必须逐字节匹配。显式推理运行库目录的 DLL 清单、每个 DLL 和版本侧车须与构建缓存所指运行库一致；不同 SDK 应先重新配置与构建，不能仅改打包参数。缓存路径是当前参照，不等于历史依赖内容锁；应用/模型与硬件资格确认仍独立进行。

需要先检查依赖而不复制多 GiB 数据或启动程序时：

```powershell
pwsh -NoProfile -File packaging/windows/package-native-local.ps1 `
  -BuildDirectory build/native-sdk-regression-x86asm -ValidateDependenciesOnly
```

该选项在依赖检查后退出，不检查完整模型/许可证、不建包、不运行启动检查。可选择设置 `$env:RIFELOVE_TENSORRT_RUNTIME_DIR = 'C:/SDKs/reviewed-onnx-tensorrt/lib'` 和 `$env:RIFELOVE_MINGW_BIN = 'C:/toolchains/mingw/bin'`；无需设置时优先使用所选构建的缓存。小型保留式回归测试：`pwsh -NoProfile -File tests/packaging/native_build_dependencies_test.ps1 -RealBuildDirectory build/native-sdk-regression-x86asm`。

加上 `-ValidateDefaultReleasePreset` 可在全新的 `out/` 测试目录连续两次配置实际 `windows-release` 预设，覆盖裸缓存名与生成身份一致的正例；它不配置或构建 `build/windows-release`，不编译/启动产品。测试核验配置阶段已生成的三份 MinGW DLL，缺失时才硬链接匹配文件，不覆盖已存在文件。离线依赖源码默认取 `build/windows-dev/_deps`，可用 `-DependencySourceDirectory` 覆盖；所有测试夹具和日志保留，不清理旧目录。SDK 根引用来自 `-RealBuildDirectory`，不会下载依赖。完整 EXE 与包内 GUI 验收仍须独立进行。

脚本会检查：原生 EXE 不导入 Qt/QML；FFmpeg 和推理运行库 DLL 的导入；完整的 ONNX Runtime CUDA、TensorRT 10、CUDA 12 和 cuDNN 9 必需 DLL 清单；MinGW/MSVC 运行时；模型图和侧车 SHA-256；模型许可文件；包内不存在 CLI、Qt、链接或 junction。`-SkipStartupSmoke` 只适用于没有图形桌面的构建环境；正常本机打包应保留启动检查。

打包脚本默认选择 `work/codec-ffmpeg-build-x86asm/ffmpeg-install` 隔离 FFmpeg SDK，包含 `avfilter-11.dll`、`avdevice-62.dll`、`libopenh264-8.dll`、`libkvazaar.dll` 和 `libaom.dll` 等直接或间接依赖；不复制 `ffmpeg.exe`、`ffprobe.exe` 或开发验证工具。原生构建必须显式使用同一 SDK，且 CMake 缓存中的 `RIFELOVE_FFMPEG_TOOL` / `RIFELOVE_FFPROBE_TOOL` 也必须指向它。已有基线构建不会因打包参数而自动升级，SDK 不一致会在写包前被拒绝。

`PACKAGE-MANIFEST.json` 记录实际 FFmpeg 工具报告的 x86asm 开关、配置 SHA-256、工具 SHA-256、每个运行库 DLL 的 SHA-256，以及输入源码锁文件身份；源码锁文件本身不能区分同一版本的不同编译配置。完整打包和硬链接复用都会逐文件比较所选 SDK 与 EXE 旁运行库；复用还会核对旧包 DLL，因此不会把基线 DLL 重新标记成 x86asm。启用 x86asm 不等于任意素材或 CPU 编码性能已完成资格确认。

针对同一已审核源码锁、相同 DLL 清单/导入依赖的配置升级，可以显式使用 `-RefreshFfmpegRuntime`：先校验旧包所有不可变文件及新构建/SDK 身份，再仅独立复制字节不同的 FFmpeg DLL；相同 codec DLL、模型、推理运行库保持 NTFS 硬链接。源码版本、依赖导入或 MinGW 运行库发生变化时仍拒绝复用，要求完整打包。新目录不得已存在，旧目录及共享文件从不就地改写。示例：

```powershell
pwsh -File packaging/windows/package-native-reuse-local.ps1 `
  -SourceDirectory out/RifeLoveNative-local-20261031-r11 `
  -BuildDirectory build/native-sdk-regression-x86asm `
  -OutputDirectory out/RifeLoveNative-local-20261031-x86asm-sdkcheck `
  -RefreshFfmpegRuntime
```

2026-10-31 的该候选包已生成；7 个 FFmpeg DLL 独立复制 33,423,872 字节（约 31.9 MiB），另外复制新 EXE 和少量配置/清单，不复制整套多 GiB 推理库。CUDA/cuDNN 与 RIFE 模型的 NTFS 文件身份已核对，确实与 r11 共享；`swscale-9.dll` 则为独立新文件。候选包的 GUI 启动验收尚未完成，不能把开发驱动的补帧成功表述为该 EXE 的 GUI 端到端验收。

`package-native-reuse-local.ps1 -PortableMode` 只对新输出目录生效：生成永久 `portable-mode.txt`，内容为 UTF-8 无 BOM 的 `RifeLovePortable 1` 和换行，并纳入 `SHA256SUMS.txt`；同时创建独立空 `data`、更新 defaults/manifest/README。不传此参数时不会从源便携包继承标记，也不会复制源 `data`。打包检查只证明目录和清单契约；运行时若便携目录不可写，现有应用会回退到 Windows 用户数据目录，实际 GUI 验收必须从详情核对真实路径。不会用启动前后临时添加/移除标记的方式切换模式。

推理运行库、TensorRT、CUDA/cuDNN、MSVC 和模型权重均需按各自许可证审查。脚本只生成本机离线目录，不访问阿里云 OSS，不上传视频、队列、日志或模型；未完成公开再分发审核的权重在清单中标成 `local-only-until-license-review`。NVIDIA 和 Microsoft 运行库的许可证/来源说明位于 `licenses/`。

FastDVDnet 当前是实验性五帧时序降噪包，不是通用压缩修复、去模糊或细节增强器。2026 年 10 月 29 日 GTX 1660 SUPER 的本地 CUDA 对照记录在 `out/temporal-quality-20261029-cuda/quality-report.json`：固定 sigma25 噪声样本 PSNR 从 20.20 dB 提升到 36.25 dB，但压缩样本 PSNR 从 34.16 dB 降到 33.19 dB，干净样本也被改变。因此清单会保留该模型及其校验信息，但 `config/defaults.json` 明确将 `temporalRestorationDefaultEnabled` 设为 `false`，不能把它宣传为默认最优或对任意视频安全。

本机验证记录（2026 年 10 月 29 日）：隔离验收包为 `out/RifeLoveNative-local-audit-20261029`，复用预览为 `out/RifeLoveNative-local-audit-reuse-20261029`。前者使用 `build/windows-release/RifeLoveNative.exe`，通过完整 TensorRT/CUDA/cuDNN/ONNX Runtime DLL 清单、三模型侧车 SHA-256、无 Qt/QML/CLI 扫描和包内启动检查；后者使用 `build/native-ui-review/RifeLoveNative.exe`，对不可变依赖建立 NTFS 硬链接，没有新增第二份运行库或模型数据。两个新包均明确 FastDVDnet 默认关闭。历史验收包 `out/RifeLoveNative-local-nvenc` 只含 RIFE 与 Real-ESRGAN，不作为本次三模型包的复用源。

2026 年 10 月 28 日的媒体验收仍来自 `out/native-package-smoke`：使用同一包内 EXE 所链接的 codec/runtime/model 资源、包外开发 CLI 触发本地引擎，处理 1280×720、6 帧、两条音轨素材：H.264 NVENC 输出 13 帧、48 FPS、视频时长 0.251 秒、容器时长 0.277 秒、FLAC 与 AAC 两条音轨；OpenH264 软件输出相同帧数、PTS 和音轨，独立视频/音频解码均通过。CLI 只用于验证包内引擎资源，不在包内交付；GUI 端到端处理仍待 UI 代理稳定构建和可操作窗口复验。

刷新保护也已验证：目标包运行时再次执行脚本会被拒绝；关闭后刷新成功，`data` 和模型目录保留。当前生成包的 `data` 为空，测试素材和结果位于仓库 `out/native-package-smoke`，不会进入本机包清单。NVENC 验收来自隔离 SDK `work/codec-ffmpeg-build/ffmpeg-install`，不是旧 Qt 包或旧 FFmpeg；该 SDK 关闭 x86asm，所以 CPU 解码和软件编码吞吐仍未完成资格确认。

原生 UI 代理需要使用以下目录约定来自动发现资源：`models/default-model.txt` 指向默认模型目录名；若发生经验证的迁移，`models/legacy-models.json` 的 `hiddenFromDefaultSelection` 列表用于隐藏保留检查点身份的 v2；`models/` 下每个模型包保留其原始 `model.json`；`runtime/` 直接包含 ONNX Runtime CUDA 所需 DLL。刷新时保留 `data/` 和用户自行添加的模型目录，不应把绝对 staging 路径写入配置或任务记录。旧版本曾生成 `rife-4.25-fp32-sdr8-v2-legacy` 别名；刷新脚本仅在旧包清单标记该目录为生成的 legacy 且 ONNX SHA-256 与 v2 完全一致时折叠它，避免误删同名用户模型。
