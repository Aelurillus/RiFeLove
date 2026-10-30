# 原生本机包媒体验收记录

日期：2026-10-28（Asia/Taipei）

验收包：`out/RifeLoveNative-local-nvenc`

素材：`out/native-package-smoke/nvenc-source-720p.mkv`。源文件为 1280×720、6 个 FFV1 视频帧，包含一条 FLAC 音轨和一条 AAC 音轨。处理使用本机 RIFE 4.25 v3、包内 `runtime/` 推理运行库和隔离 FFmpeg SDK；开发 CLI 只作为包外触发器，未复制到交付目录。

## 结果

| 路径 | 实际视频编码器 | 推理后端 | 视频帧 | PTS 范围 | 视频时长 | 容器时长 | 音轨 | 独立解码 |
| --- | --- | --- | ---: | --- | --- | --- | --- | --- |
| `out/native-package-smoke/package-nvenc-cli-720p.mkv` | `h264_nvenc` | `onnxruntime_cuda` | 13 | 0 至 250 ms，1/1000 time base | 0.251 s | 0.277 s | FLAC + AAC（2 条） | 视频、音频均 `ffmpeg -xerror` 通过 |
| `out/native-package-smoke/package-openh264-cli-720p.mkv` | `libopenh264` | `onnxruntime_cuda` | 13 | 0 至 250 ms，1/1000 time base | 0.251 s | 0.277 s | FLAC + AAC（2 条） | 视频、音频均 `ffmpeg -xerror` 通过 |

两个输出的 `ffprobe -show_frames` 均得到 13 帧，首帧 PTS 为 `0/1000`，末帧 PTS 为 `250/1000`；48 FPS 的量化序列中间帧包含 21、42、63、83、104、125、146、167、188、208、229 ms。引擎报告的输出帧数、首尾时间戳和时长误差均通过自身校验，`ffprobe` 和独立解码再次确认容器结果。

对应证据文件：

- `out/native-package-smoke/package-nvenc-cli-720p-report.json`
- `out/native-package-smoke/package-nvenc-cli-720p-ffprobe.json`
- `out/native-package-smoke/package-nvenc-cli-720p-video-frames.json`
- `out/native-package-smoke/package-openh264-cli-720p-report.json`
- `out/native-package-smoke/package-openh264-cli-720p-ffprobe.json`
- `out/native-package-smoke/package-openh264-cli-720p-video-frames.json`

## 验证边界

包内 `RifeLoveNative.exe` 已实际启动，显示默认模型和 CUDA 运行库就绪，并完成 GUI 本地源素材检查。当前窗口捕获环境尚未可靠触发 Win32 owner-draw 的“开始插帧”按钮，所以本记录没有把开发 CLI 结果写成 GUI 端到端结果；GUI 处理仍需在原生 UI 代理的稳定构建释放后复验。

NVENC 结果来自 `work/codec-ffmpeg-build/ffmpeg-install`，包含 H.264 NVENC、OpenH264、Kvazaar 和 libaom。该 SDK 配置为 `--disable-x86asm`，因此 CPU 解码和软件编码性能尚未完成资格确认。FFmpeg 的 `ffmpeg.exe` 和 `ffprobe.exe` 只用于验收，未进入包内。

## 2026-10-29 打包审计

隔离包 `out/RifeLoveNative-local-audit-20261029` 已通过打包脚本校验，包含且仅包含以下三个规范模型目录：RIFE 4.25 v3、Real-ESRGAN x2plus v2、FastDVDnet 五帧 SDR8 评估包。运行库目录包含 22 个 DLL，覆盖 ONNX Runtime core/shared/CUDA/TensorRT provider、CUDA 12、cuDNN 9 和 TensorRT 10.9；`PACKAGE-MANIFEST.json` 记录 requiredFiles、TensorRT 版本、来源 SHA-256、模型 SHA-256、许可证标识和已验证后端。

复用包 `out/RifeLoveNative-local-audit-reuse-20261029` 使用 `build/native-ui-review/RifeLoveNative.exe`，其不可变 runtime/model 文件通过 NTFS hard link 复用隔离包，实际未新增第二份大体积运行库或权重。复用脚本会拒绝旧的两模型源包或已知重复模型 basename。复用包启动检查保持运行 3 秒后正常退出；EXE SHA-256 为 `877bbb9c57b7c288c08922822618ddd881a543692eba81894f380bbf0dd809fe`。隔离包 EXE SHA-256 为 `1e618f617002cf6b0be8cf0868ab8eaf872de270f7df8db69bf7d00aff28b04b`。

