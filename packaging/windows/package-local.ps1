[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "../../build/windows-qt-release"),
    [string]$QtRoot = "C:/itzming/dependences/qt-6.12.0-local",
    [string]$FfmpegRoot = "C:/itzming/dependences/ffmpeg-8.1.3/ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-shared-8.1",
    [string]$OnnxRuntimeRoot = "C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0",
    [string]$ModelPackRoot = (Join-Path $PSScriptRoot "../../models/cache/rife-4.25-fp32-sdr8-v3"),
    [string[]]$AdditionalModelPackRoots = @(),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "../../out/RifeLove-local")
)

$ErrorActionPreference = "Stop"
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "out"))
$target = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $target.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must be inside the repository's ignored out directory."
}

$build = (Resolve-Path $BuildDirectory).Path
$qt = (Resolve-Path $QtRoot).Path
$ffmpeg = (Resolve-Path $FfmpegRoot).Path
$onnx = (Resolve-Path (Join-Path $OnnxRuntimeRoot "lib/onnxruntime.dll")).Path | Split-Path -Parent
$packRoots = @($ModelPackRoot) + @($AdditionalModelPackRoots)
$modelPacks = @(
    foreach ($packRoot in $packRoots) {
        $model = (Resolve-Path (Join-Path $packRoot "model.json")).Path | Split-Path -Parent
        $descriptor = Get-Content -LiteralPath (Join-Path $model "model.json") -Raw | ConvertFrom-Json
        if ($descriptor.id -notmatch '^[A-Za-z0-9._-]{1,128}$') { throw "Model pack id is unsafe for a package path: $($descriptor.id)" }
        [pscustomobject]@{root=$model;descriptor=$descriptor}
    }
)
if (-not $modelPacks.Count) { throw "At least one local model pack is required." }
$duplicateIds = $modelPacks | Group-Object { $_.descriptor.id } | Where-Object Count -gt 1
if ($duplicateIds) { throw "Model pack ids must be unique in a local bundle: $($duplicateIds.Name -join ', ')" }
$defaultModelPack = $modelPacks[0]
$modelNames = ($modelPacks | ForEach-Object { if ($_.descriptor.displayName) { [string]$_.descriptor.displayName } else { [string]$_.descriptor.id } }) -join ", "
$executable = Join-Path $build "RifeLove.exe"
$cliExecutable = Join-Path $build "rifelove-cli.exe"
$deployTool = Join-Path $qt "bin/windeployqt.exe"
foreach ($required in @($executable,$cliExecutable,$deployTool,(Join-Path $onnx "onnxruntime_providers_cuda.dll"))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required packaging input is missing: $required" }
}
foreach ($pack in $modelPacks) {
    if (-not (Test-Path -LiteralPath (Join-Path $pack.root "model.onnx") -PathType Leaf)) {
        throw "Required model graph is missing: $($pack.root)"
    }
}

