[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$QtRoot = "",
    [string]$FfmpegRoot = "",
    [string]$OnnxRuntimeRoot = "",
    [string]$InferenceRuntimeDirectory = "",
    [string]$ModelPackRoot = "",
    [string[]]$AdditionalModelPackRoots = @(),
    [string]$ReuseQualificationEvidencePath = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $scriptRoot "../../build/windows-qt-release" }
if ([string]::IsNullOrWhiteSpace($QtRoot)) { $QtRoot = "C:/itzming/dependences/qt-6.12.0-local" }
if ([string]::IsNullOrWhiteSpace($FfmpegRoot)) { $FfmpegRoot = "C:/itzming/dependences/ffmpeg-8.1.3/ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-shared-8.1" }
if ([string]::IsNullOrWhiteSpace($OnnxRuntimeRoot)) { $OnnxRuntimeRoot = "C:/itzming/dependences/onnxruntime-win-x64-gpu-1.22.0" }
if ([string]::IsNullOrWhiteSpace($InferenceRuntimeDirectory)) { $InferenceRuntimeDirectory = "C:/itzming/dependences/rifelove-tensorrt-10.9.0.34/onnxruntime-win-x64-gpu-1.22.0/lib" }
if ([string]::IsNullOrWhiteSpace($ModelPackRoot)) { $ModelPackRoot = Join-Path $scriptRoot "../../models/cache/rife-4.25-fp32-sdr8-v3" }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $scriptRoot "../../out/RifeLove-local" }
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot "../.."))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "out"))
$target = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $target.StartsWith($outputRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory must be inside the repository's ignored out directory."
}
$packageLeaf = Split-Path -Path $target -Leaf
$packageParent = Split-Path -Path $target -Parent
$packageRoot = Join-Path $packageParent (".{0}.staging-{1}" -f $packageLeaf, [Guid]::NewGuid().ToString("N"))
$packageHelpers = Join-Path $scriptRoot "package-helpers.ps1"
if (-not (Test-Path -LiteralPath $packageHelpers -PathType Leaf)) { throw "Required packaging helpers are missing: $packageHelpers" }
. $packageHelpers

