[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$FfmpegRoot = "",
    [string]$InferenceRuntimeDirectory = "",
    [string]$MinGWBinDirectory = "",
    [string]$CodecLicenseRoot = "",
    [string]$CodecDepsRoot = "",
    [string]$RifeModelDirectory = "",
    [string]$LegacyRifeModelDirectory = "",
    [string]$SuperResolutionModelDirectory = "",
    [string]$TemporalRestorationModelDirectory = "",
    [string]$CompressionRestorationModelDirectory = "",
    [string]$DeblurModelDirectory = "",
    [string]$OutputDirectory = "",
    # A fresh, data-free RIFE + SR package; experiments are explicit additions.
    [switch]$BaseModelsOnly,
    [switch]$RifeOnly,
    [switch]$IncludeTemporalRestoration,
    [switch]$IncludeCompressionRestoration,
    [switch]$IncludeDeblurRestoration,
    [switch]$CudaOnly,
    [switch]$SkipStartupSmoke,
    [switch]$ValidateDependenciesOnly
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptRoot "ffmpeg-sdk-helpers.ps1")
. (Join-Path $scriptRoot "native-build-dependencies.ps1")
. (Join-Path $scriptRoot "native-package-options.ps1")
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot "../.."))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "out"))
$modelSelection = Resolve-NativeModelBundleSelection -BaseModelsOnly:$BaseModelsOnly -RifeOnly:$RifeOnly -IncludeTemporalRestoration:$IncludeTemporalRestoration -IncludeCompressionRestoration:$IncludeCompressionRestoration -IncludeDeblurRestoration:$IncludeDeblurRestoration
$freshModelSelection = $BaseModelsOnly -or $RifeOnly

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $repositoryRoot "build/windows-release" }
if ([string]::IsNullOrWhiteSpace($FfmpegRoot)) { $FfmpegRoot = Join-Path $repositoryRoot "work/codec-ffmpeg-build-x86asm/ffmpeg-install" }
if ([string]::IsNullOrWhiteSpace($CodecLicenseRoot)) { $CodecLicenseRoot = Join-Path $repositoryRoot "work/codec-ffmpeg-build" }
if ([string]::IsNullOrWhiteSpace($CodecDepsRoot)) { $CodecDepsRoot = Join-Path $repositoryRoot "work/codec-deps/buildtrees" }
if ([string]::IsNullOrWhiteSpace($RifeModelDirectory)) { $RifeModelDirectory = Join-Path $repositoryRoot "models/cache/rife-4.25-fp32-sdr8-v3" }
if ([string]::IsNullOrWhiteSpace($LegacyRifeModelDirectory)) { $LegacyRifeModelDirectory = Join-Path $repositoryRoot "models/cache/rife-4.25-fp32-sdr8-v2" }
if ([string]::IsNullOrWhiteSpace($SuperResolutionModelDirectory)) { $SuperResolutionModelDirectory = Join-Path $repositoryRoot "models/cache/realesrgan-x2plus-sdr8-candidate-v2" }
if ([string]::IsNullOrWhiteSpace($TemporalRestorationModelDirectory)) { $TemporalRestorationModelDirectory = Join-Path $repositoryRoot "models/cache/fastdvdnet-clipped-five-frame-sdr8-eval" }
if ([string]::IsNullOrWhiteSpace($CompressionRestorationModelDirectory)) { $CompressionRestorationModelDirectory = Join-Path $repositoryRoot "models/cache/stdf-r3-y7-sdr8-local-eval" }
if ([string]::IsNullOrWhiteSpace($DeblurModelDirectory)) { $DeblurModelDirectory = Join-Path $repositoryRoot "models/cache/nafnet-gopro-deblur-sdr8-fp32-eval" }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $outputRoot "RifeLoveNative-local-nvenc" }

function Resolve-ExistingDirectory([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "$Label 目录不存在：$Path" }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "$Label 目录不能是链接或 junction：$Path" }
    return $item.FullName
}

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label 文件不存在：$Path" }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "$Label 文件不能是链接：$Path" }
    return $item.FullName
}

function Resolve-CodecLicense([string]$Root, [string]$RelativePath, [string]$Label) {
    $path = Join-Path $Root $RelativePath
    if (Test-Path -LiteralPath $path -PathType Leaf) { return (Resolve-ExistingFile $path $Label) }
    throw "$Label 文件不存在：$path"
}

function Find-CodecSourceLicense([string]$Root, [string]$RelativeRoot, [string]$Label) {
    $searchRoot = Join-Path $Root $RelativeRoot
    if (-not (Test-Path -LiteralPath $searchRoot -PathType Container)) { throw "$Label 来源目录不存在：$searchRoot" }
    $files = @(Get-ChildItem -LiteralPath $searchRoot -Recurse -File -Force | Where-Object {
        $_.Name -in @("LICENSE", "LICENSE.txt", "LICENSE.md", "COPYING")
    } | Sort-Object FullName)
    if ($files.Count -eq 0) { throw "$Label 缺少许可文件：$searchRoot" }
    foreach ($file in $files) {
        if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        return $file.FullName
    }
    throw "$Label 许可文件全部为链接：$searchRoot"
}

function Assert-InOutputRoot([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $prefix = $outputRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "OutputDirectory 必须位于仓库 out 目录内：$full"
    }
    return $full
}

function Assert-NoLinkTree([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "拒绝使用包含链接的路径：$Path" }
    foreach ($child in (Get-ChildItem -LiteralPath $Path -Force -Recurse)) {
        if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "源依赖包含链接，拒绝不受限复制：$($child.FullName)" }
    }
}

function Remove-PackagePath([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        if ($item.PSIsContainer) { [IO.Directory]::Delete($Path, $false) } else { Remove-Item -LiteralPath $Path -Force }
        return
    }
    if ($item.PSIsContainer) {
        foreach ($child in (Get-ChildItem -LiteralPath $Path -Force)) { Remove-PackagePath $child.FullName }
    }
    Remove-Item -LiteralPath $Path -Force
}

function Get-PackageTreeItems([string]$Path) {
    foreach ($child in (Get-ChildItem -LiteralPath $Path -Force)) {
        $child
        if ($child.PSIsContainer -and -not ($child.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Get-PackageTreeItems $child.FullName
        }
    }
}

function Get-RunningNativePackageProcesses([string]$TargetPath) {
    $prefix = [IO.Path]::GetFullPath($TargetPath).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    # Match by executable path rather than process name so a locally renamed
    # native binary (for example RifeLoveNative-new.exe during a staged UI
    # handoff) still blocks an unsafe package replacement.
    foreach ($process in (Get-Process -ErrorAction SilentlyContinue)) {
        $processPath = $null
        try { $processPath = $process.Path } catch { }
        if (-not [string]::IsNullOrWhiteSpace($processPath)) {
            $full = [IO.Path]::GetFullPath($processPath)
            if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                [pscustomobject]@{ Name = $process.ProcessName; Id = $process.Id; Path = $full }
            }
        }
    }
}

function Assert-PackageNotRunning([string]$TargetPath) {
    $running = @(Get-RunningNativePackageProcesses $TargetPath)
    if ($running.Count -gt 0) {
        $details = ($running | ForEach-Object { "{0} (PID {1})" -f $_.Name, $_.Id }) -join ", "
        throw "拒绝替换正在运行的原生包：$details。请先关闭 RifeLoveNative.exe；用户 data 会原样保留。"
    }
}