if (Test-Path -LiteralPath $target) {
    $existingTarget = Get-Item -LiteralPath $target -Force
    if ($existingTarget.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing to replace a linked output path: $target" }
    $existingRuntime = Join-Path $target "runtime"
    if (Test-Path -LiteralPath $existingRuntime) {
        $runtimeItem = Get-Item -LiteralPath $existingRuntime -Force
        if ($runtimeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { Remove-Item -LiteralPath $existingRuntime -Force }
    }
    Remove-Item -LiteralPath $target -Recurse -Force
}
New-Item -ItemType Directory -Path $target | Out-Null
Copy-Item -LiteralPath $executable -Destination $target
Copy-Item -LiteralPath $cliExecutable -Destination $target
$savedPath = $env:PATH
try {
    $env:PATH = "$qt/bin;$ffmpeg/bin;$savedPath"
    & $deployTool --release --compiler-runtime --qmldir (Join-Path $repositoryRoot "ui") --dir $target (Join-Path $target "RifeLove.exe")
    if ($LASTEXITCODE -ne 0) { throw "Qt runtime deployment failed with exit code $LASTEXITCODE" }
} finally {
    $env:PATH = $savedPath
}

foreach ($name in @("avcodec-62.dll","avformat-62.dll","avutil-60.dll","swscale-9.dll","swresample-6.dll")) {
    $source = Join-Path $ffmpeg "bin/$name"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required FFmpeg runtime is missing: $source" }
    Copy-Item -LiteralPath $source -Destination $target
}

foreach ($pack in $modelPacks) {
    $modelTarget = Join-Path $target ("models/{0}" -f $pack.descriptor.id)
    New-Item -ItemType Directory -Path $modelTarget -Force | Out-Null
    foreach ($name in @("model.json","model.onnx","LICENSE.txt","source-manifest.json","validation.json")) {
        $source = Join-Path $pack.root $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required model package file is missing: $source" }
        Copy-Item -LiteralPath $source -Destination $modelTarget
    }
}
$modelsDirectory = Join-Path $target "models"
Set-Content -LiteralPath (Join-Path $modelsDirectory "default-model.txt") -Value ([string]$defaultModelPack.descriptor.id) -Encoding UTF8

# Keep NVIDIA CUDA/cuDNN under their local SDK installation; this device-local
# link avoids silently redistributing those separately licensed binaries.
$runtimeTarget = Join-Path $target "runtime"
New-Item -ItemType Junction -Path $runtimeTarget -Target $onnx | Out-Null
New-Item -ItemType File -Path (Join-Path $target "portable.flag") | Out-Null
$platformTarget = Join-Path $target "platforms"
New-Item -ItemType Directory -Path $platformTarget -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $qt "plugins/platforms/qoffscreen.dll") -Destination $platformTarget
$licenseTarget = Join-Path $target "licenses"
New-Item -ItemType Directory -Path $licenseTarget | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot "LICENSE") -Destination (Join-Path $licenseTarget "RifeLove-MIT.txt")
Copy-Item -LiteralPath (Join-Path $ffmpeg "LICENSE.txt") -Destination (Join-Path $licenseTarget "FFmpeg-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path (Split-Path $onnx -Parent) "LICENSE") -Destination (Join-Path $licenseTarget "ONNX-Runtime-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path (Split-Path $onnx -Parent) "ThirdPartyNotices.txt") -Destination (Join-Path $licenseTarget "ONNX-Runtime-ThirdPartyNotices.txt")
$dependencyRoot = Split-Path (Split-Path $onnx -Parent) -Parent
$tensorRtRuntime = Test-Path -LiteralPath (Join-Path $onnx "nvinfer_10.dll")
$tensorRtLicense = Join-Path $dependencyRoot "licenses/TensorRT-10.9-LICENSE.txt"
if ($tensorRtRuntime -and (Test-Path -LiteralPath $tensorRtLicense -PathType Leaf)) {
    Copy-Item -LiteralPath $tensorRtLicense -Destination (Join-Path $licenseTarget "TensorRT-10.9-LICENSE.txt")
}

$readme = @"
RifeLove 本机运行包

启动：双击 RifeLove.exe。应用会使用此文件夹内的 data、models 和设置；不需要联网。
本次附带并完成文件校验的模型：$modelNames。首次启动默认选择 $($defaultModelPack.descriptor.displayName)。当前色彩路径支持模型清单声明的 BT.709 与 BT.470BG/SMPTE 170M 8/10-bit SDR，并保留输入位深和色彩标签；HDR、其他位深和未知色彩仍被拦截。模型许可与 SHA-256 位于各自的 models 子目录。
默认推理：优先尝试 TensorRT 10.9；不兼容的节点由 ONNX Runtime CUDA 执行，初始化失败时也会回退到 CUDA。TensorRT 引擎缓存位于本机用户目录。
默认视频编码：导入时预检查 NVIDIA NVENC；若当前驱动不兼容，会在任务开始前选择 H.264 软件编码并说明原因。RIFE 插帧仍使用 CUDA。高级编码中可以显式选择 NVENC 的恒定质量 CQ（VBR + CQ）；该设置需要本机 NVENC 可初始化，无法回退到软件 H.264。
导入视频时会按源视频的分辨率和像素格式预检查 NVENC、软件 H.264、HEVC 与 AV1；自动模式按编码器能力选择兼容格式，尽可能保留源位深。这项检查只打开编码器上下文，最终仍以实际导出结果为准。
本机命令行：.\rifelove-cli.exe interpolate "输入视频.mp4" "输出视频.mkv" --fps 60 --model-dir ".\models\$($defaultModelPack.descriptor.id)" --runtime-dir ".\runtime" --progress --report "处理报告.json"。报告记录模型/GPU、实际编码器和回退原因、端到端速度、质量保护计数及时间轴校验；不包含未实测的主观画质评分。也可以用 `rifelove-cli.exe --help` 查看全部本地命令。
桌面包含本地原片/结果分屏对比、同步播放和帧率时间步进。
进度栏显示按源时长估算的输出帧总数与剩余时间；最终帧数以时间轴验证为准。若解码帧缺少 PTS，软件只依据流声明起点或前一帧的明确时长恢复，并在扫描/处理报告中计数；缺少证据时会停止并说明，避免按平均帧率猜测可变帧率时间轴。

GPU 运行库使用本机依赖目录中的 ONNX Runtime CUDA 1.22.0、TensorRT 10.9、CUDA 12.8 和 cuDNN 9；runtime 是该本机目录的联接，不是跨电脑可搬运的发行包。目标电脑需要兼容的 NVIDIA 驱动。此本机目录不代表完成了公开发行所需的第三方再分发审核。

