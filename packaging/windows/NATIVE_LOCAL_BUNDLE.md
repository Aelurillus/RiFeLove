# RifeLove 原生 Windows 本机包

`package-native-local.ps1` 生成可双击启动的 Win32 原生本机目录。它默认使用 `build/windows-release/RifeLoveNative.exe`，可通过 `-BuildDirectory` 指定已构建的原生目录，不使用 Qt/QML，也不把 `rifelove-cli.exe` 或其他命令行工具放入交付目录。

默认输出是 `out/RifeLoveNative-local-nvenc`。脚本先在同一目录的随机 staging 路径中复制和校验文件，再执行短启动检查，最后以目录移动完成切换。目标目录存在时，脚本会拒绝替换正在运行的 `RifeLoveNative.exe`；切换成功前旧目录会保留在随机 previous 路径，失败时恢复旧目录。`data` 和旧目录中未随包的用户模型会复制到新的活动目录，`data` 不进入 `SHA256SUMS.txt`，因此刷新不会把用户视频、任务数据库或日志变成发布内容。

默认包包含以下本机资源：

- `RifeLoveNative.exe`，以及其实际链接的 FFmpeg、MinGW 和 MSVC 运行库。
- 当前已验证的 FFmpeg 共享库：`avcodec-62.dll`、`avformat-62.dll`、`avutil-60.dll`、`swresample-6.dll`、`swscale-9.dll`。
- ONNX Runtime GPU、CUDA 12、cuDNN 9 和 TensorRT 10.9 的 DLL，放在 `runtime/`；运行库目录内的 DLL 全部记录在 `dependency-imports.json`。
- `models/rife-4.25-fp32-sdr8-v3` 作为默认插帧模型。
- 干净包包含 RIFE、Real-ESRGAN、FastDVDnet 和 STDF-R3 各一份模型。FastDVDnet 与 STDF-R3 默认关闭。刷新已有包时，只有 `data/` 中的队列/检查点文件同时包含 `models/cache/rife-4.25-fp32-sdr8-v2`、RIFE ID/version 和匹配 SHA-256，才额外保留 `models/rife-4.25-fp32-sdr8-v2`；此目录保留原始 basename，并由 `models/legacy-models.json` 标记为隐藏，不参与默认模型选择。
- `models/realesrgan-x2plus-sdr8-v2`，供本机超分路径使用；当前原生窗口仍以 RIFE 插帧为主要工作流。
- `models/fastdvdnet-clipped-five-frame-sdr8-eval`，供本机 SDR8 源率五帧时序降噪路径使用；它不是压缩修复、去模糊或细节增强模型，当前仅作为本地评估包随包提供，默认关闭，必须由用户明确选择后才启用。
- `models/stdf-r3-y7-sdr8-local-eval`，供源率插帧前的七帧亮度压缩修复评估使用。仅接受明确标记的 BT.709 SDR、8-bit `yuv420p`、TV range。原整帧模式仍限 384×256；独立的「高清分块（实验；较慢）」开关允许每轴不超过 3840、总像素不超过 8,294,400 的输入，采用 256×128 core、64-pixel halo、全图右/下边界补齐至 8 的固定策略。横竖 UHD 及非 8 对齐合成输入已通过完整 CPU/CUDA 输出逐字节对照，证据见 [分块验证记录](../../docs/development/STDF_R3_TILING.md)。压缩修复和分块均默认关闭；分块有明显额外耗时，未知色彩、HDR、其他像素格式、真实素材主观画质与长视频仍未验收，不代表通用细节增强或 Topaz 级质量。
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

## 显式基础包与可选实验模型

两个脚本都支持 `-BaseModelsOnly`：只捆绑一个 RIFE 4.25 v3 插帧包和一个 Real-ESRGAN x2plus v2 超分包。FastDVDnet、STDF-R3 和重复的旧 RIFE v2 不再是基础包的前置条件；未选择的实验模型不会读取描述文件、图、许可证或来源/验证侧车，也不会因为源包校验清单仍列出已缺失的实验文件而阻止基础包生成。源清单路径仍须通过格式、重复项和目录边界检查。

