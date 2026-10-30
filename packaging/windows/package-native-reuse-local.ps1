[CmdletBinding()]
param(
    [string]$SourceDirectory = "",
    [string]$BuildDirectory = "",
    [string]$OutputDirectory = "",
    [string]$TemporalRestorationModelDirectory = ""
)

# Local preview only. Hard links save space on the same NTFS volume. The new
# names remain usable if the old directory is removed, but modifying the linked
# files in place changes both previews; the checksum manifest detects that.
$ErrorActionPreference = "Stop"
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "out"))
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $outputRoot "RifeLoveNative-local-nvenc" }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repositoryRoot "build/native-ui-review" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $outputRoot "RifeLoveNative-local-ui-preview" }
if (-not $TemporalRestorationModelDirectory) { $TemporalRestorationModelDirectory = Join-Path $repositoryRoot "models/cache/fastdvdnet-clipped-five-frame-sdr8-eval" }
$source = [IO.Path]::GetFullPath($SourceDirectory)
$build = [IO.Path]::GetFullPath($BuildDirectory)
$target = [IO.Path]::GetFullPath($OutputDirectory)
$rootPrefix = $outputRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $source.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not $target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    [string]::Equals($source, $target, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Source and new output must be distinct directories inside the repository out directory."
}
if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Source package is missing: $source" }
if (Test-Path -LiteralPath $target) { throw "Refusing to overwrite an existing package: $target" }
foreach ($directory in @($outputRoot, $source, $build)) {
    $item = Get-Item -LiteralPath $directory -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package roots must not be links: $directory" }
}
$exe = Join-Path $build "RifeLoveNative.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Native executable is missing: $exe" }

function Get-SafeFiles([string]$directory) {
    foreach ($item in (Get-ChildItem -LiteralPath $directory -Force)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source package contains a link: $($item.FullName)" }
        if ($item.PSIsContainer) {
            if ($item.FullName -ne (Join-Path $source "data")) { Get-SafeFiles $item.FullName }
        } else { $item }
    }
}

function Get-SafeTreeFiles([string]$directory) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) { throw "Model directory is missing: $directory" }
    $root = Get-Item -LiteralPath $directory -Force
    if ($root.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Model directory cannot be a link: $directory" }
    foreach ($item in (Get-ChildItem -LiteralPath $directory -Force)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Model tree contains a link: $($item.FullName)" }
        if ($item.PSIsContainer) { Get-SafeTreeFiles $item.FullName } else { $item }
    }
}