$build = (Resolve-Path $BuildDirectory).Path
$qt = (Resolve-Path $QtRoot).Path
$ffmpeg = (Resolve-Path $FfmpegRoot).Path
$onnx = (Resolve-Path (Join-Path $OnnxRuntimeRoot "lib/onnxruntime.dll")).Path | Split-Path -Parent
$inferenceRuntime = (Resolve-Path $InferenceRuntimeDirectory).Path
$runtimeVersionFile = Join-Path $inferenceRuntime "tensorrt-runtime.version"
if (-not (Test-Path -LiteralPath $runtimeVersionFile -PathType Leaf)) { throw "Pinned TensorRT runtime manifest is missing: $runtimeVersionFile" }
$runtimeVersionLines = Get-Content -LiteralPath $runtimeVersionFile
if ($runtimeVersionLines.Count -lt 2 -or $runtimeVersionLines[0] -notmatch '^10\.9\.0\.' -or $runtimeVersionLines[1] -notmatch '^[0-9a-fA-F]{64}$') {
    throw "Pinned TensorRT runtime manifest is invalid: $runtimeVersionFile"
}
$ortRuntimeVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $inferenceRuntime "onnxruntime.dll")).FileVersion
if (-not $ortRuntimeVersion.StartsWith("1.22.")) { throw "Expected ONNX Runtime 1.22 in the TensorRT runtime directory; found $ortRuntimeVersion" }
$requiredInferenceRuntimeFiles = @(
    "onnxruntime.dll","onnxruntime_providers_cuda.dll","onnxruntime_providers_tensorrt.dll","onnxruntime_providers_shared.dll",
    "nvinfer_10.dll","nvinfer_plugin_10.dll","nvinfer_builder_resource_10.dll","nvonnxparser_10.dll","cudart64_12.dll","cublas64_12.dll","cublasLt64_12.dll",
    "cufft64_11.dll","cudnn64_9.dll","cudnn_graph64_9.dll","cudnn_ops64_9.dll","cudnn_cnn64_9.dll","cudnn_adv64_9.dll",
    "cudnn_engines_precompiled64_9.dll","cudnn_engines_runtime_compiled64_9.dll","cudnn_heuristic64_9.dll"
)
foreach ($name in $requiredInferenceRuntimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $inferenceRuntime $name) -PathType Leaf)) {
        throw "Pinned inference runtime is missing required file: $(Join-Path $inferenceRuntime $name)"
    }
}
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
$validatorExecutable = Join-Path $build "rifelove-validate-cuda.exe"
$deployTool = Join-Path $qt "bin/windeployqt.exe"
foreach ($required in @($executable,$cliExecutable,$validatorExecutable,$deployTool,(Join-Path $onnx "onnxruntime_providers_cuda.dll"))) {
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
}
Assert-PackageNotRunning -TargetPath $target
if (Test-Path -LiteralPath $packageRoot) { throw "The unique package staging path already exists: $packageRoot" }
New-Item -ItemType Directory -Path $packageRoot | Out-Null
try {
Copy-Item -LiteralPath $executable -Destination $packageRoot
Copy-Item -LiteralPath $cliExecutable -Destination $packageRoot
$savedPath = $env:PATH
try {
    $env:PATH = "$qt/bin;$ffmpeg/bin;$savedPath"
    & $deployTool --release --compiler-runtime --qmldir (Join-Path $repositoryRoot "ui") --dir $packageRoot (Join-Path $packageRoot "RifeLove.exe")
    if ($LASTEXITCODE -ne 0) { throw "Qt runtime deployment failed with exit code $LASTEXITCODE" }
} finally {
    $env:PATH = $savedPath
}

foreach ($name in @("avcodec-62.dll","avformat-62.dll","avutil-60.dll","swscale-9.dll","swresample-6.dll")) {
    $source = Join-Path $ffmpeg "bin/$name"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required FFmpeg runtime is missing: $source" }
    Copy-Item -LiteralPath $source -Destination $packageRoot
}

foreach ($pack in $modelPacks) {
    $modelTarget = Join-Path $packageRoot ("models/{0}" -f $pack.descriptor.id)
    New-Item -ItemType Directory -Path $modelTarget -Force | Out-Null
    foreach ($name in @("model.json","model.onnx","LICENSE.txt","source-manifest.json","validation.json")) {
        $source = Join-Path $pack.root $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required model package file is missing: $source" }
        Copy-Item -LiteralPath $source -Destination $modelTarget
    }
}
$modelsDirectory = Join-Path $packageRoot "models"
Set-Content -LiteralPath (Join-Path $modelsDirectory "default-model.txt") -Value ([string]$defaultModelPack.descriptor.id) -Encoding UTF8

# Keep NVIDIA CUDA/cuDNN under their local SDK installation; this device-local
# link avoids silently redistributing those separately licensed binaries.
$runtimeTarget = Join-Path $packageRoot "runtime"
New-Item -ItemType Junction -Path $runtimeTarget -Target $inferenceRuntime | Out-Null
New-Item -ItemType File -Path (Join-Path $packageRoot "portable.flag") | Out-Null
$platformTarget = Join-Path $packageRoot "platforms"
New-Item -ItemType Directory -Path $platformTarget -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $qt "plugins/platforms/qoffscreen.dll") -Destination $platformTarget
$licenseTarget = Join-Path $packageRoot "licenses"
New-Item -ItemType Directory -Path $licenseTarget | Out-Null
Copy-Item -LiteralPath (Join-Path $repositoryRoot "LICENSE") -Destination (Join-Path $licenseTarget "RifeLove-MIT.txt")
Copy-Item -LiteralPath (Join-Path $ffmpeg "LICENSE.txt") -Destination (Join-Path $licenseTarget "FFmpeg-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path (Split-Path $onnx -Parent) "LICENSE") -Destination (Join-Path $licenseTarget "ONNX-Runtime-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path (Split-Path $onnx -Parent) "ThirdPartyNotices.txt") -Destination (Join-Path $licenseTarget "ONNX-Runtime-ThirdPartyNotices.txt")
$spdlogLicenseCandidates = @(
    (Join-Path $build "_deps/spdlog-src/LICENSE"),
    (Join-Path $repositoryRoot "vcpkg_installed/x64-mingw-dynamic/share/spdlog/copyright")
)
$spdlogLicense = $spdlogLicenseCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $spdlogLicense) { throw "The spdlog MIT license file is missing from the configured dependency source." }
Copy-Item -LiteralPath $spdlogLicense -Destination (Join-Path $licenseTarget "spdlog-MIT.txt")
$tensorRtRoot = Split-Path (Split-Path $inferenceRuntime -Parent) -Parent
$tensorRtLicense = Join-Path $tensorRtRoot "licenses/TensorRT-10.9-LICENSE.txt"
if (-not (Test-Path -LiteralPath $tensorRtLicense -PathType Leaf)) { throw "TensorRT license file is missing: $tensorRtLicense" }
Copy-Item -LiteralPath $tensorRtLicense -Destination (Join-Path $licenseTarget "TensorRT-10.9-LICENSE.txt")

