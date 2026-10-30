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
- `config/defaults.json`、`portable.flag`、许可证文本、来源/验证侧车和 SHA-256 清单。

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

脚本会检查：原生 EXE 不导入 Qt/QML；FFmpeg 和推理运行库 DLL 的导入；完整的 ONNX Runtime CUDA、TensorRT 10、CUDA 12 和 cuDNN 9 必需 DLL 清单；MinGW/MSVC 运行时；模型图和侧车 SHA-256；模型许可文件；包内不存在 CLI、Qt、链接或 junction。`-SkipStartupSmoke` 只适用于没有图形桌面的构建环境；正常本机打包应保留启动检查。

本包使用 `work/codec-ffmpeg-build/ffmpeg-install` 隔离 FFmpeg SDK，包含 `avfilter-11.dll`、`avdevice-62.dll`、`libopenh264-8.dll`、`libkvazaar.dll` 和 `libaom.dll` 等直接或间接依赖；不复制 `ffmpeg.exe`、`ffprobe.exe` 或 CLI。该 SDK 已通过真实 H.264 NVENC、OpenH264 软件编码及直接 DLL RIFE 回归，自动编码优先 H.264 NVENC 并保留软件回退。构建关闭 x86asm，因此 CPU 解码/软件编码吞吐仍需单独资格确认；`PACKAGE-MANIFEST.json` 会记录这一限制。

推理运行库、TensorRT、CUDA/cuDNN、MSVC 和模型权重均需按各自许可证审查。脚本只生成本机离线目录，不访问阿里云 OSS，不上传视频、队列、日志或模型；未完成公开再分发审核的权重在清单中标成 `local-only-until-license-review`。NVIDIA 和 Microsoft 运行库的许可证/来源说明位于 `licenses/`。

FastDVDnet 当前是实验性五帧时序降噪包，不是通用压缩修复、去模糊或细节增强器。2026 年 10 月 29 日 GTX 1660 SUPER 的本地 CUDA 对照记录在 `out/temporal-quality-20261029-cuda/quality-report.json`：固定 sigma25 噪声样本 PSNR 从 20.20 dB 提升到 36.25 dB，但压缩样本 PSNR 从 34.16 dB 降到 33.19 dB，干净样本也被改变。因此清单会保留该模型及其校验信息，但 `config/defaults.json` 明确将 `temporalRestorationDefaultEnabled` 设为 `false`，不能把它宣传为默认最优或对任意视频安全。

本机验证记录（2026 年 10 月 29 日）：隔离验收包为 `out/RifeLoveNative-local-audit-20261029`，复用预览为 `out/RifeLoveNative-local-audit-reuse-20261029`。前者使用 `build/windows-release/RifeLoveNative.exe`，通过完整 TensorRT/CUDA/cuDNN/ONNX Runtime DLL 清单、三模型侧车 SHA-256、无 Qt/QML/CLI 扫描和包内启动检查；后者使用 `build/native-ui-review/RifeLoveNative.exe`，对不可变依赖建立 NTFS 硬链接，没有新增第二份运行库或模型数据。两个新包均明确 FastDVDnet 默认关闭。历史验收包 `out/RifeLoveNative-local-nvenc` 只含 RIFE 与 Real-ESRGAN，不作为本次三模型包的复用源。

2026 年 10 月 28 日的媒体验收仍来自 `out/native-package-smoke`：使用同一包内 EXE 所链接的 codec/runtime/model 资源、包外开发 CLI 触发本地引擎，处理 1280×720、6 帧、两条音轨素材：H.264 NVENC 输出 13 帧、48 FPS、视频时长 0.251 秒、容器时长 0.277 秒、FLAC 与 AAC 两条音轨；OpenH264 软件输出相同帧数、PTS 和音轨，独立视频/音频解码均通过。CLI 只用于验证包内引擎资源，不在包内交付；GUI 端到端处理仍待 UI 代理稳定构建和可操作窗口复验。

刷新保护也已验证：目标包运行时再次执行脚本会被拒绝；关闭后刷新成功，`data` 和模型目录保留。当前生成包的 `data` 为空，测试素材和结果位于仓库 `out/native-package-smoke`，不会进入本机包清单。NVENC 验收来自隔离 SDK `work/codec-ffmpeg-build/ffmpeg-install`，不是旧 Qt 包或旧 FFmpeg；该 SDK 关闭 x86asm，所以 CPU 解码和软件编码吞吐仍未完成资格确认。

原生 UI 代理需要使用以下目录约定来自动发现资源：`models/default-model.txt` 指向默认模型目录名；若发生经验证的迁移，`models/legacy-models.json` 的 `hiddenFromDefaultSelection` 列表用于隐藏保留检查点身份的 v2；`models/` 下每个模型包保留其原始 `model.json`；`runtime/` 直接包含 ONNX Runtime CUDA 所需 DLL。刷新时保留 `data/` 和用户自行添加的模型目录，不应把绝对 staging 路径写入配置或任务记录。旧版本曾生成 `rife-4.25-fp32-sdr8-v2-legacy` 别名；刷新脚本仅在旧包清单标记该目录为生成的 legacy 且 ONNX SHA-256 与 v2 完全一致时折叠它，避免误删同名用户模型。