function Read-ValidatedModelPack([string]$directory, [string]$label, [string]$expectedId, [string]$expectedFamily) {
    $files = @(Get-SafeTreeFiles $directory)
    $descriptorPath = Join-Path $directory "model.json"
    if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) { throw "$label descriptor is missing: $directory" }
    $descriptor = Get-Content -Raw -LiteralPath $descriptorPath | ConvertFrom-Json
    if ([string]$descriptor.id -cne $expectedId) { throw "$label id mismatch: $directory" }
    if ([string]$descriptor.family -cne $expectedFamily) { throw "$label family mismatch: $directory" }
    if ([string]$descriptor.modelFile -cne "model.onnx") { throw "$label must use model.onnx: $directory" }
    $graph = Join-Path $directory "model.onnx"
    if (-not (Test-Path -LiteralPath $graph -PathType Leaf)) { throw "$label graph is missing: $directory" }
    $graphHash = (Get-FileHash -LiteralPath $graph -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($graphHash -ne ([string]$descriptor.sha256).ToLowerInvariant()) { throw "$label graph SHA-256 mismatch: $directory" }
    $licenseFile = [string]$descriptor.license.file
    if ([string]::IsNullOrWhiteSpace($licenseFile) -or [IO.Path]::GetFileName($licenseFile) -cne $licenseFile) { throw "$label license filename is unsafe: $directory" }
    $licensePath = Join-Path $directory $licenseFile
    if (-not (Test-Path -LiteralPath $licensePath -PathType Leaf)) { throw "$label license is missing: $licensePath" }
    $licenseHash = [string]$descriptor.license.sha256
    if ($licenseHash -notmatch '^[a-fA-F0-9]{64}$' -or
        (Get-FileHash -LiteralPath $licensePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $licenseHash.ToLowerInvariant()) {
        throw "$label license SHA-256 mismatch: $directory"
    }
    $provenanceFile = [string]$descriptor.provenance.manifestFile
    $validationFile = [string]$descriptor.validation.reportFile
    if ([IO.Path]::GetFileName($provenanceFile) -cne $provenanceFile -or [IO.Path]::GetFileName($validationFile) -cne $validationFile) { throw "$label sidecar filename is unsafe: $directory" }
    foreach ($sidecar in @(
        [pscustomobject]@{ Path = Join-Path $directory $provenanceFile; Hash = [string]$descriptor.provenance.manifestSha256; Label = "provenance" },
        [pscustomobject]@{ Path = Join-Path $directory $validationFile; Hash = [string]$descriptor.validation.reportSha256; Label = "validation" }
    )) {
        if (-not (Test-Path -LiteralPath $sidecar.Path -PathType Leaf)) { throw "$label $($sidecar.Label) sidecar is missing: $directory" }
        if ((Get-FileHash -LiteralPath $sidecar.Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sidecar.Hash.ToLowerInvariant()) { throw "$label $($sidecar.Label) SHA-256 mismatch: $directory" }
    }
    [pscustomobject]@{ source = (Get-Item -LiteralPath $directory -Force).FullName; files = $files; descriptor = $descriptor; graphHash = $graphHash; licensePath = $licensePath }
}

function New-HardLinkChecked([string]$sourceFile, [string]$destinationFile) {
    if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) { throw "Hard-link source is missing: $sourceFile" }
    $sourceItem = Get-Item -LiteralPath $sourceFile -Force
    if ($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Hard-link source cannot be a reparse point: $sourceFile" }
    if (Test-Path -LiteralPath $destinationFile) { throw "Hard-link destination already exists: $destinationFile" }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destinationFile) -Force | Out-Null
    New-Item -ItemType HardLink -Path $destinationFile -Target $sourceItem.FullName | Out-Null
}

function Resolve-ManifestPath([string]$relative) {
    $parts = $relative -split '[\\/]'
    if ([IO.Path]::IsPathRooted($relative) -or $parts.Count -eq 0 -or
        @($parts | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0) {
        throw "Unsafe manifest path: $relative"
    }
    $full = [IO.Path]::GetFullPath((Join-Path $source ($relative.Replace('/', '\'))))
    $prefix = $source.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Manifest path leaves source package: $relative" }
    return $full
}

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label 文件不存在：$Path" }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "$Label 文件不能是链接：$Path" }
    return $item.FullName
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
    $selectedRoot = Resolve-RepositoryPath $FfmpegPath
    if (-not [string]::Equals($cacheRoot, $selectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "打包拒绝：CMakeCache 的 RIFELOVE_FFMPEG_ROOT 与复用包 FFmpeg 根目录不一致。CMake=$cacheRoot；复用包=$selectedRoot"
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
    }
}

$manifestFile = Join-Path $source "PACKAGE-MANIFEST.json"
$sumFile = Join-Path $source "SHA256SUMS.txt"
if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf) -or -not (Test-Path -LiteralPath $sumFile -PathType Leaf)) {
    throw "Source package manifest or checksum list is missing."
}
$manifest = Get-Content -Raw -LiteralPath $manifestFile | ConvertFrom-Json -AsHashtable
if ($manifest.packageKind -ne "local-native-windows-x64" -or $manifest.qtIncluded -or $manifest.cliIncluded -or $manifest.ossEnabled) {
    throw "Source package is not the expected local, non-Qt desktop bundle."
}
if ([string]::IsNullOrWhiteSpace([string]$manifest.ffmpeg.root)) { throw "Source package has no FFmpeg SDK root; rebuild a full package first." }
$ffmpegSdk = Read-FfmpegSdkIdentity $build ([string]$manifest.ffmpeg.root)
if ($manifest.ffmpeg.ContainsKey("sdkManifest")) {
    $recordedSdk = $manifest.ffmpeg.sdkManifest
    if ($recordedSdk -isnot [System.Collections.IDictionary]) { throw "Source package FFmpeg SDK manifest is invalid; rebuild a full package first." }
    foreach ($property in @("path", "sha256", "nvencApiMajor", "nvencApiMinor", "nvencMinimumWindowsDriver")) {
        if (-not $recordedSdk.Contains($property)) { throw "Source package FFmpeg SDK manifest is missing $property; rebuild a full package first." }
    }
    if ([string]$recordedSdk.path -cne $ffmpegSdk.manifestRelativePath -or
        [string]$recordedSdk.sha256 -cne $ffmpegSdk.manifestSha256 -or
        [string]$recordedSdk.nvencApiMajor -cne [string]$ffmpegSdk.nvencApiMajor -or
        [string]$recordedSdk.nvencApiMinor -cne [string]$ffmpegSdk.nvencApiMinor -or
        [string]$recordedSdk.nvencMinimumWindowsDriver -cne $ffmpegSdk.nvencMinimumWindowsDriver) {
        throw "Source package FFmpeg SDK manifest differs from the selected build; rebuild a full package instead."
    }
}
$requiredModelDirectories = @(
    "models/rife-4.25-fp32-sdr8-v3",
    "models/realesrgan-x2plus-sdr8-v2"
)
$manifestModelDirectories = @($manifest.models | ForEach-Object { ([string]$_.directory).Replace('\', '/') })
foreach ($relative in $requiredModelDirectories) {
    if ($manifestModelDirectories -notcontains $relative) {
        throw "Source package lacks the required validated model: $relative. Run package-native-local.ps1 first; an old two-model preview cannot be upgraded by hard-link reuse."
    }
    $modelRoot = Join-Path $source ($relative.Replace('/', '\'))
    if (-not (Test-Path -LiteralPath $modelRoot -PathType Container) -or
        -not (Test-Path -LiteralPath (Join-Path $modelRoot 'model.onnx') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $modelRoot 'model.json') -PathType Leaf)) {
        throw "Source package model is incomplete: $relative"
    }
    $entry = @($manifest.models | Where-Object { ([string]$_.directory).Replace('\', '/') -eq $relative }) | Select-Object -First 1
    if ($null -eq $entry -or [string]::IsNullOrWhiteSpace([string]$entry.sha256)) { throw "Source package model has no manifest SHA-256: $relative" }
    $actual = (Get-FileHash -LiteralPath (Join-Path $modelRoot 'model.onnx') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne ([string]$entry.sha256).ToLowerInvariant()) { throw "Source package model SHA-256 mismatch: $relative" }
}
$forbiddenDuplicateModelDirectories = @(
    "models/realesrgan-x2plus-sdr8-candidate-v2",
    "models/fastdvdnet-clipped-five-frame-sdr8"
)
foreach ($relative in $forbiddenDuplicateModelDirectories) {
    if (Test-Path -LiteralPath (Join-Path $source ($relative.Replace('/', '\'))) -PathType Container) {
        throw "Source package contains a duplicate/legacy model basename: $relative"
    }
}
$requiredRuntimeFiles = @(
    "onnxruntime.dll", "onnxruntime_providers_shared.dll", "onnxruntime_providers_cuda.dll", "onnxruntime_providers_tensorrt.dll",
    "cudart64_12.dll", "cublas64_12.dll", "cublasLt64_12.dll", "cufft64_11.dll", "cufftw64_11.dll", "nvblas64_12.dll",
    "cudnn64_9.dll", "cudnn_adv64_9.dll", "cudnn_cnn64_9.dll", "cudnn_engines_precompiled64_9.dll",
    "cudnn_engines_runtime_compiled64_9.dll", "cudnn_graph64_9.dll", "cudnn_heuristic64_9.dll", "cudnn_ops64_9.dll",
    "nvinfer_10.dll", "nvinfer_builder_resource_10.dll", "nvinfer_plugin_10.dll", "nvonnxparser_10.dll"
)
$recordedRuntimeFiles = @($manifest.inferenceRuntime.files | ForEach-Object { ([string]$_).ToLowerInvariant() })
foreach ($name in $requiredRuntimeFiles) {
    if ($recordedRuntimeFiles -notcontains $name.ToLowerInvariant() -or
        -not (Test-Path -LiteralPath (Join-Path $source "runtime/$name") -PathType Leaf)) {
        throw "Source package lacks the required inference runtime DLL: $name"
    }
}
$temporalRelative = "models/fastdvdnet-clipped-five-frame-sdr8-eval"
$temporalInSource = $manifestModelDirectories -contains $temporalRelative
$temporalPack = $null
if ($temporalInSource) {
    $temporalRoot = Join-Path $source ($temporalRelative.Replace('/', '\'))
    $temporalEntry = @($manifest.models | Where-Object { ([string]$_.directory).Replace('\', '/') -eq $temporalRelative }) | Select-Object -First 1
    if ($null -eq $temporalEntry -or [string]$temporalEntry.sha256 -ne ((Get-FileHash -LiteralPath (Join-Path $temporalRoot 'model.onnx') -Algorithm SHA256).Hash.ToLowerInvariant())) {
        throw "Source package temporal model SHA-256 mismatch."
    }
    $temporalPack = Read-ValidatedModelPack $temporalRoot "FastDVDnet source model" "fastdvdnet-clipped-five-frame-sdr8-eval" "video-restoration"
} else {
    $temporalPack = Read-ValidatedModelPack $TemporalRestorationModelDirectory "FastDVDnet local model" "fastdvdnet-clipped-five-frame-sdr8-eval" "video-restoration"
}
$temporalAdditionalExpected = @{}
if (-not $temporalInSource) {
    foreach ($file in $temporalPack.files) {
        $relative = "models/fastdvdnet-clipped-five-frame-sdr8-eval/" + $file.FullName.Substring($temporalPack.source.Length).TrimStart('\', '/').Replace('\', '/')
        $temporalAdditionalExpected[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $temporalLicenseRelative = "licenses/Model-fastdvdnet-clipped-five-frame-sdr8-eval-LICENSE.txt"
    $temporalAdditionalExpected[$temporalLicenseRelative] = (Get-FileHash -LiteralPath $temporalPack.licensePath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$expected = @{}
foreach ($line in (Get-Content -LiteralPath $sumFile)) {
    if ($line -eq "RifeLove native local bundle SHA-256" -or [string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -notmatch '^([a-fA-F0-9]{64})  (.+)$') { throw "Malformed source checksum entry." }
    $hash = $Matches[1].ToLowerInvariant()
    $relative = $Matches[2].Replace('\', '/')
    if ($expected.ContainsKey($relative)) { throw "Duplicate source checksum path: $relative" }
    $null = Resolve-ManifestPath $relative
    $expected[$relative] = $hash
}
$files = @(Get-SafeFiles $source)
foreach ($file in $files) {
    $relative = $file.FullName.Substring($source.Length).TrimStart('\', '/').Replace('\', '/')
    if ($relative -eq "SHA256SUMS.txt") { continue }
    if (-not $expected.ContainsKey($relative)) { throw "Unlisted source package file: $relative" }
    # The old EXE is intentionally replaced: its checksum is already known to
    # be stale. Every other immutable input must still match the source list.
    if ($relative -eq "RifeLoveNative.exe") { continue }
    if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected[$relative]) {
        throw "Source checksum mismatch: $relative"
    }
}
foreach ($relative in $expected.Keys) {
    if (-not (Test-Path -LiteralPath (Resolve-ManifestPath $relative) -PathType Leaf)) { throw "Source checksum file is missing: $relative" }
}

$objdump = (Get-Command llvm-objdump.exe -ErrorAction Stop).Source
function Get-Imports([string]$binary) {
    return @(& $objdump -p $binary | ForEach-Object {
        if ($_ -match 'DLL Name:\s*(\S+)') { $Matches[1].ToLowerInvariant() }
    } | Sort-Object -Unique)
}
$newImports = @(Get-Imports $exe)
$oldImports = @(Get-Imports (Join-Path $source "RifeLoveNative.exe"))
if ($newImports.Count -eq 0 -or (Compare-Object -ReferenceObject $oldImports -DifferenceObject $newImports)) {
    throw "New executable imports differ from the source bundle; rebuild a full package instead."
}
$recordedImports = @((Get-Content -Raw -LiteralPath (Join-Path $source "dependency-imports.json") | ConvertFrom-Json).native | Sort-Object -Unique)
if (Compare-Object -ReferenceObject $recordedImports -DifferenceObject $newImports) {
    throw "New executable imports differ from the recorded dependency list."
}
if ($newImports | Where-Object { $_ -match '(?i)^qt|qml' }) { throw "New executable imports Qt/QML." }

$stage = "$target.staging-$([guid]::NewGuid().ToString('N'))"
if (-not $stage.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Invalid staging path." }
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($source.Length).TrimStart('\', '/').Replace('\', '/')
        if ($relative -in @("SHA256SUMS.txt", "PACKAGE-MANIFEST.json", "RifeLoveNative.exe", "README-本机原生包.txt", "config/defaults.json")) { continue }
        if ($relative -match '(?i)(^|/)qt|qml|(^|/)rifelove-cli(\.exe)?$') { throw "Unexpected Qt or CLI artifact: $relative" }
        $destination = Join-Path $stage ($relative.Replace('/', '\'))
        New-HardLinkChecked $file.FullName $destination
    }
    if (-not $temporalInSource) {
        foreach ($file in $temporalPack.files) {
            $relative = "models/fastdvdnet-clipped-five-frame-sdr8-eval/" + $file.FullName.Substring($temporalPack.source.Length).TrimStart('\', '/').Replace('\', '/')
            New-HardLinkChecked $file.FullName (Join-Path $stage ($relative.Replace('/', '\')))
        }
        New-HardLinkChecked $temporalPack.licensePath (Join-Path $stage "licenses/Model-fastdvdnet-clipped-five-frame-sdr8-eval-LICENSE.txt")
    }
    New-Item -ItemType Directory -Path (Join-Path $stage "data") | Out-Null
    Copy-Item -LiteralPath $exe -Destination (Join-Path $stage "RifeLoveNative.exe")

    $defaultsPath = Join-Path $source "config/defaults.json"
    if (-not (Test-Path -LiteralPath $defaultsPath -PathType Leaf)) { throw "Source package defaults are missing: $defaultsPath" }
    $defaults = Get-Content -Raw -LiteralPath $defaultsPath | ConvertFrom-Json -AsHashtable
    $defaults["temporalRestorationModelDirectory"] = $temporalRelative
    $defaults["temporalRestorationDefaultEnabled"] = $false
    $defaults["temporalRestorationMode"] = "experimental-optional"
    New-Item -ItemType Directory -Path (Join-Path $stage "config") -Force | Out-Null
    $defaults | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $stage "config/defaults.json") -Encoding utf8NoBOM

    foreach ($entry in @($manifest.models)) {
        $entryRelative = ([string]$entry.directory).Replace('\', '/')
        $entryRoot = Join-Path $stage ($entryRelative.Replace('/', '\'))
        $entryDescriptor = Get-Content -Raw -LiteralPath (Join-Path $entryRoot "model.json") | ConvertFrom-Json
        $entry.licenseId = if ($entryDescriptor.license -is [string]) { [string]$entryDescriptor.license } else { [string]$entryDescriptor.license.id }
        $entry.validatedBackends = @($entryDescriptor.validation.validatedBackends)
        $entry.role = if ($entryRelative -eq $temporalRelative) { "experimental-optional-temporal-denoise" } elseif ($entryRelative -eq "models/realesrgan-x2plus-sdr8-v2") { "optional-super-resolution" } else { "default-interpolation" }
    }
    if (-not $temporalInSource) {
        $manifest.models = @($manifest.models) + [ordered]@{
            directory = $temporalRelative
            id = [string]$temporalPack.descriptor.id
            family = [string]$temporalPack.descriptor.family
            version = [string]$temporalPack.descriptor.version
            sha256 = $temporalPack.graphHash
            licenseId = [string]$temporalPack.descriptor.license.id
            validatedBackends = @($temporalPack.descriptor.validation.validatedBackends)
            legacy = $false
            hiddenFromDefaultSelection = $false
            canonicalBasename = "fastdvdnet-clipped-five-frame-sdr8-eval"
            role = "experimental-optional-temporal-denoise"
            redistribution = "local-only-until-license-review"
        }
    }
    $manifest.inferenceRuntime.requiredFiles = $requiredRuntimeFiles
    $manifest.inferenceRuntime.providers = @("TensorRT", "CUDA", "CPU-fallback")
    $manifest.temporalRestorationDefaultEnabled = $false
    $manifest.storage = "local-only; no Alibaba OSS"
    $manifest.packageName = Split-Path -Leaf $target
    $manifest.generatedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
    $manifest.executableSha256 = (Get-FileHash -LiteralPath (Join-Path $stage "RifeLoveNative.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest.ffmpeg.sdkManifest = [ordered]@{ path = $ffmpegSdk.manifestRelativePath; sha256 = $ffmpegSdk.manifestSha256; nvencApiMajor = $ffmpegSdk.nvencApiMajor; nvencApiMinor = $ffmpegSdk.nvencApiMinor; nvencMinimumWindowsDriver = $ffmpegSdk.nvencMinimumWindowsDriver }
    $manifest.updatePolicy = "local preview; immutable dependencies share NTFS hard links with the source package; FastDVDnet is linked from the local model cache when absent"
    $manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $stage "PACKAGE-MANIFEST.json") -Encoding utf8NoBOM
    @"
RifeLove 原生 Windows 本机预览

双击 RifeLoveNative.exe。请保留此目录中的 runtime、models 和其他运行库。
这是本机预览，不是完整产品或可公开再分发的安装包。支持单任务、文件夹批量、任务历史、当前会话暂停/继续和平台副本预设；中断恢复界面、模型中心和动态压缩/细节修复仍未完成。
高帧率与长视频仍需更广泛的兼容性验收；失败时不会发布未通过验证的输出。TensorRT 初次加载可能较慢。
默认使用 RIFE 4.25 插帧模型；Real-ESRGAN x2plus 超分可选，默认关闭。
包内 FastDVDnet 五帧模型仅是 SDR8 实验性时序降噪模型，默认关闭；它可与超分同时选择，但不代表通用压缩修复、去模糊或细节增强能力。
此目录与旧本机预览使用 NTFS 硬链接复用不可变依赖，删除旧目录不会使本目录失效；直接修改共享依赖会同时影响两个目录。
模型、视频和任务留在本机；阿里云 OSS 已停用。模型及 NVIDIA 运行库的公开再分发许可仍需审核。
"@ | Set-Content -LiteralPath (Join-Path $stage "README-本机原生包.txt") -Encoding utf8NoBOM
    $lines = @("RifeLove native local bundle SHA-256")
    foreach ($file in @(Get-SafeFiles $stage | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($stage.Length).TrimStart('\', '/').Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $knownHash = if ($expected.ContainsKey($relative)) { $expected[$relative] } elseif ($temporalAdditionalExpected.ContainsKey($relative)) { $temporalAdditionalExpected[$relative] } else { $null }
        if ($relative -notin @("PACKAGE-MANIFEST.json", "RifeLoveNative.exe", "README-本机原生包.txt", "config/defaults.json") -and
            ([string]::IsNullOrWhiteSpace($knownHash) -or $hash -ne $knownHash)) {
            throw "Linked dependency changed during packaging: $relative"
        }
        $lines += "$hash  $relative"
    }
    [IO.File]::WriteAllLines((Join-Path $stage "SHA256SUMS.txt"), [string[]]$lines, [Text.UTF8Encoding]::new($false))
    [IO.Directory]::Move($stage, $target)
    Write-Output "Created local preview: $target"
    Write-Output "Executable SHA-256: $($manifest.executableSha256)"
} catch {
    # The exact random staging directory was created by this invocation under
    # the checked output root; never remove or replace a pre-existing target.
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    throw
}
