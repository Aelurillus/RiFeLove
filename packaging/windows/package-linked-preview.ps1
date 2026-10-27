[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$DependencyBundle = "",
    [string]$RuntimeDirectory = "",
    [string]$RifeModelDirectory = "",
    [string]$DeblurModelDirectory = "",
    [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$outRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "out"))
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repoRoot "build/windows-qt-release" }
if (-not $DependencyBundle) { $DependencyBundle = Join-Path $outRoot "RifeLove-offline-2026-10-26-v0.1.3" }
if (-not $RuntimeDirectory) { $RuntimeDirectory = "C:/itzming/dependences/rifelove-tensorrt-10.9.0.34/onnxruntime-win-x64-gpu-1.22.0/lib" }
if (-not $RifeModelDirectory) { $RifeModelDirectory = Join-Path $repoRoot "models/cache/rife-4.25-fp32-sdr8-v3" }
if (-not $DeblurModelDirectory) { $DeblurModelDirectory = Join-Path $repoRoot "models/cache/nafnet-gopro-deblur-sdr8-fp32-eval" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $outRoot "RifeLove-local-preview-2026-10-27" }

$target = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $target.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
    throw "The preview must be created inside the repository out directory."
}
if (Test-Path -LiteralPath $target) { throw "Refusing to replace an existing preview: $target" }
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$bundle = (Resolve-Path -LiteralPath $DependencyBundle).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$rife = (Resolve-Path -LiteralPath $RifeModelDirectory).Path
$deblur = (Resolve-Path -LiteralPath $DeblurModelDirectory).Path
foreach ($file in @((Join-Path $build "RifeLove.exe"),(Join-Path $build "rifelove-cli.exe"),
                    (Join-Path $runtime "onnxruntime.dll"),(Join-Path $runtime "onnxruntime_providers_cuda.dll"))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required preview file is missing: $file" }
}
$packs = @()
foreach ($directory in @($rife,$deblur)) {
    $descriptorPath = Join-Path $directory "model.json"
    $descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
    if ($descriptor.id -notmatch '^[A-Za-z0-9._-]{1,128}$' -or $descriptor.modelFile -cne "model.onnx") {
        throw "Unexpected local model descriptor: $descriptorPath"
    }
    $graph = Join-Path $directory "model.onnx"
    if ((Get-FileHash -LiteralPath $graph -Algorithm SHA256).Hash -ine $descriptor.sha256) {
        throw "Local model hash does not match its descriptor: $directory"
    }
    $packs += [pscustomobject]@{directory=$directory;descriptor=$descriptor}
}
if ($packs[0].descriptor.family -ne "rife" -or $packs[1].descriptor.family -ne "video-restoration") {
    throw "Expected one RIFE pack and one local restoration pack."
}
$pluginDirectories = @("generic","iconengines","imageformats","licenses","multimedia","networkinformation",
                       "platforms","qml","qmltooling","tls","translations")
foreach ($name in $pluginDirectories) {
    $source = Join-Path $bundle $name
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Missing dependency directory: $source" }
    $item = Get-Item -LiteralPath $source -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (Get-ChildItem -LiteralPath $source -Force -Recurse | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } | Select-Object -First 1)) {
        throw "The dependency bundle contains a link in $name."
    }
}

New-Item -ItemType Directory -Path $target | Out-Null
Copy-Item -LiteralPath (Join-Path $build "RifeLove.exe") -Destination $target
Copy-Item -LiteralPath (Join-Path $build "rifelove-cli.exe") -Destination $target
foreach ($file in (Get-ChildItem -LiteralPath $bundle -Filter "*.dll" -File)) {
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
foreach ($name in $pluginDirectories) {
    Copy-Item -LiteralPath (Join-Path $bundle $name) -Destination (Join-Path $target $name) -Recurse
}
New-Item -ItemType Junction -Path (Join-Path $target "runtime") -Target $runtime | Out-Null
$models = Join-Path $target "models"
New-Item -ItemType Directory -Path $models | Out-Null
foreach ($pack in $packs) {
    New-Item -ItemType Junction -Path (Join-Path $models $pack.descriptor.id) -Target $pack.directory | Out-Null
}
[IO.File]::WriteAllText((Join-Path $models "default-model.txt"),[string]$packs[0].descriptor.id + "`n",[Text.UTF8Encoding]::new($false))
New-Item -ItemType File -Path (Join-Path $target "portable.flag") | Out-Null
$readme = @"
RifeLove 本机开发预览

双击 RifeLove.exe 启动。此预览只在这台电脑使用，runtime 和 models 目录链接到本机依赖及模型缓存；移动或删除那些目录后需要重新生成预览。
默认补帧模型为 RIFE 4.25。高级模型设置可选 NAFNet 去模糊（实验），仅供已标记 BT.709 SDR 8-bit 视频试用；它逐帧处理，可能闪烁，速度也较慢。首次使用 TensorRT 可能需要数分钟构建引擎。
当前预览没有附带已完成画质和许可验收的超分模型，去模糊与超分尚不能串联。300 FPS 以上是输出视频帧率选项，不是本机每秒处理速度的保证。
视频、任务和设置保存在本机。此目录是开发预览，不是正式发行版。
"@
[IO.File]::WriteAllText((Join-Path $target "使用说明.txt"),$readme,[Text.UTF8Encoding]::new($false))
$metadata = [ordered]@{kind="local-linked-preview"; executableSha256=(Get-FileHash -LiteralPath (Join-Path $target "RifeLove.exe") -Algorithm SHA256).Hash.ToLowerInvariant();
    runtime=$runtime; rifeModel=$rife; deblurModel=$deblur; redistributable=$false}
$metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $target "preview-info.json") -Encoding utf8NoBOM
Write-Output "Created local linked preview: $target"