$readmeLines = @(
    'RifeLove 本机运行包',
    '',
    '启动：双击 RifeLove.exe。应用会使用此文件夹内的 data、models 和设置；不需要联网。',
    '运行日志只保存在本机：桌面日志位于 data/logs/rifelove.jsonl，CLI 日志位于 %LOCALAPPDATA%/RifeLove/logs/cli.jsonl。日志最多 4 MiB 并保留最近 3 个轮转文件；错误详情有时会包含本地文件名，分享前请检查。',
    '本次附带并完成文件校验的模型：{0}。首次启动默认选择 {1}。当前色彩路径支持模型清单声明的 BT.709 与 BT.470BG/SMPTE 170M 8/10-bit SDR，并保留输入位深和色彩标签；HDR、其他位深和未知色彩仍被拦截。模型许可与 SHA-256 位于各自的 models 子目录。',
    '默认推理：优先尝试 TensorRT 10.9 FP32；不兼容的节点由 ONNX Runtime CUDA 执行，初始化失败时也会回退到 CUDA。两套随包模型都经过本机 TensorRT FP32 数值校验，摘要保存在 data/tensorrt-validation.json。FP16 在当前 GTX 1660 SUPER 上未通过 8-bit SDR 精度门槛，因此不作为软件处理选项。TensorRT 引擎缓存位于本机用户目录。',
    '默认视频编码：任务开始前按目标帧率、像素格式、码率和编码选项预检查 NVIDIA NVENC；若当前驱动或像素格式不兼容，会选择已通过同一输出设置检查的软件编码器。界面会分别标明 CPU 视频编码和 CUDA RIFE 插帧。当前驱动提供 NVENC API 12.2，而本机 FFmpeg 要求 API 13.0 / 驱动 570.0 或更新，所以自动模式将使用软件 H.264。高级编码中可以显式选择 NVENC 的恒定质量 CQ（VBR + CQ）；该设置需要本机 NVENC 可初始化，无法回退到软件编码。',
    '导入视频时会显示按源分辨率和像素格式检查的 NVENC、软件 H.264、HEVC 与 AV1 能力；开始任务时会再次按目标输出帧率、时间基、像素格式、码率、GOP 和编码选项检查实际配置。进度栏按预计总帧数显示确定进度与剩余时间；估算总帧数不替代最终时间轴验证。',
    '本机命令行：.\rifelove-cli.exe interpolate "输入视频.mp4" "输出视频.mkv" --fps 60 --model-dir ".\models\{2}" --runtime-dir ".\runtime" --progress --report "处理报告.json"。报告记录模型/GPU、实际编码器和回退原因、端到端速度、质量保护计数及时间轴校验；不包含未实测的主观画质评分。',
    '本地插帧会优先尝试 CUDA/NVDEC 解码；设备、解码器或像素格式协商不可用时会在交付帧前改用软件解码。可运行 .\rifelove-cli.exe media-scan "输入视频.mp4" --decode cuda 查看实际解码路径与回退原因。',
    '复用 Studio 导出的本机配置：先运行 .\rifelove-cli.exe profile-check "配置.json" 检查配置，再运行 .\rifelove-cli.exe interpolate "输入视频.mp4" "输出视频.mkv" --profile "配置.json"。输入与输出文件需明确给出，输出扩展名必须匹配配置中的容器；配置和模型均从本机读取。',
    '队列报告：在任务队列点击“导出报告”，或运行 .\rifelove-cli.exe queue-report ".\data\queue.sqlite" --output "队列报告.json"。报告包括任务状态、失败原因、处理帧数、完成任务平均输出 FPS 和质量保护计数；没有参考视频时不会给出 PSNR、SSIM 或 LPIPS。也可以用 `rifelove-cli.exe --help` 查看全部本地命令。',
    '桌面包含本地原片/结果分屏对比、同步播放、精确 CFR/VFR 帧步进，以及打开原片或结果所在文件夹的操作。',
    '任务队列每 256 个输出帧保存一个已验证检查点。进程异常退出后，重开软件可在中断任务上选择“从检查点继续”；软件会校验本机源文件、模型、运行库和片段，再跳过已完成的插帧。若最终视频已发布但队列状态未提交，启动时会核对成品大小、SHA-256 和验证记录，匹配时自动恢复为完成；不匹配时保留文件并显示诊断。普通暂停/继续仍在当前进程内接着处理。',
    '进度栏显示按源时长估算的输出帧总数与剩余时间；最终帧数以时间轴验证为准。工作期间会显示 RIFE 推理、编码、解码、预处理/色彩和输出验证阶段耗时，方便判断处理速度受哪一环影响。若解码帧缺少 PTS，软件只依据流声明起点或前一帧的明确时长恢复，并在扫描/处理报告中计数；缺少证据时会停止并说明，避免按平均帧率猜测可变帧率时间轴。',
    '',
    'GPU 运行库使用本机依赖目录中的 ONNX Runtime CUDA 1.22.0、TensorRT 10.9、CUDA 12.8 和 cuDNN 9；runtime 联接指向完整的本机推理运行库。它不是跨电脑可搬运的发行包。目标电脑需要兼容的 NVIDIA 驱动。此本机目录不代表完成了公开发行所需的第三方再分发审核。',
    '',
    '当前验证边界：本地 GTX 1660 SUPER；RIFE 4.25 与 4.26 FP32 均通过六组小尺寸 CPU/CUDA/TensorRT 数值对比；4.25 另通过合成 720p BT.709、30→60 FPS 端到端验证，并保留音轨和 0.5 秒时长。RIFE 4.26 TensorRT FP32 六组对照最大绝对误差 0.00183、最低 PSNR 85.94 dB；相同模型的 TensorRT FP16 六组测试未通过门槛（最大误差 0.953、最低 PSNR 16.53 dB），因此当前只启用 FP32。BT.470BG/SMPTE 170M 8-bit SDR 通过 CUDA RIFE 合成片段验证；BT.709 10-bit SDR 通过 96x64 合成 RIFE 插帧、AV1 回退和输出色彩/时间轴检查，但不是实际素材画质验收。这些结果没有证明 4.26 画质更好。当前驱动的 NVENC API 低于 FFmpeg 要求；自动模式改用源格式兼容的软件编码，Studio 会显示可恢复提示并继续 RIFE 插帧。HDR 和真实素材对比仍未验证。',
    '任务、偏好设置和模型中心目录保存在 data/preferences.ini 与 data/queue.sqlite.'
)
$readme = ($readmeLines -join [Environment]::NewLine) -f $modelNames, [string]$defaultModelPack.descriptor.displayName, [string]$defaultModelPack.descriptor.id
Set-Content -LiteralPath (Join-Path $packageRoot "README-本机运行.txt") -Value $readme -Encoding UTF8

