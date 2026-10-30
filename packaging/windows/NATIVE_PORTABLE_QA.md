# 原生 EXE 的隔离 GUI 验收

本方案使用全新专用本机包、永久便携标记和独立空数据目录。普通发行包仍默认使用 `%LOCALAPPDATA%\RifeLove`。打包/目录检查与真实 EXE 的 GUI 验收分别记录；打包通过不代表界面、GPU 推理或发布门槛已通过。

## 1. 先完成最终构建

在最终代码、构建与必要回归稳定后执行。原生 EXE 必须与选定的 x86asm FFmpeg SDK 对应；CMake 缓存的 `RIFELOVE_FFMPEG_ROOT`、`RIFELOVE_FFMPEG_TOOL` 和 `RIFELOVE_FFPROBE_TOOL` 都应指向 `work/codec-ffmpeg-build-x86asm/ffmpeg-install`。打包脚本会拒绝缓存、EXE 旁 DLL 与 SDK 身份不一致的混包。

以下 `$build` 应替换为已经完成最终构建/测试的目录。现有 `build/native-sdk-regression-x86asm` 可作为独立构建目录，但不能因为以前编译过就跳过本次最终构建。

## 2. 在新目录生成便携包

在仓库根目录执行；示例仅打包，不启动 EXE：

```powershell
$build = 'build/native-sdk-regression-x86asm'
$package = 'out/RifeLoveNative-local-portable-gui-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
pwsh -NoProfile -File packaging/windows/package-native-reuse-local.ps1 `
  -SourceDirectory out/RifeLoveNative-local-20261031-x86asm-sdkcheck `
  -BuildDirectory $build `
  -OutputDirectory $package `
  -PortableMode
if ($LASTEXITCODE -ne 0) { throw 'Portable package creation failed.' }

pwsh -NoProfile -File tools/validation/check_native_package_data_mode.ps1 `
  -PackageDirectory $package -ExpectedMode Portable -RequireEmptyData `
  -VerifyAllImmutableFiles -ExpectedExecutable "$build/RifeLoveNative.exe"
if ($LASTEXITCODE -ne 0) { throw 'Portable package contract check failed.' }
```

若复用源为旧 SDK 的 `out/RifeLoveNative-local-20261031-r11`，同时传入 `-RefreshFfmpegRuntime`；同源码锁、DLL 清单、依赖导入、编译器运行库和所有源文件校验仍必须通过。仅独立复制变化的 FFmpeg DLL；已核验相同的多 GiB 模型/推理库继续使用 NTFS 硬链接。它不会覆盖源包或已有目标。

显式便携包具有：

- `portable-mode.txt` 的精确内容为 `RifeLovePortable 1` 后跟 LF，UTF-8 无 BOM，永久保留并纳入 SHA-256 清单。
- 全新、独立、非链接、初始为空的 `data`；不复制源包或 Windows 用户目录的队列、模型偏好和日志。
- defaults 与 manifest 的 `portableModeEnabled=true`、有效 `dataDirectory=data`；manifest 同时记录永久标记存在。
- README 如实标识便携模式和现有运行时的回退行为。

模型及推理运行库的硬链接是不可变资源共享，不包括 `data` 或 EXE。不要就地编辑共享 DLL/模型。便携标记不应在启动前后临时切换，也不通过删除标记把此 QA 包改造成普通包。

## 3. 实际 GUI 验收

由验收操作者启动新包的 `RifeLoveNative.exe`，保留原包、原用户队列及结果文件。只使用新建的测试素材/输出目录。

1. 打开界面详情，记录“用户数据目录”为新包的绝对 `data` 路径，并出现便携模式说明；确认新队列为空。现有程序遇到无效标记或不可写的 `data` 会回退到 Windows 用户数据目录；若出现回退，停止此次 GUI 工作流，不把它记成隔离验收通过。打包检查不能代替该运行时路径核对。
2. 验证按钮、下拉框、模型选择、窗口缩放及比较窗口；记录实际最终 EXE 的 SHA-256、窗口尺寸、缩放比例和截图。
3. 通过 GUI 导入短视频并提交一个真实任务，确认队列状态、实际后端/编码器、处理进度、完成状态和结果路径。分别按需要测试 RIFE CUDA/TensorRT、软件/NVENC；GPU 测试顺序运行并与其他实验协调显存。
4. 使用独立 FFmpeg 解码实际 GUI 结果，核对像素、时长、帧数、PTS 和音轨；终端驱动以前通过的结果不能替代该结果。独立解码工具不随本机包交付。
5. 正常退出并重新打开同一专用包，确认新测试队列、模型偏好和历史仍来自其 `data`。只报告实际验证过的暂停/取消/恢复能力，不把任务重启识别宣称为完整分段续跑。
6. 再运行只读契约检查，省略 `-RequireEmptyData`（此时允许新 QA 状态），保留截图、输出、队列与校验记录。无需清理或删除便携标记、包、用户数据。

## 4. 正反例及可复现检查

保留式单元夹具（只创建新的小文件，保留全部夹具；不启动 EXE，不删除标记）：

```powershell
pwsh -NoProfile -File tests/packaging/native_portable_package_test.ps1
```

它覆盖普通默认模式、精确永久标记、初始空数据、两个请求模式下拒绝覆盖现有标记、两个模式下拒绝初始化已有数据，及拒绝后原内容不变。

反例可从一个便携包复用生成**另一个新普通包**，不传 `-PortableMode`，然后执行：

```powershell
pwsh -NoProfile -File tools/validation/check_native_package_data_mode.ps1 `
  -PackageDirectory out/YOUR-NEW-NORMAL-PACKAGE -ExpectedMode Normal `
  -RequireEmptyData -VerifyAllImmutableFiles
```

普通包必须没有便携标记、清单不包含该标记、metadata 指向 Windows 用户目录，且不会复制源便携 `data`。对它指定 `-ExpectedMode Portable` 必须拒绝。已有目标再次打包也必须在 staging 前拒绝；这些拒绝不改写旧包。

2026-10-31 已完成的打包契约夹具是 `out/RifeLoveNative-local-20261031-portable-packagingtest` 与 `out/RifeLoveNative-local-20261031-normal-from-portable-test`。便携源在初始空目录检查后新增了专用 `data/qa-do-not-copy.txt`，用来证实普通目标不继承数据；该夹具和永久标记继续保留。它们使用的是打包检查时的 EXE，不是最终代码 GUI 通过证明。

这些操作均在本机进行，不上传模型、视频、队列或日志。公开再分发许可证审核、支持矩阵、长视频与完整产品验收仍单独推进。