function Copy-FileChecked([string]$Source, [string]$Destination) {
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    Copy-Item -LiteralPath (Resolve-ExistingFile $Source $Source) -Destination $Destination -Force
}

function Read-Json([string]$Path) {
    try { return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json) }
    catch { throw "无法读取 JSON：$Path；$($_.Exception.Message)" }
}

function Read-CMakeCacheValue([string]$CachePath, [string]$Key) {
    $prefix = "${Key}:"
    $line = @(Get-Content -LiteralPath $CachePath | Where-Object { $_.StartsWith($prefix, [StringComparison]::Ordinal) }) | Select-Object -First 1
    if ($null -eq $line) { throw "CMakeCache.txt 缺少 $Key：$CachePath" }
    $separator = $line.IndexOf('=')
    if ($separator -lt 0) { throw "CMakeCache.txt 中的 $Key 格式无效：$CachePath" }
    return $line.Substring($separator + 1)
}

function Normalize-ComparablePath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    if ($full.Length -gt $root.Length) { $full = $full.TrimEnd('\', '/') }
    return $full
}

function Resolve-RepositoryPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return (Normalize-ComparablePath $Path) }
    return (Normalize-ComparablePath (Join-Path $repositoryRoot $Path))
}

function Get-RepositoryRelativePath([string]$Path) {
    $full = Normalize-ComparablePath $Path
    return [IO.Path]::GetRelativePath($repositoryRoot, $full).Replace('\', '/')
}

function Read-FfmpegSdkIdentity([string]$BuildPath, [string]$FfmpegPath) {
    $cachePath = Resolve-ExistingFile (Join-Path $BuildPath "CMakeCache.txt") "CMake 缓存"
    $cacheRootValue = Read-CMakeCacheValue $cachePath "RIFELOVE_FFMPEG_ROOT"
    if ([string]::IsNullOrWhiteSpace($cacheRootValue)) { throw "CMakeCache.txt 的 RIFELOVE_FFMPEG_ROOT 为空：$cachePath" }
    $cacheRoot = Resolve-RepositoryPath $cacheRootValue
    $selectedRoot = Normalize-ComparablePath $FfmpegPath
    if (-not [string]::Equals($cacheRoot, $selectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "打包拒绝：CMakeCache 的 RIFELOVE_FFMPEG_ROOT 与打包 FFmpeg 根目录不一致。CMake=$cacheRoot；打包=$selectedRoot"
    }
    $linkInputs = Assert-FfmpegBuildLinkIdentity -BuildDirectory $BuildPath -SdkRoot $selectedRoot
    foreach ($entry in @(@('RIFELOVE_FFMPEG_TOOL','ffmpeg.exe'), @('RIFELOVE_FFPROBE_TOOL','ffprobe.exe'))) {
        $cachedTool = Resolve-RepositoryPath (Read-CMakeCacheValue $cachePath $entry[0])
        $expectedTool = Normalize-ComparablePath (Join-Path $selectedRoot "bin/$($entry[1])")
        if (-not [string]::Equals($cachedTool,$expectedTool,[StringComparison]::OrdinalIgnoreCase)) {
            throw "打包拒绝：$($entry[0]) 未指向所选 SDK。请显式重新配置 FFmpeg/ffprobe 工具缓存。"
        }
    }

    $manifestValue = Read-CMakeCacheValue $cachePath "RIFELOVE_FFMPEG_SDK_MANIFEST"
    if ([string]::IsNullOrWhiteSpace($manifestValue)) {
        $normalizedRoot = $cacheRoot.Replace('\', '/').ToLowerInvariant()
        if ($normalizedRoot -match '(^|/)work/(codec-ffmpeg-build(-x86asm)?|nvenc-ffmpeg-8\.1\.3-n12\.2\.72\.0)/ffmpeg-install/?$') {
            $manifestPath = Join-Path $repositoryRoot "tools/development/ffmpeg-source-sdk.lock.json"
        } else {
            $manifestPath = Join-Path $repositoryRoot "tools/development/ffmpeg-sdk.lock.json"
        }
    } else {
        $manifestPath = Resolve-RepositoryPath $manifestValue
    }
    $manifestPath = Resolve-ExistingFile $manifestPath "FFmpeg SDK manifest"
    $manifestRelativePath = Get-RepositoryRelativePath $manifestPath
    $lock = Read-Json $manifestPath
    foreach ($property in @("nvencApiMajor", "nvencApiMinor", "nvencMinimumWindowsDriver")) {
        if ($null -eq $lock.PSObject.Properties[$property]) { throw "FFmpeg SDK manifest 缺少字段 $property：$manifestPath" }
    }
    if ($lock.nvencMinimumWindowsDriver -isnot [string]) { throw "FFmpeg SDK manifest 的最低驱动字段必须是字符串：$manifestPath" }
    $majorText = [string]$lock.nvencApiMajor
    $minorText = [string]$lock.nvencApiMinor
    if ($majorText -notmatch '^\d+$' -or $minorText -notmatch '^\d+$') { throw "FFmpeg SDK manifest 的 NVENC API 版本无效：$manifestPath" }
    $major = [int]$majorText
    $minor = [int]$minorText
    $minimumDriver = [string]$lock.nvencMinimumWindowsDriver

    $generatedConfigPath = Resolve-ExistingFile (Join-Path $BuildPath "generated/nvenc_build_config.hpp") "生成的 NVENC 配置"
    $generated = Get-Content -Raw -LiteralPath $generatedConfigPath
    $generatedMajor = [regex]::Match($generated, '(?m)^\s*#define\s+RIFELOVE_NVENC_API_MAJOR\s+(\d+)\s*$')
    $generatedMinor = [regex]::Match($generated, '(?m)^\s*#define\s+RIFELOVE_NVENC_API_MINOR\s+(\d+)\s*$')
    $generatedDriver = [regex]::Match($generated, '(?m)^\s*#define\s+RIFELOVE_NVENC_MINIMUM_WINDOWS_DRIVER\s+"([^"]*)"\s*$')
    if (-not $generatedMajor.Success -or -not $generatedMinor.Success -or -not $generatedDriver.Success) {
        throw "生成的 NVENC 配置缺少可核对的 API/最低驱动字段：$generatedConfigPath"
    }
    if ([int]$generatedMajor.Groups[1].Value -ne $major -or [int]$generatedMinor.Groups[1].Value -ne $minor -or
        $generatedDriver.Groups[1].Value -cne $minimumDriver) {
        throw "打包拒绝：生成的 NVENC 配置与 FFmpeg SDK manifest 不一致：$manifestPath"
    }
    [pscustomobject]@{
        manifestRelativePath = $manifestRelativePath
        manifestSha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        nvencApiMajor = $major
        nvencApiMinor = $minor
        nvencMinimumWindowsDriver = $minimumDriver
        root = $selectedRoot
        configuration = (Read-FfmpegBuildConfiguration $selectedRoot)
        linkInputs = $linkInputs
    }
}

function Get-DescriptorHash([object]$Descriptor, [string]$Field, [string]$Label) {
    $value = $Descriptor.$Field
    if ([string]::IsNullOrWhiteSpace([string]$value) -or [string]$value -notmatch '^[0-9a-fA-F]{64}$') {
        throw "$Label 缺少有效 SHA-256：$Field"
    }
    return ([string]$value).ToLowerInvariant()
}

function Validate-ModelPack([string]$Directory, [string]$DestinationName, [string]$ExpectedFamily, [bool]$Legacy) {
    Assert-NoLinkTree $Directory
    $descriptorPath = Join-Path $Directory "model.json"
    $descriptor = Read-Json $descriptorPath
    $id = [string]$descriptor.id
    if ($id -notmatch '^[A-Za-z0-9._-]{1,128}$') { throw "模型 ID 不安全：$id" }
    if ([string]$descriptor.family -cne $ExpectedFamily) { throw "模型族不符合预期：$Directory" }
    if ([string]$descriptor.modelFile -cne "model.onnx") { throw "模型必须使用 model.onnx：$Directory" }
    $graph = Resolve-ExistingFile (Join-Path $Directory "model.onnx") "模型图"
    $graphHash = (Get-FileHash -LiteralPath $graph -Algorithm SHA256).Hash.ToLowerInvariant()
    $declaredHash = Get-DescriptorHash $descriptor "sha256" "模型"
    if ($graphHash -cne $declaredHash) { throw "模型 SHA-256 不匹配：$Directory" }
    $licenseFile = if ($Legacy) { [string]$descriptor.licenseFile } else { [string]$descriptor.license.file }
    if ([string]::IsNullOrWhiteSpace($licenseFile) -or [IO.Path]::GetFileName($licenseFile) -cne $licenseFile) { throw "模型许可文件名不安全：$Directory" }
    $licensePath = Resolve-ExistingFile (Join-Path $Directory $licenseFile) "模型许可"
    if (-not $Legacy) {
        $licenseHash = Get-DescriptorHash $descriptor.license "sha256" "模型许可"
        if ((Get-FileHash -LiteralPath $licensePath -Algorithm SHA256).Hash -ine $licenseHash) {
            throw "模型许可 SHA-256 不匹配：$Directory"
        }
        $provenance = [string]$descriptor.provenance.manifestFile
        $validation = [string]$descriptor.validation.reportFile
        if ([IO.Path]::GetFileName($provenance) -cne $provenance -or [IO.Path]::GetFileName($validation) -cne $validation) { throw "模型侧车文件名不安全：$Directory" }
        foreach ($item in @(
            [pscustomobject]@{ Path = Join-Path $Directory $provenance; Hash = [string]$descriptor.provenance.manifestSha256; Label = "来源清单" },
            [pscustomobject]@{ Path = Join-Path $Directory $validation; Hash = [string]$descriptor.validation.reportSha256; Label = "验证报告" }
        )) {
            $file = Resolve-ExistingFile $item.Path $item.Label
            $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actual -cne $item.Hash.ToLowerInvariant()) { throw "$($item.Label) SHA-256 不匹配：$Directory" }
        }
    } else {
        foreach ($name in @([string]$descriptor.validationFile, [string]$descriptor.sourceManifestFile)) {
            if ([string]::IsNullOrWhiteSpace($name)) { throw "旧模型缺少验证/来源侧车文件：$Directory" }
            Resolve-ExistingFile (Join-Path $Directory $name) "旧模型侧车" | Out-Null
        }
    }
    if ($DestinationName -ceq 'nafnet-gopro-deblur-sdr8-fp32-eval') {
        Assert-NativeDeblurModelIdentity -Descriptor $descriptor -GraphHash $graphHash -Directory $Directory
    }
    [pscustomobject]@{
        source = $Directory; destination = $DestinationName; descriptor = $descriptor; id = $id; family = [string]$descriptor.family;
        graphHash = $graphHash; licenseFile = $licenseFile; licensePath = $licensePath; legacy = $Legacy
    }
}

function Get-NativeFullModelPacks(
    [object]$Selection, [string]$RifeDirectory, [string]$SuperResolutionDirectory,
    [string]$TemporalDirectory, [string]$CompressionDirectory, [string]$DeblurDirectory = '') {
    Validate-ModelPack (Resolve-ExistingDirectory $RifeDirectory "RIFE 4.25 v3") "rife-4.25-fp32-sdr8-v3" "rife" $false
    if ($Selection.includeSuperResolution) {
        Validate-ModelPack (Resolve-ExistingDirectory $SuperResolutionDirectory "Real-ESRGAN x2plus v2") "realesrgan-x2plus-sdr8-v2" "video-restoration" $false
    }
    if ($Selection.includeTemporalRestoration) {
        Validate-ModelPack (Resolve-ExistingDirectory $TemporalDirectory "FastDVDnet 时序降噪评估包") "fastdvdnet-clipped-five-frame-sdr8-eval" "video-restoration" $false
    }
    if ($Selection.includeCompressionRestoration) {
        Validate-ModelPack (Resolve-ExistingDirectory $CompressionDirectory "STDF-R3 压缩修复评估包") "stdf-r3-y7-sdr8-local-eval" "video-restoration" $false
    }
    if ($Selection.includeDeblurRestoration) {
        Validate-ModelPack (Resolve-ExistingDirectory $DeblurDirectory "NAFNet 去模糊本机评估包") "nafnet-gopro-deblur-sdr8-fp32-eval" "video-restoration" $false
    }
}

function Find-Objdump {
    $commands = @("llvm-objdump.exe", "llvm-objdump", "dumpbin.exe", "dumpbin")
    foreach ($name in $commands) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    throw "需要 llvm-objdump 或 dumpbin 检查 DLL 导入，但当前 PATH 没有可用工具。"
}

function Get-ImportedDlls([string]$Binary, [string]$Objdump) {
    $lines = @(& $Objdump -p $Binary 2>$null)
    $names = @($lines | ForEach-Object {
        if ($_ -match 'DLL Name:\s*(\S+)') { $matches[1].ToLowerInvariant() }
        elseif ($_ -match '^\s*([A-Za-z0-9_.-]+\.dll)\s*$') { $matches[1].ToLowerInvariant() }
    } | Sort-Object -Unique)
    if ($names.Count -eq 0) { throw "无法读取 DLL 导入：$Binary" }
    return $names
}

function Test-ModelGraphHash([string]$Directory, [string]$ExpectedHash) {
    try {
        $graph = Resolve-ExistingFile (Join-Path $Directory "model.onnx") "用户模型图"
        return ((Get-FileHash -LiteralPath $graph -Algorithm SHA256).Hash -ieq $ExpectedHash)
    } catch {
        return $false
    }
}

function Find-LegacyModelReference([string]$OldRoot, [string]$ExpectedHash) {
    $result = [ordered]@{ referenced = $false; evidence = @(); token = "models/cache/rife-4.25-fp32-sdr8-v2"; sha256 = $ExpectedHash }
    if ([string]::IsNullOrWhiteSpace($OldRoot) -or -not (Test-Path -LiteralPath $OldRoot -PathType Container)) { return [pscustomobject]$result }
    $oldData = Join-Path $OldRoot "data"
    if (-not (Test-Path -LiteralPath $oldData -PathType Container)) { return [pscustomobject]$result }
    Assert-NoLinkTree $oldData
    $extensions = @(".sqlite", ".sqlite3", ".db", ".json", ".jsonl", ".manifest", ".txt", ".log", ".cfg", ".ini", ".yaml", ".yml")
    foreach ($file in @(Get-PackageTreeItems $oldData | Where-Object {
        -not $_.PSIsContainer -and $extensions -contains $_.Extension.ToLowerInvariant() -and $_.Length -le 67108864
    })) {
        try {
            $text = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes($file.FullName))
            $normalized = [regex]::Replace($text.ToLowerInvariant().Replace('\','/'), '/+', '/')
            $pathMatch = $normalized.Contains("models/cache/rife-4.25-fp32-sdr8-v2")
            $identityMatch = $normalized.Contains($ExpectedHash.ToLowerInvariant()) -and
                $normalized.Contains("rife-4.25-fp32-sdr8") -and
                ($normalized.Contains('"version":"4.25"') -or $normalized.Contains('"version": "4.25"') -or
                 $normalized.Contains('"expectedmodelversion":"4.25"') -or $normalized.Contains('"expectedmodelversion": "4.25"'))
            if ($pathMatch -and $identityMatch) {
                $result.referenced = $true
                $result.evidence += [pscustomobject]@{
                    file = $file.FullName.Substring($oldData.Length).TrimStart('\','/').Replace('\','/')
                    pathToken = $result.token
                    sha256 = $ExpectedHash
                }
            }
        } catch { }
    }
    return [pscustomobject]$result
}

function Resolve-NativeLegacyModelMigration(
    [string]$OldRoot, [string]$LegacyDirectory,
    [string]$ExpectedHash = '8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3') {
    # This pinned historical identity lets clean packages scan references
    # without requiring (or opening) any unused v2 descriptor or graph.
    $reference = Find-LegacyModelReference $OldRoot $ExpectedHash
    $pack = $null
    if ($reference.referenced) {
        $pack = Validate-ModelPack (Resolve-ExistingDirectory $LegacyDirectory "RIFE 4.25 旧版") "rife-4.25-fp32-sdr8-v2" "rife" $true
        if ($pack.graphHash -ine $ExpectedHash -or [string]$pack.id -cne 'rife-4.25-fp32-sdr8' -or [string]$pack.descriptor.version -cne '4.25') {
            throw 'Referenced legacy RIFE model identity does not match the pinned recovery identity.'
        }
    }
    [pscustomobject]@{ reference = $reference; hash = $ExpectedHash; pack = $pack }
}

function Test-GeneratedLegacyModelDirectory([string]$OldRoot, [string]$Directory, [string]$ExpectedHash) {
    if (-not (Test-ModelGraphHash $Directory $ExpectedHash)) { return $false }
    $manifestPath = Join-Path $OldRoot "PACKAGE-MANIFEST.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $false }
    try {
        $manifest = Read-Json $manifestPath
        foreach ($model in @($manifest.models)) {
            if ([bool]$model.legacy -and ([string]$model.sha256).ToLowerInvariant() -eq $ExpectedHash.ToLowerInvariant()) { return $true }
        }
    } catch { }
    return $false
}

function Copy-UserState([string]$OldRoot, [string]$NewRoot, [string[]]$BundledModelDirectories, [hashtable]$BundledModelAliases, [string]$LegacyModelHash) {
    if (-not (Test-Path -LiteralPath $OldRoot -PathType Container)) { return }
    Assert-PackageNotRunning $OldRoot
    $oldData = Join-Path $OldRoot "data"
    if (Test-Path -LiteralPath $oldData -PathType Container) {
        Assert-NoLinkTree $oldData
        $newData = Join-Path $NewRoot "data"
        New-Item -ItemType Directory -Path $newData -Force | Out-Null
        foreach ($item in (Get-PackageTreeItems $oldData | Where-Object { -not $_.PSIsContainer })) {
            $relative = $item.FullName.Substring($oldData.Length).TrimStart('\','/')
            Copy-FileChecked $item.FullName (Join-Path $newData $relative)
        }
    }
    $oldModels = Join-Path $OldRoot "models"
    if (-not (Test-Path -LiteralPath $oldModels -PathType Container)) { return }
    Assert-NoLinkTree $oldModels
    $newModels = Join-Path $NewRoot "models"
    foreach ($directory in (Get-ChildItem -LiteralPath $oldModels -Directory -Force)) {
        if ($BundledModelDirectories -contains $directory.Name) { continue }
        # Older revisions of this script renamed the checkpoint-required v2
        # directory to `-legacy`. If its graph hash is the bundled v2 hash,
        # fold the generated alias into the canonical basename below. A
        # user-owned directory with the same name but different bytes remains
        # user data and is copied intact.
        if ($BundledModelAliases.ContainsKey($directory.Name) -and
            (Test-ModelGraphHash $directory.FullName ([string]$BundledModelAliases[$directory.Name]))) { continue }
        if (-not $BundledModelDirectories.Contains("rife-4.25-fp32-sdr8-v2") -and
            $directory.Name -in @("rife-4.25-fp32-sdr8-v2", "rife-4.25-fp32-sdr8-v2-legacy") -and
            (Test-GeneratedLegacyModelDirectory $OldRoot $directory.FullName $LegacyModelHash)) { continue }
        if (Test-Path -LiteralPath (Join-Path $newModels $directory.Name)) { continue }
        Copy-Item -LiteralPath $directory.FullName -Destination (Join-Path $newModels $directory.Name) -Recurse
    }
    $oldDefault = Join-Path $oldModels "default-model.txt"
    if (Test-Path -LiteralPath $oldDefault -PathType Leaf) {
        $value = (Get-Content -LiteralPath $oldDefault -Raw).Trim()
        if ($value -match '^[A-Za-z0-9._-]{1,128}$' -and
            -not $BundledModelAliases.ContainsKey($value) -and
            (Test-Path -LiteralPath (Join-Path $newModels $value) -PathType Container)) {
            Set-Content -LiteralPath (Join-Path $newModels "default-model.txt") -Value $value -Encoding utf8NoBOM
        }
    }
}

function Invoke-StartupSmoke([string]$PackageRoot) {
    $exe = Join-Path $PackageRoot "RifeLoveNative.exe"
    $pathBefore = $env:PATH
    try {
        $env:PATH = "$PackageRoot;$PackageRoot/runtime;$env:SystemRoot/System32;$pathBefore"
        $process = Start-Process -FilePath $exe -WorkingDirectory $PackageRoot -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 3
        if ($process.HasExited) { throw "原生桌面启动失败，退出码：$($process.ExitCode)" }
        Stop-Process -Id $process.Id -Force
        $null = $process.WaitForExit(5000)
    } finally {
        $env:PATH = $pathBefore
    }
}

function Write-Sha256Manifest([string]$PackageRoot) {
    $lines = @("RifeLove native local bundle SHA-256")
    foreach ($file in @(Get-PackageTreeItems $PackageRoot | Where-Object {
        -not $_.PSIsContainer -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and
        $_.Name -cne "SHA256SUMS.txt" -and -not $_.FullName.StartsWith((Join-Path $PackageRoot "data") + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($PackageRoot.Length).TrimStart('\','/').Replace('\','/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines += "$hash  $relative"
    }
    [IO.File]::WriteAllLines((Join-Path $PackageRoot "SHA256SUMS.txt"), [string[]]$lines, [Text.UTF8Encoding]::new($false))
}

$target = Assert-InOutputRoot $OutputDirectory
if ($freshModelSelection -and (Test-Path -LiteralPath $target)) {
    throw 'Selected model bundle requires a fresh output directory; existing model, queue and recovery data is never copied or removed.'
}
$packageLeaf = Split-Path -Path $target -Leaf
if ($packageLeaf -notmatch '^RifeLoveNative-local(?:$|[-.])') { throw "为避免与 Qt 产物冲突，原生包目录必须以 RifeLoveNative-local 开头：$target" }
if ($CudaOnly -and $packageLeaf -notmatch '(?i)(^|-)cuda-only($|-)') {
    throw "CUDA-only 输出目录必须包含 -cuda-only，以免与完整 TensorRT 包混淆：$target"
}
$packageParent = Split-Path -Path $target -Parent
$staging = Join-Path $packageParent (".{0}.staging-{1}" -f $packageLeaf, [Guid]::NewGuid().ToString("N"))
$backup = Join-Path $packageParent (".{0}.previous-{1}" -f $packageLeaf, [Guid]::NewGuid().ToString("N"))

$build = Resolve-ExistingDirectory $BuildDirectory "原生构建"
$dependencySelection = Resolve-NativePackageBuildDependencies -BuildDirectory $build -InferenceRuntimeDirectory $InferenceRuntimeDirectory -MinGWBinDirectory $MinGWBinDirectory
$InferenceRuntimeDirectory = $dependencySelection.inferenceRuntimeDirectory
$MinGWBinDirectory = $dependencySelection.minGWBinDirectory
$ffmpeg = Resolve-ExistingDirectory $FfmpegRoot "FFmpeg"
$runtime = Resolve-ExistingDirectory $InferenceRuntimeDirectory "ONNX/TensorRT/CUDA/cuDNN 运行库 [$($dependencySelection.inferenceRuntimeSource)；可用 -InferenceRuntimeDirectory 或 RIFELOVE_TENSORRT_RUNTIME_DIR 覆盖]"
$mingw = Resolve-ExistingDirectory $MinGWBinDirectory "MinGW 运行库 [$($dependencySelection.minGWSource)；可用 -MinGWBinDirectory 或 RIFELOVE_MINGW_BIN 覆盖]"
$nativeExe = Resolve-ExistingFile (Join-Path $build "RifeLoveNative.exe") "非 Qt 原生 EXE"
$ffmpegSdk = Read-FfmpegSdkIdentity $build $ffmpeg
Assert-NoLinkTree $ffmpeg
Assert-NoLinkTree $runtime
Assert-NoLinkTree $mingw
$compilerIdentity = Get-NativeCompilerRuntimeIdentity $dependencySelection
Assert-NativeInferenceRuntimeIdentity $dependencySelection

$ffmpegFiles = @(
    "avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swresample-6.dll", "swscale-9.dll",
    "avfilter-11.dll", "avdevice-62.dll", "libaom.dll", "libkvazaar.dll", "libopenh264-8.dll"
)
foreach ($name in $ffmpegFiles) { Resolve-ExistingFile (Join-Path $ffmpeg "bin/$name") "FFmpeg 运行库" | Out-Null }
$ffmpegRuntimeHashes = Assert-FfmpegRuntimeIdentity -SdkRoot $ffmpeg -RuntimeDirectory $build -FileNames $ffmpegFiles
$ffmpegX86asmDescription = if ($ffmpegSdk.configuration.x86asmEnabled) { "enabled; end-to-end throughput remains workload-specific" } else { "disabled; CPU throughput remains unqualified" }
$runtimeSourceFiles = @(Get-ChildItem -LiteralPath $runtime -File -Filter "*.dll" | Sort-Object Name)
$runtimeSelection = Resolve-NativeInferenceRuntimeSelection -AvailableFileNames @($runtimeSourceFiles | ForEach-Object Name) -CudaOnly:$CudaOnly
$runtimeFiles = @($runtimeSourceFiles | Where-Object { $runtimeSelection.files -contains $_.Name })
$requiredRuntimeFiles = @($runtimeSelection.requiredFiles)
$versionFile = Join-Path $runtime "tensorrt-runtime.version"
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) { throw "缺少 TensorRT 运行库版本/校验侧车：$versionFile" }
$versionLines = @(Get-Content -LiteralPath $versionFile | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($versionLines.Count -lt 2 -or $versionLines[0] -notmatch '^\d+\.\d+\.\d+\.\d+$' -or $versionLines[1] -notmatch '^[0-9a-fA-F]{64}$') {
    throw "TensorRT 运行库版本侧车格式无效，必须包含版本和来源 SHA-256：$versionFile"
}
$runtimeSourceDescription = if ($runtimeSelection.includesTensorRt) { 'local TensorRT/ONNX Runtime CUDA SDK' } else { 'local ONNX Runtime CUDA SDK' }
$mingwFiles = @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")
foreach ($name in $mingwFiles) {
    Resolve-ExistingFile (Join-Path $mingw $name) "MinGW 运行库" | Out-Null
    $sdkCompilerRuntime = Resolve-ExistingFile (Join-Path $ffmpeg "bin/$name") "FFmpeg SDK 的 MinGW 运行库"
    if ((Get-FileHash -LiteralPath $sdkCompilerRuntime -Algorithm SHA256).Hash.ToLowerInvariant() -cne $compilerIdentity.runtimeSha256[$name]) {
        throw "FFmpeg SDK 的 $name 与原生构建编译器不一致；请使用同一经过核验的工具链重建，拒绝混用 DLL。"
    }
}
$msvcFiles = @("MSVCP140.dll", "MSVCP140_1.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll")
foreach ($name in $msvcFiles) { Resolve-ExistingFile (Join-Path $env:SystemRoot "System32/$name") "MSVC 运行库" | Out-Null }
if ($ValidateDependenciesOnly) {
    [ordered]@{
        result = 'passed'; purpose = 'dependency-preflight-only'; buildDirectory = $build
        inferenceRuntimeDirectory = $runtime; inferenceRuntimeSource = $dependencySelection.inferenceRuntimeSource
        inferenceRuntimeProfile = $runtimeSelection.profile; inferenceRuntimeFiles = @($runtimeFiles | ForEach-Object Name)
        minGWBinDirectory = $mingw; minGWSource = $dependencySelection.minGWSource
        compiler = $dependencySelection.compiler; compilerIdentity = $compilerIdentity
        ffmpegLinkInputs = $ffmpegSdk.linkInputs
        ffmpegX86asm = $ffmpegSdk.configuration.x86asmEnabled; ffmpegRuntimeSha256 = $ffmpegRuntimeHashes
        packageCreated = $false; launchedExe = $false
    } | ConvertTo-Json -Depth 8
    return
}

$legacyMigration = Resolve-NativeLegacyModelMigration $(if ($freshModelSelection) { '' } else { $target }) $LegacyRifeModelDirectory
$legacyModelHash = $legacyMigration.hash
$legacyReference = $legacyMigration.reference
$includeLegacyModel = [bool]$legacyReference.referenced

$packs = @(
    Get-NativeFullModelPacks $modelSelection $RifeModelDirectory $SuperResolutionModelDirectory $TemporalRestorationModelDirectory $CompressionRestorationModelDirectory $DeblurModelDirectory
    if ($includeLegacyModel) { $legacyMigration.pack }
)
$bundledModelDirectories = @($packs | ForEach-Object { $_.destination })
$bundledModelAliases = @{}
if ($includeLegacyModel) { $bundledModelAliases["rife-4.25-fp32-sdr8-v2-legacy"] = $legacyModelHash }

$objdump = Find-Objdump
$nativeImports = Get-ImportedDlls $nativeExe $objdump
if (@($nativeImports | Where-Object { $_ -match 'qt|qml' }).Count -gt 0) { throw "原生 EXE 导入了 Qt/QML，拒绝生成本机包：$($nativeImports -join ', ')" }
$requiredNativeImports = @("avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swscale-9.dll", "libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")
foreach ($name in $requiredNativeImports) { if ($nativeImports -notcontains $name) { throw "原生 EXE 缺少预期导入：$name" } }

if (Test-Path -LiteralPath $target) {
    $targetItem = Get-Item -LiteralPath $target -Force
    if ($targetItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "拒绝替换链接形式的目标包：$target" }
    Assert-PackageNotRunning $target
    if (-not $modelSelection.includeDeblurRestoration -and
        (Test-Path -LiteralPath (Join-Path $target 'models/nafnet-gopro-deblur-sdr8-fp32-eval') -PathType Container)) {
        throw 'Existing package contains NAFNet. Pass -IncludeDeblurRestoration to retain it, or choose a new output directory; existing task/model state will not be silently changed.'
    }
}
if (Test-Path -LiteralPath $staging) { throw "随机 staging 路径已存在：$staging" }
New-Item -ItemType Directory -Path $staging | Out-Null
$promoted = $false
try {
    # Initialize the Normal-mode contract before preserving any existing data.
    # Full refresh never inherits or creates a portable opt-in marker.
    New-Item -ItemType Directory -Path (Join-Path $staging "data") | Out-Null
    $dataMode = Initialize-NativePackageDataMode -PackageRoot $staging
    Copy-FileChecked $nativeExe (Join-Path $staging "RifeLoveNative.exe")
    foreach ($name in $ffmpegFiles) { Copy-FileChecked (Join-Path $ffmpeg "bin/$name") (Join-Path $staging $name) }
    foreach ($name in $mingwFiles) { Copy-FileChecked (Join-Path $mingw $name) (Join-Path $staging $name) }
    foreach ($name in $msvcFiles) { Copy-FileChecked (Join-Path $env:SystemRoot "System32/$name") (Join-Path $staging $name) }

    $runtimeTarget = Join-Path $staging "runtime"
    New-Item -ItemType Directory -Path $runtimeTarget | Out-Null
    foreach ($file in $runtimeFiles) { Copy-FileChecked $file.FullName (Join-Path $runtimeTarget $file.Name) }
    if ($runtimeSelection.includesTensorRt) { Copy-FileChecked $versionFile (Join-Path $runtimeTarget "tensorrt-runtime.version") }

    $modelsTarget = Join-Path $staging "models"
    New-Item -ItemType Directory -Path $modelsTarget | Out-Null
    foreach ($pack in $packs) {
        $destination = Join-Path $modelsTarget $pack.destination
        Copy-Item -LiteralPath $pack.source -Destination $destination -Recurse
        Assert-NoLinkTree $destination
    }
    Set-Content -LiteralPath (Join-Path $modelsTarget "default-model.txt") -Value "rife-4.25-fp32-sdr8-v3" -Encoding utf8NoBOM
    if ($includeLegacyModel) {
        $legacyMetadata = [ordered]@{
            schemaVersion = 1
            hiddenFromDefaultSelection = @("rife-4.25-fp32-sdr8-v2")
            entries = @([ordered]@{
                directory = "models/rife-4.25-fp32-sdr8-v2"
                canonicalBasename = "rife-4.25-fp32-sdr8-v2"
                sha256 = $legacyModelHash
                reason = "migrated local queue/checkpoint data contains this exact path and matching model identity"
                defaultSelectable = $false
            })
        }
        $legacyMetadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $modelsTarget "legacy-models.json") -Encoding utf8NoBOM
    }

    $configTarget = Join-Path $staging "config"
    New-Item -ItemType Directory -Path $configTarget | Out-Null
    $defaults = [ordered]@{
        schemaVersion = 1; product = "RifeLoveNative"; qt = $false; cliIncluded = $false; oss = $false
        defaultModelDirectory = "models/rife-4.25-fp32-sdr8-v3"; legacyModelDirectory = if ($includeLegacyModel) { "models/rife-4.25-fp32-sdr8-v2" } else { $null }
        restorationModelDirectory = if ($modelSelection.includeSuperResolution) { "models/realesrgan-x2plus-sdr8-v2" } else { $null }; temporalRestorationModelDirectory = if ($modelSelection.includeTemporalRestoration) { "models/fastdvdnet-clipped-five-frame-sdr8-eval" } else { $null }; temporalRestorationDefaultEnabled = $false; temporalRestorationMode = if ($modelSelection.includeTemporalRestoration) { "experimental-optional" } else { "not-bundled" }; compressionRestorationModelDirectory = if ($modelSelection.includeCompressionRestoration) { "models/stdf-r3-y7-sdr8-local-eval" } else { $null }; compressionRestorationDefaultEnabled = $false; compressionRestorationTiled = $false; compressionRestorationStrength = 0.75; compressionRestorationMode = if ($modelSelection.includeCompressionRestoration) { "experimental-local-evaluation" } else { "not-bundled" }; deblurRestorationModelDirectory = if ($modelSelection.includeDeblurRestoration) { "models/nafnet-gopro-deblur-sdr8-fp32-eval" } else { $null }; deblurRestorationDefaultEnabled = $false; deblurRestorationMode = if ($modelSelection.includeDeblurRestoration) { "experimental-local-evaluation" } else { "not-bundled" }; targetRate = "60/1"; encoder = "auto-h264-nvenc-with-software-fallback"
        ffmpeg = "Isolated FFmpeg 8.1.3 with NVENC, OpenH264, Kvazaar and libaom; x86asm $ffmpegX86asmDescription"
        legacyModelIncluded = $includeLegacyModel
        modelBundleMode = $modelSelection.mode
    }
    Set-NativePackageDataModeMetadata -Metadata $defaults -DataMode $dataMode
    $defaults | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $configTarget "defaults.json") -Encoding utf8NoBOM
    $licenseTarget = Join-Path $staging "licenses"
    New-Item -ItemType Directory -Path $licenseTarget | Out-Null
    Copy-FileChecked (Join-Path $repositoryRoot "LICENSE") (Join-Path $licenseTarget "RifeLove-MIT.txt")
    $ffmpegSourceRoot = Join-Path $CodecLicenseRoot "ffmpeg-8.1.3"
    Copy-FileChecked (Resolve-CodecLicense $CodecLicenseRoot "ffmpeg-8.1.3/LICENSE.md" "FFmpeg 许可") (Join-Path $licenseTarget "FFmpeg-LICENSE.md")
    Copy-FileChecked (Resolve-CodecLicense $CodecLicenseRoot "ffmpeg-8.1.3/COPYING.LGPLv2.1" "FFmpeg LGPL 许可") (Join-Path $licenseTarget "FFmpeg-LGPL-2.1.txt")
    Copy-FileChecked (Find-CodecSourceLicense $CodecDepsRoot "openh264/src" "OpenH264") (Join-Path $licenseTarget "OpenH264-LICENSE.txt")
    Copy-FileChecked (Find-CodecSourceLicense $CodecDepsRoot "kvazaar/src" "Kvazaar") (Join-Path $licenseTarget "Kvazaar-LICENSE.txt")
    Copy-FileChecked (Find-CodecSourceLicense $CodecDepsRoot "aom/src" "libaom") (Join-Path $licenseTarget "libaom-LICENSE.txt")
    $nvCodecNotice = @(
        "This local build uses NVIDIA nv-codec-headers n12.2.72.0 for NVENC/NVDEC integration.",
        "The source archive SHA-256 is recorded in tools/development/ffmpeg-source-sdk.lock.json; this package records actual configuration and runtime DLL hashes in PACKAGE-MANIFEST.json.",
        "Review NVIDIA Video Codec SDK terms before any redistribution."
    ) -join [Environment]::NewLine
    Set-Content -LiteralPath (Join-Path $licenseTarget "NVIDIA-nv-codec-headers-notice.txt") -Value $nvCodecNotice -Encoding utf8NoBOM
    $onnxRoot = Split-Path -Parent $runtime
    Copy-FileChecked (Join-Path $onnxRoot "LICENSE") (Join-Path $licenseTarget "ONNX-Runtime-MIT.txt")
    Copy-FileChecked (Join-Path $onnxRoot "ThirdPartyNotices.txt") (Join-Path $licenseTarget "ONNX-Runtime-ThirdPartyNotices.txt")
    $tensorRtRoot = Split-Path (Split-Path $runtime -Parent) -Parent
    if ($runtimeSelection.includesTensorRt) {
        Copy-FileChecked (Join-Path $tensorRtRoot "licenses/TensorRT-10.9-LICENSE.txt") (Join-Path $licenseTarget "TensorRT-10.9-LICENSE.txt")
    }
    foreach ($item in @(Get-ChildItem -LiteralPath (Join-Path $tensorRtRoot "onnx-cuda-12.8") -Recurse -File -Filter "License.txt")) {
        $safe = ($item.Directory.Name -replace '[^A-Za-z0-9._-]','_')
        Copy-FileChecked $item.FullName (Join-Path $licenseTarget ("NVIDIA-{0}-License.txt" -f $safe))
    }
    Copy-FileChecked (Join-Path $mingw "../licenses/gcc/COPYING.RUNTIME") (Join-Path $licenseTarget "GCC-Runtime.txt")
    Copy-FileChecked (Join-Path $mingw "../licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt") (Join-Path $licenseTarget "MinGW-w64-Runtime.txt")
    Copy-FileChecked (Join-Path $mingw "../licenses/winpthreads/COPYING") (Join-Path $licenseTarget "winpthreads.txt")
    $msvcNotice = @(
        "The native desktop package includes the following Microsoft Visual C++ runtime DLLs copied from the local Windows System32 directory:",
        ($msvcFiles -join ", "),
        "This package is a local workstation bundle. Review Microsoft's Visual C++ Redistributable terms before redistribution."
    ) -join [Environment]::NewLine
    Set-Content -LiteralPath (Join-Path $licenseTarget "Microsoft-VCRuntime-notice.txt") -Value $msvcNotice -Encoding utf8NoBOM
    foreach ($pack in $packs) {
        Copy-FileChecked $pack.licensePath (Join-Path $licenseTarget ("Model-{0}-{1}" -f $pack.destination, [IO.Path]::GetFileName($pack.licensePath)))
    }

    $imports = [ordered]@{}
    $imports.native = $nativeImports
    $imports.ffmpeg = [ordered]@{}
    foreach ($name in $ffmpegFiles) { $imports.ffmpeg[$name] = Get-ImportedDlls (Join-Path $staging $name) $objdump }
    $imports.inference = [ordered]@{}
    foreach ($file in $runtimeFiles) { $imports.inference[$file.Name] = Get-ImportedDlls (Join-Path $runtimeTarget $file.Name) $objdump }
    $imports | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $staging "dependency-imports.json") -Encoding utf8NoBOM

    $manifest = [ordered]@{
        schemaVersion = 1; packageKind = "local-native-windows-x64"; packageName = $packageLeaf
        generatedAtUtc = [DateTimeOffset]::UtcNow.ToString("o"); executable = "RifeLoveNative.exe"
        executableSha256 = (Get-FileHash -LiteralPath (Join-Path $staging "RifeLoveNative.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
        qtIncluded = $false; cliIncluded = $false; ossEnabled = $false; userMediaIncluded = $false; userDataIncluded = $false
        compilerRuntime = $compilerIdentity
        modelBundleMode = $modelSelection.mode
        ffmpeg = [ordered]@{ source = "local SDK selected by the native build"; root = "local-sdk:ffmpeg"; binaries = $ffmpegFiles; runtimeSha256 = $ffmpegRuntimeHashes; configuration = $ffmpegSdk.configuration; linkInputs = $ffmpegSdk.linkInputs; nvenc = "integrated in isolated SDK; H.264 NVENC and software fallback require hardware/workload validation"; x86asm = $ffmpegX86asmDescription; sdkManifest = [ordered]@{ path = $ffmpegSdk.manifestRelativePath; sha256 = $ffmpegSdk.manifestSha256; nvencApiMajor = $ffmpegSdk.nvencApiMajor; nvencApiMinor = $ffmpegSdk.nvencApiMinor; nvencMinimumWindowsDriver = $ffmpegSdk.nvencMinimumWindowsDriver } }
        inferenceRuntime = [ordered]@{ profile = $runtimeSelection.profile; source = $runtimeSourceDescription; files = @($runtimeFiles | ForEach-Object Name); requiredFiles = $requiredRuntimeFiles; tensorRtVersion = if ($runtimeSelection.includesTensorRt) { $versionLines[0] } else { $null }; sourceSha256 = if ($runtimeSelection.includesTensorRt) { $versionLines[1] } else { $null }; providers = $runtimeSelection.providers }
        models = @($packs | ForEach-Object { [ordered]@{ directory = "models/$($_.destination)"; id = $_.id; family = $_.family; version = [string]$_.descriptor.version; sha256 = $_.graphHash; licenseId = if ($_.descriptor.license -is [string]) { [string]$_.descriptor.license } else { [string]$_.descriptor.license.id }; licensePath = "licenses/Model-$($_.destination)-$($_.licenseFile)"; provenancePath = if ($_.legacy) { $null } else { "models/$($_.destination)/$($_.descriptor.provenance.manifestFile)" }; validatedBackends = @($_.descriptor.validation.validatedBackends); legacy = $_.legacy; hiddenFromDefaultSelection = $_.legacy; canonicalBasename = $_.destination; role = if ($_.destination -eq "fastdvdnet-clipped-five-frame-sdr8-eval") { "experimental-optional-temporal-denoise" } elseif ($_.destination -eq "stdf-r3-y7-sdr8-local-eval") { "experimental-optional-compression-repair" } elseif ($_.destination -eq "nafnet-gopro-deblur-sdr8-fp32-eval") { "experimental-optional-deblur" } elseif ($_.destination -eq "realesrgan-x2plus-sdr8-v2") { "optional-super-resolution" } else { "default-interpolation" }; redistribution = if ($_.legacy) { "local-legacy-required-by-verified-migration-reference" } else { "local-only-until-license-review" } } })
        migration = [ordered]@{ legacyModelIncluded = $includeLegacyModel; legacyModelPathToken = $legacyReference.token; legacyModelSha256 = $legacyModelHash; evidence = @($legacyReference.evidence); cleanPackageOmitsUnreferencedLegacy = $true }
        defaults = "config/defaults.json"; updatePolicy = "transactional; refuse running target; preserve data"; storage = "local-only; no Alibaba OSS"
    }
    Set-NativePackageDataModeMetadata -Metadata $manifest -DataMode $dataMode -Manifest
    $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $staging "PACKAGE-MANIFEST.json") -Encoding utf8NoBOM
    $includedModelsDescription = (@($packs | ForEach-Object { 'models/' + $_.destination }) -join '；')
    $experimentalModelsDescription = if ($modelSelection.includeTemporalRestoration -or $modelSelection.includeCompressionRestoration) {
        '本包选中的 FastDVDnet / STDF-R3 为本地实验评估模型，默认关闭；仅在用户明确选择且满足对应 model.json 输入/尺寸/色彩契约时启用，不代表通用压缩修复或 Topaz 级质量。'
    } else { '本基础包未捆绑 FastDVDnet 或 STDF-R3，不要求它们的权重、许可证或侧车文件。' }
    $deblurDescription = if ($modelSelection.includeDeblurRestoration) {
        'NAFNet GoPro 去模糊仅供本机 SDR8 BT.709 实验评估，默认关闭，需在模型中心单独选择，可与超分独立启用。逐帧处理可能出现闪烁或细节伪影，且会增加耗时；与有意添加的曝光动态模糊同时使用可能互相抵消。图 SHA-256 已固定核验；原始链接权重的再分发许可尚未确认，本包及权重不得据此公开发布。独立许可与来源见 licenses/Model-nafnet-gopro-deblur-sdr8-fp32-eval-LICENSE、models/nafnet-gopro-deblur-sdr8-fp32-eval/source-manifest.json 和 PACKAGE-MANIFEST.json。'
    } else { '本包未捆绑 NAFNet 去模糊，相关默认路径为空。' }
    $stateRefreshDescription = if ($freshModelSelection) { '本次为全新模型选择目录，不复制历史队列、恢复记录或用户模型；原目录保持不变。' } else { '刷新本机包会保留旧 data 和用户添加的模型；SHA256SUMS.txt 不包含可变用户 data。' }
    $srDescription = if ($modelSelection.includeSuperResolution) { 'Real-ESRGAN x2plus 超分可选且默认关闭。' } else { '本包未捆绑超分模型。' }
    $inferenceRuntimeDescription = if ($runtimeSelection.includesTensorRt) {
        '本包捆绑 TensorRT 10.9 与 ONNX Runtime CUDA；首次推理可能生成本机 TensorRT 缓存。'
    } else {
        '本包仅捆绑 ONNX Runtime CUDA、CUDA 与 cuDNN，不含 TensorRT；自动后端使用 CUDA。若要启用 TensorRT，请改用 full-tensorrt 版本。'
    }
    $readme = @"
RifeLove 原生 Windows 本机包

启动：双击 RifeLoveNative.exe。此目录是 Win32/Direct2D/DirectWrite 原生桌面包，不包含 Qt、QML 或 CLI。
默认插帧模型：models/rife-4.25-fp32-sdr8-v3。$srDescription 实际打包模型：$includedModelsDescription。
$experimentalModelsDescription
$deblurDescription
任务数据库与本机模型偏好默认保存在 Windows 当前用户的 %LOCALAPPDATA%\RifeLove，不要求 EXE 目录可写。视频输出位置由用户选择，原片不会被覆盖。
只有在 EXE 同目录创建 portable-mode.txt 且第一行为 RifeLovePortable 1，才显式使用旁边的 data 目录。$stateRefreshDescription
当前包使用隔离构建的 FFmpeg 运行库，包含 H.264 NVENC、OpenH264、Kvazaar 和 libaom；自动编码优先使用 H.264 NVENC，并在不可用时软件回退。x86asm：$ffmpegX86asmDescription。处理性能依赖硬件、模型、分辨率和编码配置；短片回归不代表所有素材已验收。
$inferenceRuntimeDescription 仍需要兼容的 NVIDIA 驱动。
只有刷新时发现 data 中存在带精确 v2 路径 token、模型 ID、版本和 SHA-256 的队列/检查点记录，包才会额外保留 models/rife-4.25-fp32-sdr8-v2；干净包不会捆绑未被实际迁移数据引用的重复 RIFE 模型。
依赖来源、导入清单、模型 SHA-256、许可证和限制见 PACKAGE-MANIFEST.json、dependency-imports.json、SHA256SUMS.txt 和 licenses。
"@
    Set-Content -LiteralPath (Join-Path $staging "README-本机原生包.txt") -Value $readme -Encoding utf8NoBOM

    if (-not $freshModelSelection) { Copy-UserState $target $staging $bundledModelDirectories $bundledModelAliases $legacyModelHash }
    if (@(Get-ChildItem -LiteralPath (Join-Path $staging "data") -Force).Count -gt 0) {
        $manifest.packagedDataState = "preserved-existing-local-state; data excluded from immutable checksums"
        $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $staging "PACKAGE-MANIFEST.json") -Encoding utf8NoBOM
    }
    if (-not (Test-Path -LiteralPath (Join-Path $staging "models/default-model.txt") -PathType Leaf)) {
        Set-Content -LiteralPath (Join-Path $staging "models/default-model.txt") -Value "rife-4.25-fp32-sdr8-v3" -Encoding utf8NoBOM
    }
    if (-not $SkipStartupSmoke) { Invoke-StartupSmoke $staging }

    if (Get-PackageTreeItems $staging | Where-Object { $_.Name -match '(?i)(^|-)rifelove-cli(\.exe)?$|(^|-)cli(\.exe)?$' }) { throw "包内出现 CLI 文件，拒绝交付。" }
    if (Get-PackageTreeItems $staging | Where-Object { $_.Name -match '(?i)^qt|qt[0-9]|qml' }) { throw "包内出现 Qt/QML 文件，拒绝交付。" }
    Write-Sha256Manifest $staging

    Assert-PackageNotRunning $target
    if ($freshModelSelection -and (Test-Path -LiteralPath $target)) {
        throw 'Selected model bundle output appeared during staging; refusing to replace or migrate it.'
    }
    if (Test-Path -LiteralPath $target) { Move-Item -LiteralPath $target -Destination $backup }
    try {
        Move-Item -LiteralPath $staging -Destination $target
        $promoted = $true
        if (-not $SkipStartupSmoke) {
            try { Invoke-StartupSmoke $target }
            catch {
                Remove-PackagePath $target
                if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $target }
                throw
            }
        }
    } catch {
        if (-not (Test-Path -LiteralPath $target) -and (Test-Path -LiteralPath $backup)) { Move-Item -LiteralPath $backup -Destination $target }
        throw
    }
    if (Test-Path -LiteralPath $backup) { Remove-PackagePath $backup }
    Write-Output "Created native local package: $target"
    Write-Output "SHA-256 manifest: $(Join-Path $target 'SHA256SUMS.txt')"
    Write-Output "NVENC: integrated in isolated FFmpeg SDK; H.264 NVENC and software fallback require final media regression."
} catch {
    if (Test-Path -LiteralPath $staging) { Remove-PackagePath $staging }
    throw
}