```powershell
# 从现有已验证依赖生成新基础包，不复制源包 data 或恢复记录。
pwsh -NoProfile -File packaging/windows/package-native-reuse-local.ps1 `
  -SourceDirectory out/RifeLoveNative-local-20261031-r17 `
  -BuildDirectory build/native-sdk-regression-x86asm `
  -OutputDirectory out/RifeLoveNative-local-base `
  -BaseModelsOnly

# 也可完整打包。基础模式必须使用尚不存在的输出目录。
pwsh -NoProfile -File packaging/windows/package-native-local.ps1 `
  -BuildDirectory build/native-sdk-regression-x86asm `
  -OutputDirectory out/RifeLoveNative-local-base-full `
  -BaseModelsOnly
```

在基础模式上按需添加 `-IncludeTemporalRestoration`（FastDVDnet）、`-IncludeCompressionRestoration`（STDF-R3）和/或 `-IncludeDeblurRestoration`（NAFNet GoPro）。这些开关只选择随包文件，不启用处理功能；实验模型仍默认关闭。NAFNet 在完整四模型模式中也必须显式选择；仅传 `-DeblurModelDirectory` 不会让它入包。显式选择但缺失或校验失败的模型会报错。完整打包使用对应 `-...ModelDirectory` 或默认缓存；复用打包先使用源清单声明的模型，源包缺少时使用显式目录，STDF 与 NAFNet 的显式 Include 开关也可使用默认缓存。单独指定目录不会让基础模式自动选择该模型。

未传 `-BaseModelsOnly` 时保持兼容：完整打包仍默认包含原四模型；四模型源包复用仍保留四模型，旧三模型源包仍可按原逻辑复用。复用已有三模型包时，只有显式 STDF 目录或 `-IncludeCompressionRestoration` 才新增 STDF。NAFNet 不因源包恰好包含它而在新包中自动保留；需要 `-IncludeDeblurRestoration`。完整刷新现有 NAFNet 包若未带此开关会报错，避免把已有任务引用的模型无声移除。默认行为没有改成基础包。

基础包的 `config/defaults.json` 将未捆绑模型路径设为 `null`、模式设为 `not-bundled`；`PACKAGE-MANIFEST.json` 只列实际选中模型，并记录 `modelBundleMode`。模型目录、独立许可证、校验清单和 README 同步过滤，默认插帧选择重置为 RIFE v3。基础模式不迁移源包历史恢复记录、队列或用户添加模型；源文件不被修改或删除。完整脚本的基础模式只允许全新输出目录，避免把删减模型选择作用于已有用户数据；默认完整刷新仍沿用原来的 data 保留机制。

旧 v2 不再无条件读取：完整刷新先用固定历史图 SHA-256 检查旧 `data/` 中精确路径、ID、版本和 hash 的恢复引用，仅命中时才读取并校验旧包。干净目录无需存在 v2 缓存。此变化不会改写、迁移或删除开发队列、Windows 用户数据库或恢复记录；用户现有的同名数据仍保留原位置。

CPU-only 打包契约回归：`pwsh -NoProfile -File tests/packaging/native_model_reuse_test.ps1`。微型假 ONNX/侧车覆盖基础与四模型模式、独立实验选择、缺失/损坏排除模型、实际硬链接复制、空目标 data、源队列/恢复数据 hash 不变及按引用延迟验证 v2。若本机已有 NAFNet 缓存，额外核验真实图 SHA-256、身份、来源与许可证，并通过 NTFS 硬链接复用模型图，不另复制大权重。夹具会保留；该测试不启动产品、不下载权重，也不代表模型画质、GPU 或正式发行包已验收。

推理运行库发现顺序为 `-InferenceRuntimeDirectory`、环境变量 `RIFELOVE_TENSORRT_RUNTIME_DIR`、选定构建的同名 CMake 缓存。MinGW 运行库发现顺序为 `-MinGWBinDirectory`、环境变量 `RIFELOVE_MINGW_BIN`、由该构建缓存与生成记录核定的实际编译器所在目录；不通过 PATH 搜索 `g++`，不再假定用户名、Scoop 或 TensorRT 的安装位置。缺失依赖时脚本给出对应参数/环境变量提示，环境变量无效不会静默退回其他安装。

模型来源与包内目录是两套明确的路径：`-SuperResolutionModelDirectory` 默认读取本机缓存 `models/cache/realesrgan-x2plus-sdr8-candidate-v2`，经过描述文件、模型图、许可证和侧车校验后，统一打包为 `models/realesrgan-x2plus-sdr8-v2`；这不是遗漏模型或第二份权重。`-TemporalRestorationModelDirectory`、`-CompressionRestorationModelDirectory` 和 `-DeblurModelDirectory` 分别默认读取 FastDVDnet、STDF-R3 和 NAFNet 缓存，包内保留规范 basename。NAFNet 缓存目录为 `models/cache/nafnet-gopro-deblur-sdr8-fp32-eval`，描述中的模型 ID 为 `nafnet-zoo-gopro-sdr8-local`；包内路径使用目录 basename，不用模型 ID 代替。不得把缓存目录名直接写入包内 defaults。

NAFNet 是可选的本机评估权重，不是 RIFE 或超分的默认模型。选择后打包器核对固定图 SHA-256 `b99b39f0bdf29f15205aa50dd23b7a14b571b0c7ae9576ba2f9417fccad4f056`、去模糊适配器及 BT.709 SDR8 能力、独立 `LICENSE`、来源清单、转换记录和验证侧车。包内 `config/defaults.json` 的 `deblurRestorationDefaultEnabled` 仍为 `false`；`PACKAGE-MANIFEST.json` 记为 `experimental-optional-deblur` 并指向 `licenses/Model-nafnet-gopro-deblur-sdr8-fp32-eval-LICENSE` 与来源侧车，所有实际文件进入 `SHA256SUMS.txt`。原生模型中心从 `models/` 扫描 `model.json` 发现它。逐帧去模糊可能有闪烁或伪影，速度也会下降；与曝光动态模糊并用可能相互抵消。来源清单明确写明链接 ONNX 权重再分发授权尚未确认，因此这里只允许本机打包与验证，不构成公开分发许可确认。

`config/defaults.json` 包含 `compressionRestorationModelDirectory = "models/stdf-r3-y7-sdr8-local-eval"`、`compressionRestorationDefaultEnabled = false`、`compressionRestorationStrength = 0.75` 与 `compressionRestorationMode = "experimental-local-evaluation"`。强度只是显式启用时的设置，不会使模型自动启用。`PACKAGE-MANIFEST.json` 将 STDF 标记为 `experimental-optional-compression-repair`，保留许可证、版本、验证后端与图 SHA-256；模型目录、来源/验证侧车及复制到 `licenses/` 的许可文本全部进入 `SHA256SUMS.txt`。原生模型中心通过 `models/` 下的 `model.json` 和能力字段发现修复模型，不依赖 `default-model.txt` 把它选为插帧模型。

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

复用脚本支持四模型源包：优先校验源包 FastDVDnet / STDF-R3 的描述文件、清单身份、图、许可证及来源/验证侧车，再硬链接到新目录，不要求重复提供本机模型缓存。缺少 FastDVDnet 时沿用 `-TemporalRestorationModelDirectory` 的本机来源。旧三模型包未声明 STDF 时保持三模型，压缩修复关闭且模型路径为 `null`；只有显式传入 `-CompressionRestorationModelDirectory` 才校验并添加真实的本机 STDF 模型，绝不伪造清单项或指向不存在的模型。新增文件与许可证也使用 NTFS 硬链接并记录 SHA-256。已有目录却缺少清单项、重复清单项或身份不匹配会被拒绝；复用会重建实验模型的关闭默认值和正确角色。此流程不会复制整套多 GiB 数据，也不修改旧包或共享模型文件。小型四模型、三模型兼容/显式升级、篡改拒绝及硬链接回归：`pwsh -NoProfile -File tests/packaging/native_model_reuse_test.ps1`。测试只用微型夹具，不启动产品，也不构成 GPU 或画质验收。

Windows CTest 在检测到 PowerShell 7 (`pwsh`) 后注册 `Packaging.NativeModelReuse` 与 `Packaging.NativeDataMode`，标签为 `packaging;cpu-only`，可通过 `ctest --test-dir build/ci -L packaging --output-on-failure` 单独运行。现有 CPU-only GitHub CI 的 CTest 步骤会自动执行它们；不需要 FFmpeg、CUDA、TensorRT、私有模型缓存或已有 `out/`，也不下载模型、启动 EXE 或上传夹具。测试在本机 `out/` 生成少量明确标记的伪模型/数据契约夹具。真实源包校验另在本机显式运行，例如 `pwsh -NoProfile -File tests/packaging/native_model_reuse_test.ps1 -SourcePackageDirectory out/RifeLoveNative-local-20261031-r14`；这个可选参数不会写入 CTest 或 CI，不替代 GUI/GPU 验收。

推理运行库、TensorRT、CUDA/cuDNN、MSVC 和模型权重均需按各自许可证审查。脚本只生成本机离线目录，不访问阿里云 OSS，不上传视频、队列、日志或模型；未完成公开再分发审核的权重在清单中标成 `local-only-until-license-review`。NVIDIA 和 Microsoft 运行库的许可证/来源说明位于 `licenses/`。

FastDVDnet 当前是实验性五帧时序降噪包，不是通用压缩修复、去模糊或细节增强器。2026 年 10 月 29 日 GTX 1660 SUPER 的本地 CUDA 对照记录在 `out/temporal-quality-20261029-cuda/quality-report.json`：固定 sigma25 噪声样本 PSNR 从 20.20 dB 提升到 36.25 dB，但压缩样本 PSNR 从 34.16 dB 降到 33.19 dB，干净样本也被改变。因此清单会保留该模型及其校验信息，但 `config/defaults.json` 明确将 `temporalRestorationDefaultEnabled` 设为 `false`，不能把它宣传为默认最优或对任意视频安全。

本机验证记录（2026 年 10 月 29 日）：隔离验收包为 `out/RifeLoveNative-local-audit-20261029`，复用预览为 `out/RifeLoveNative-local-audit-reuse-20261029`。前者使用 `build/windows-release/RifeLoveNative.exe`，通过完整 TensorRT/CUDA/cuDNN/ONNX Runtime DLL 清单、三模型侧车 SHA-256、无 Qt/QML/CLI 扫描和包内启动检查；后者使用 `build/native-ui-review/RifeLoveNative.exe`，对不可变依赖建立 NTFS 硬链接，没有新增第二份运行库或模型数据。两个新包均明确 FastDVDnet 默认关闭。历史验收包 `out/RifeLoveNative-local-nvenc` 只含 RIFE 与 Real-ESRGAN，不作为本次三模型包的复用源。

2026 年 10 月 28 日的媒体验收仍来自 `out/native-package-smoke`：使用同一包内 EXE 所链接的 codec/runtime/model 资源、包外开发 CLI 触发本地引擎，处理 1280×720、6 帧、两条音轨素材：H.264 NVENC 输出 13 帧、48 FPS、视频时长 0.251 秒、容器时长 0.277 秒、FLAC 与 AAC 两条音轨；OpenH264 软件输出相同帧数、PTS 和音轨，独立视频/音频解码均通过。CLI 只用于验证包内引擎资源，不在包内交付；GUI 端到端处理仍待 UI 代理稳定构建和可操作窗口复验。

刷新保护也已验证：目标包运行时再次执行脚本会被拒绝；关闭后刷新成功，`data` 和模型目录保留。当前生成包的 `data` 为空，测试素材和结果位于仓库 `out/native-package-smoke`，不会进入本机包清单。NVENC 验收来自隔离 SDK `work/codec-ffmpeg-build/ffmpeg-install`，不是旧 Qt 包或旧 FFmpeg；该 SDK 关闭 x86asm，所以 CPU 解码和软件编码吞吐仍未完成资格确认。

原生 UI 代理需要使用以下目录约定来自动发现资源：`models/default-model.txt` 指向默认模型目录名；若发生经验证的迁移，`models/legacy-models.json` 的 `hiddenFromDefaultSelection` 列表用于隐藏保留检查点身份的 v2；`models/` 下每个模型包保留其原始 `model.json`；`runtime/` 直接包含 ONNX Runtime CUDA 所需 DLL。刷新时保留 `data/` 和用户自行添加的模型目录，不应把绝对 staging 路径写入配置或任务记录。旧版本曾生成 `rife-4.25-fp32-sdr8-v2-legacy` 别名；刷新脚本仅在旧包清单标记该目录为生成的 legacy 且 ONNX SHA-256 与 v2 完全一致时折叠它，避免误删同名用户模型。