$savedQpa = $env:QT_QPA_PLATFORM
$savedQuick = $env:QT_QUICK_BACKEND
$savedFont = $env:QT_QPA_FONTDIR
try {
    $env:PATH = "$packageRoot;$qt/bin;$qt/Tools/mingw1510_64/mingw64/bin;$ffmpeg/bin;$onnx;$savedPath"
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QUICK_BACKEND = "software"
    $env:QT_QPA_FONTDIR = "C:/Windows/Fonts"
    $screenshot = Join-Path $packageRoot "first-run.png"
    $quotedScreenshot = '"' + $screenshot + '"'
    $smoke = Start-Process -FilePath (Join-Path $packageRoot "RifeLove.exe") -ArgumentList "--smoke-screenshot $quotedScreenshot" -Wait -PassThru -WindowStyle Hidden
    if ($smoke.ExitCode -ne 0) { throw "Packaged RifeLove startup check failed with exit code $($smoke.ExitCode)" }
    $studioScreenshot = Join-Path $packageRoot "studio-smoke.png"
    $quotedStudioScreenshot = '"' + $studioScreenshot + '"'
    $studioSmoke = Start-Process -FilePath (Join-Path $packageRoot "RifeLove.exe") -ArgumentList "--smoke-screenshot $quotedStudioScreenshot --smoke-page studio" -Wait -PassThru -WindowStyle Hidden
    if ($studioSmoke.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $studioScreenshot -PathType Leaf)) {
        throw "Packaged RifeLove Studio page check failed with exit code $($studioSmoke.ExitCode)"
    }
} finally {
    $env:PATH = $savedPath
    $env:QT_QPA_PLATFORM = $savedQpa
    $env:QT_QUICK_BACKEND = $savedQuick
    $env:QT_QPA_FONTDIR = $savedFont
}
if (-not (Test-Path -LiteralPath (Join-Path $packageRoot "data/preferences.ini") -PathType Leaf)) {
    throw "Packaged RifeLove did not create its first-run preferences."
}
$cliHelp = & (Join-Path $packageRoot "rifelove-cli.exe") --help 2>&1
$cliExitCode = $LASTEXITCODE
if ($cliExitCode -ne 0 -or (($cliHelp -join "`n") -notmatch "interpolate") -or (($cliHelp -join "`n") -notmatch "queue-run") -or (($cliHelp -join "`n") -notmatch "queue-report") -or (($cliHelp -join "`n") -notmatch "profile-check") -or (($cliHelp -join "`n") -notmatch "--profile PROFILE_JSON") -or (($cliHelp -join "`n") -notmatch "\[--report JSON\]")) {
    throw "Packaged RifeLove CLI did not expose the local interpolation and queue commands."
}

