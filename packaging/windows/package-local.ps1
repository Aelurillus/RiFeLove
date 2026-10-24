[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "../../build/windows-qt-release"),
    [string]$QtRoot = "C:/itzming/dependences/qt-6.12.0-local",
    [string]$FfmpegRoot = "C:/itzming/dependences/ffmpeg-8.1.3/ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-shared-8.1",
    [string]$OnnxRuntimeRoot = "C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0",
    [string]$ModelPackRoot = (Join-Path $PSScriptRoot "../../models/cache/rife-4.25-fp32-sdr8-v2"),
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
$model = (Resolve-Path (Join-Path $ModelPackRoot "model.json")).Path | Split-Path -Parent
$executable = Join-Path $build "RifeLove.exe"
$deployTool = Join-Path $qt "bin/windeployqt.exe"
foreach ($required in @($executable,$deployTool,(Join-Path $onnx "onnxruntime_providers_cuda.dll"),(Join-Path $model "model.onnx"))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required packaging input is missing: $required" }
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

$modelTarget = Join-Path $target "models/rife-4.25-fp32-sdr8-v2"
New-Item -ItemType Directory -Path $modelTarget -Force | Out-Null
foreach ($name in @("model.json","model.onnx","LICENSE.txt","source-manifest.json","validation.json")) {
    $source = Join-Path $model $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required model package file is missing: $source" }
    Copy-Item -LiteralPath $source -Destination $modelTarget
}

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

$readme = @"
RifeLove 本机运行包

启动：双击 RifeLove.exe。应用会使用此文件夹内的 data、models 和设置；不需要联网。
本次附带且已校验的模型：RIFE 4.25 FP32（BT.709 8-bit SDR）。模型许可与 SHA-256 位于 models 和 licenses 文件夹。
默认视频编码：自动尝试 NVIDIA NVENC；驱动不支持时会回退到 H.264 软件编码。

GPU 运行库使用本机已安装的 ONNX Runtime CUDA 1.22.0 与 NVIDIA CUDA 12.8/cuDNN 9 SDK，runtime 是其本机目录联接，不是跨电脑可搬运的发行包。目标电脑仍需 NVIDIA 驱动、兼容的 CUDA/cuDNN 运行环境。此本机目录不代表完成了公开发行所需的第三方再分发审核。

当前验证边界：本地 GTX 1660 SUPER；已验证流程为显式 BT.709、8-bit SDR。TensorRT、HDR、10-bit AI 插帧和其它 RIFE 模型尚未随包提供。
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

$preferences = Get-Content -LiteralPath (Join-Path $target "data/preferences.ini") -Raw
if ($preferences -notmatch "model\.json|rife-4\.25" -or $preferences -notmatch "onnxruntime\.dll|runtime") {
    throw "Packaged first-run preferences do not contain the bundled model and runtime defaults."
}

$manifest = @(
    "RifeLove local bundle SHA-256",
    ((Get-FileHash -LiteralPath (Join-Path $target "RifeLove.exe") -Algorithm SHA256).Hash.ToLowerInvariant() + "  RifeLove.exe"),
    ((Get-FileHash -LiteralPath (Join-Path $modelTarget "model.onnx") -Algorithm SHA256).Hash.ToLowerInvariant() + "  models/rife-4.25-fp32-sdr8-v2/model.onnx")
)
Set-Content -LiteralPath (Join-Path $target "SHA256SUMS.txt") -Value $manifest -Encoding UTF8
Write-Output "Packaged and launched local RifeLove build: $target"