当前验证边界：本地 GTX 1660 SUPER；RIFE 4.25 与 4.26 FP32 均通过六组小尺寸 CPU/CUDA/TensorRT 数值对比；4.25 另通过合成 720p BT.709、30→60 FPS 端到端验证，并保留音轨和 0.5 秒时长。BT.470BG/SMPTE 170M 8-bit SDR 通过 CUDA RIFE 合成片段验证；10-bit SDR 目前通过合成 CPU 色彩转换回归，尚未完成 10-bit RIFE 端到端和真实素材画质验收。这些不是实际素材画质验收，也没有证明 4.26 画质更好。当前驱动的 NVENC API 低于 FFmpeg 要求；自动模式改用源格式兼容的软件编码，Studio 会显示可恢复提示并继续 RIFE 插帧。HDR 和真实素材对比仍未验证。
任务、偏好设置和模型中心目录保存在 data/preferences.ini 与 data/queue.sqlite。
"@
Set-Content -LiteralPath (Join-Path $target "README-本机运行.txt") -Value $readme -Encoding UTF8

$savedQpa = $env:QT_QPA_PLATFORM
$savedQuick = $env:QT_QUICK_BACKEND
$savedFont = $env:QT_QPA_FONTDIR
try {
    $env:PATH = "$target;$qt/bin;$qt/Tools/mingw1510_64/mingw64/bin;$ffmpeg/bin;$onnx;$savedPath"
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QUICK_BACKEND = "software"
    $env:QT_QPA_FONTDIR = "C:/Windows/Fonts"
    $screenshot = Join-Path $target "first-run.png"
    $quotedScreenshot = '"' + $screenshot + '"'
    $smoke = Start-Process -FilePath (Join-Path $target "RifeLove.exe") -ArgumentList "--smoke-screenshot $quotedScreenshot" -Wait -PassThru -WindowStyle Hidden
    if ($smoke.ExitCode -ne 0) { throw "Packaged RifeLove startup check failed with exit code $($smoke.ExitCode)" }
} finally {
    $env:PATH = $savedPath
    $env:QT_QPA_PLATFORM = $savedQpa
    $env:QT_QUICK_BACKEND = $savedQuick
    $env:QT_QPA_FONTDIR = $savedFont
}
if (-not (Test-Path -LiteralPath (Join-Path $target "data/preferences.ini") -PathType Leaf)) {
    throw "Packaged RifeLove did not create its first-run preferences."
}
$cliHelp = & (Join-Path $target "rifelove-cli.exe") --help 2>&1
$cliExitCode = $LASTEXITCODE
if ($cliExitCode -ne 0 -or (($cliHelp -join "`n") -notmatch "interpolate") -or (($cliHelp -join "`n") -notmatch "queue-run") -or (($cliHelp -join "`n") -notmatch "\[--report JSON\]")) {
    throw "Packaged RifeLove CLI did not expose the local interpolation and queue commands."
}

$preferences = Get-Content -LiteralPath (Join-Path $target "data/preferences.ini") -Raw
if ($preferences -notmatch [regex]::Escape([string]$defaultModelPack.descriptor.id) -or $preferences -notmatch "onnxruntime\.dll|runtime") {
    throw "Packaged first-run preferences do not contain the default bundled model and runtime."
}
foreach ($pack in $modelPacks) {
    if ($preferences -notmatch [regex]::Escape([string]$pack.descriptor.id)) {
        throw "Packaged first-run local model catalog is missing $($pack.descriptor.id)."
    }
}

$manifest = @(
    "RifeLove local bundle SHA-256",
    ((Get-FileHash -LiteralPath (Join-Path $target "RifeLove.exe") -Algorithm SHA256).Hash.ToLowerInvariant() + "  RifeLove.exe"),
    ((Get-FileHash -LiteralPath (Join-Path $target "rifelove-cli.exe") -Algorithm SHA256).Hash.ToLowerInvariant() + "  rifelove-cli.exe"),
    ((Get-FileHash -LiteralPath (Join-Path $modelsDirectory "default-model.txt") -Algorithm SHA256).Hash.ToLowerInvariant() + "  models/default-model.txt")
)
foreach ($pack in $modelPacks) {
    foreach ($name in @("model.json","model.onnx","LICENSE.txt","source-manifest.json","validation.json")) {
        $relative = "models/$($pack.descriptor.id)/$name"
        $manifest += (Get-FileHash -LiteralPath (Join-Path $target $relative) -Algorithm SHA256).Hash.ToLowerInvariant() + "  $relative"
    }
}
Set-Content -LiteralPath (Join-Path $target "SHA256SUMS.txt") -Value $manifest -Encoding UTF8
Write-Output "Packaged and launched local RifeLove build: $target"