$preferences = Get-Content -LiteralPath (Join-Path $packageRoot "data/preferences.ini") -Raw
if ($preferences -notmatch [regex]::Escape([string]$defaultModelPack.descriptor.id) -or $preferences -notmatch "onnxruntime\.dll|runtime") {
    throw "Packaged first-run preferences do not contain the default bundled model and runtime."
}
foreach ($pack in $modelPacks) {
    if ($preferences -notmatch [regex]::Escape([string]$pack.descriptor.id)) {
        throw "Packaged first-run local model catalog is missing $($pack.descriptor.id)."
    }
}

$qualificationEvidencePath = Join-Path $packageRoot "data/tensorrt-validation.json"
$qualificationEvidence = $null
if (-not [string]::IsNullOrWhiteSpace($ReuseQualificationEvidencePath)) {
    $sourceEvidencePath = (Resolve-Path -LiteralPath $ReuseQualificationEvidencePath).Path
    if ((Split-Path -Path $sourceEvidencePath -Leaf) -cne "tensorrt-validation.json" -or
        (Split-Path -Path (Split-Path -Path $sourceEvidencePath -Parent) -Leaf) -cne "data") {
        throw "Reusable TensorRT evidence must be data/tensorrt-validation.json inside a qualified local package."
    }
    $sourcePackageRoot = Split-Path -Path (Split-Path -Path $sourceEvidencePath -Parent) -Parent
    $sourceManifestPath = Join-Path $sourcePackageRoot "SHA256SUMS.txt"
    if (-not (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf)) {
        throw "Reusable TensorRT evidence has no package checksum manifest."
    }
    $manifestMatch = [regex]::Match((Get-Content -LiteralPath $sourceManifestPath -Raw),
        '(?m)^([0-9a-fA-F]{64})  data/tensorrt-validation\.json\r?$')
    if (-not $manifestMatch.Success) { throw "The source package manifest does not bind its TensorRT evidence." }
    $sourceEvidenceHash = (Get-FileHash -LiteralPath $sourceEvidencePath -Algorithm SHA256).Hash
    if ($sourceEvidenceHash -ine $manifestMatch.Groups[1].Value) {
        throw "The source package TensorRT evidence does not match its recorded SHA-256."
    }
    $cachedEvidence = Get-Content -LiteralPath $sourceEvidencePath -Raw | ConvertFrom-Json
    $gpuOutput = & nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>&1
    $gpuExitCode = $LASTEXITCODE
    if ($gpuExitCode -ne 0) { throw "Could not verify the current NVIDIA device for cached TensorRT evidence: $($gpuOutput -join "`n")" }
    $gpuIdentities = @(
        foreach ($line in $gpuOutput) {
            $fields = ([string]$line).Split(',',2)
            if ($fields.Count -eq 2) {
                [pscustomobject]@{name=$fields[0].Trim();computeCapability=$fields[1].Trim()}
            }
        }
    )
    Assert-TensorRtQualificationEvidence -Evidence $cachedEvidence -ModelPacks $modelPacks `
        -OnnxRuntimeVersion $ortRuntimeVersion -TensorRtVersion ([string]$runtimeVersionLines[0]) `
        -TensorRtManifestSha256 ([string]$runtimeVersionLines[1]) -GpuIdentities $gpuIdentities
    $qualificationEvidence = [ordered]@{
        schemaVersion = 1
        generatedAtUtc = [string]$cachedEvidence.generatedAtUtc
        reusedAtUtc = [DateTime]::UtcNow.ToString("o")
        reusedFromPackage = Split-Path -Path $sourcePackageRoot -Leaf
        runtime = $cachedEvidence.runtime
        models = @($cachedEvidence.models)
    }
} else {
    $qualificationDirectory = Join-Path $packageRoot "data/tensorrt-qualification"
    New-Item -ItemType Directory -Path $qualificationDirectory -Force | Out-Null
    $savedPath = $env:PATH
    $qualificationSummaries = @()
    try {
        $mingwRuntime = Join-Path $qt "Tools/mingw1510_64/mingw64/bin"
        $env:PATH = "$build;$packageRoot;$qt/bin;$ffmpeg/bin;$inferenceRuntime;$mingwRuntime;$savedPath"
        foreach ($pack in $modelPacks) {
            $reportPath = Join-Path $qualificationDirectory ("{0}.json" -f $pack.descriptor.id)
            $validationOutput = & $validatorExecutable $pack.root $runtimeTarget $reportPath tensorrt 2>&1
            $validationExitCode = $LASTEXITCODE
            if ($validationExitCode -ne 0) {
                throw "Native TensorRT FP32 qualification failed for $($pack.descriptor.id): $($validationOutput -join "`n")"
            }
            $validation = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            if ($validation.status -ne "passed_native_tensorrt_reference" -or
                $validation.backend -ne "onnxruntime_tensorrt_cuda" -or
                $validation.requested_precision -ne "fp32" -or
                $validation.fp16_enabled -or
                $validation.model_sha256 -ne $pack.descriptor.sha256 -or
                $validation.cases.Count -ne 6 -or
                $validation.tensorrt_heavy_operators.Count -lt 1) {
                throw "TensorRT FP32 report did not prove the requested runtime, model or six image-compute reference cases for $($pack.descriptor.id)."
            }
            $maximumError = [double](($validation.cases | Measure-Object -Property max_abs_error -Maximum).Maximum)
            $minimumPsnr = [double](($validation.cases | Measure-Object -Property psnr_db -Minimum).Minimum)
            if ($maximumError -gt [double]$validation.max_abs_error_limit -or $minimumPsnr -lt [double]$validation.minimum_psnr_db) {
                throw "TensorRT FP32 numerical thresholds were not met for $($pack.descriptor.id)."
            }
            $qualificationSummaries += [ordered]@{
                modelId = [string]$pack.descriptor.id
                modelVersion = [string]$pack.descriptor.version
                modelSha256 = [string]$validation.model_sha256
                device = [string]$validation.device
                computeCapability = [string]$validation.compute_capability
                precision = "fp32"
                backend = [string]$validation.backend
                referenceCasesPassed = $validation.cases.Count
                maxAbsError = $maximumError
                allowedMaxAbsError = [double]$validation.max_abs_error_limit
                minPsnrDb = $minimumPsnr
                requiredMinPsnrDb = [double]$validation.minimum_psnr_db
                tensorrtImageOperators = @($validation.tensorrt_heavy_operators)
            }
        }
    } finally {
        $env:PATH = $savedPath
    }
    $qualificationEvidence = [ordered]@{
        schemaVersion = 1
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        runtime = [ordered]@{
            onnxRuntimeVersion = $ortRuntimeVersion
            tensorRtVersion = [string]$runtimeVersionLines[0]
            tensorRtManifestSha256 = [string]$runtimeVersionLines[1]
        }
        models = @($qualificationSummaries)
    }
    Remove-Item -LiteralPath $qualificationDirectory -Recurse -Force
}
$qualificationEvidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $qualificationEvidencePath -Encoding UTF8

Assert-PackageNotRunning -TargetPath $target
Preserve-PackageUserState -OldRoot $target -NewRoot $packageRoot

$manifest = New-PackageChecksumManifest -PackageRoot $packageRoot -RuntimeRoot $inferenceRuntime -QualificationEvidencePath $qualificationEvidencePath
Set-Content -LiteralPath (Join-Path $packageRoot "SHA256SUMS.txt") -Value $manifest -Encoding UTF8

# Copy mutable local state only after package qualification succeeds. Refuse to
# switch while either app from this exact package is open, so SQLite data and
# loaded files remain stable throughout the upgrade.
Promote-PackageDirectory -StagingPath $packageRoot -TargetPath $target
Write-Output "Packaged local RifeLove build: $target"
} catch {
    if (Test-Path -LiteralPath $packageRoot) {
        try { Remove-PackagePath -Path $packageRoot }
        catch { Write-Warning "Could not clean failed package staging directory $packageRoot`: $($_.Exception.Message)" }
    }
    throw
}