模型权重仍按本机使用限制处理：清单中的 `redistribution` 为 `local-only-until-license-review`；Real-ESRGAN 权重的再分发授权尚未确认。FastDVDnet 的质量边界见下节，默认关闭。

## 2026-10-31 x86asm 候选 SDK 回归（未覆盖旧包）

当前使用独立 Release 目录 `build/native-sdk-regression-x86asm`，且缓存中的 FFmpeg SDK、FFmpeg 工具和 ffprobe 工具均指向 `work/codec-ffmpeg-build-x86asm/ffmpeg-install`。未替换既有 SDK、正式构建或用户本机包。

- 53 项聚焦媒体、色彩、队列与打包回归通过。随后 153 项扩展 CPU 回归为 151 通过、2 失败：原生 UI 缩放列宽、旧检查点身份恢复；失败需交各自模块修复，不作为媒体像素验证通过的替代品。
- OpenH264 H.264、Kvazaar HEVC、libaom AV1 8/10-bit 的真实 480×360、12 帧编码，均保留两条 AAC 音轨，独立 FFmpeg 视频与音频解码通过；像素比较 41.4–43.9 dB。
- 产品 `VideoMuxWriter` 的 7 组已知像素验证通过，覆盖 yuv420p 同格式不缩放/放大/4K→1440p 缩小、NV12 不缩放及缩小、planar 10-bit 与 P010 的 4K→1440p 缩小。独立解码后的色块内部误差全部为 0；只有预期滤波边界被排除。这验证写出器缩放环节，不声称运行真实超分模型。
- 旧 r11 包与新 SDK 的硬链接复用实测被 DLL 哈希保护拒绝，未生成目标包。配置身份、DLL 哈希与源码锁身份分别记录，不能再用共同源码锁将旧 SDK 冒充新配置。

GPU 时段释放后，CUDA/TensorRT FP32 六组参考、各 36 项契约验证及 6 项 GPU 内存/生命周期测试通过（8 个 CTest 项）；H.264 NVENC、HEVC NVENC 8/10-bit 自然素材短片通过独立解码；产品写出器 NV12/H.264 和 P010/HEVC 的 3840×2160→2560×1440 通过已知像素比较，前者色块内部零误差，后者最大色度误差为 3/2 个 10-bit 码值。

3 条真实 RIFE 应用服务链路（CUDA＋OpenH264、TensorRT＋NVENC/NV12、CUDA＋NVENC/P010）均产生 24 帧、48 FPS、保持 0.5 秒源视频时长，源视频实际 CUDA 硬件解码，无报告的推理后端/编码器回退。独立 FFmpeg 解码全部帧，PTS 对目标网格最大误差 0.5 毫秒，符合 Matroska 1 毫秒时间基；两条 AAC 音轨各 25 包的 payload SHA-256 与 PTS 均逐包保持。10-bit 素材的 42 毫秒非零视频起点得到保留。CUDA 有 cuDNN 卷积算法 fallback 警告，不据此宣称普遍处理速度。

本地证据：`out/native-sdk-regression-20261031/` 下 `cpu-codecs.json`、`writer-cpu.json`、`gpu-codecs.json`、`writer-gpu.json`、`real-rife-independent-validation.json`，以及独立构建的 CPU/GPU JUnit 报告。新本机候选 `out/RifeLoveNative-local-20261031-x86asm-sdkcheck` 已生成，仅独立复制 7 个不同 FFmpeg DLL（33,423,872 字节）及新 EXE/少量元数据，大型模型与推理库保持经校验的硬链接，旧 r11 未改写。此具体 EXE 的 GUI 启动验收仍待完成；这些短片不能推出所有 GPU/素材、长视频、真实超分模型或完整发布已验收。运行库和模型仍受既有本机使用、许可审核边界约束。

## FastDVDnet 本机实验性限制

`models/fastdvdnet-clipped-five-frame-sdr8-eval` 随隔离本机包提供，但默认关闭。它只表示已经完成的 SDR8 五帧时序降噪实验，不代表通用压缩修复、去模糊、超分辨率或细节增强能力。

2026-10-29 在 NVIDIA GeForce GTX 1660 SUPER 上的 CUDA 对照见 `out/temporal-quality-20261029-cuda/quality-report.json`。固定 sigma25 噪声样本 PSNR 为 20.20 → 36.25 dB；压缩样本为 34.16 → 33.19 dB；clean 样本也产生改变（输出 PSNR 38.85 dB，边缘误差 4.59 levels）。这组小型合成素材不能推出用户视频质量结论，所以打包配置将其标记为 `experimental-optional-temporal-denoise` 并保持 `temporalRestorationDefaultEnabled=false`。
