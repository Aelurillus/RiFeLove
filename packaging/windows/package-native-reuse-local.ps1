[CmdletBinding()]
param(
    [string]$SourceDirectory = "",
    [string]$BuildDirectory = "",
    [string]$OutputDirectory = "",
    [string]$TemporalRestorationModelDirectory = "",
    [string]$CompressionRestorationModelDirectory = "",
    # Fresh base bundle: only RIFE + SR are staged; experiments are explicit.
    [switch]$BaseModelsOnly,
    [switch]$IncludeTemporalRestoration,
    [switch]$IncludeCompressionRestoration,
    # Explicitly restage only differing FFmpeg DLLs from the selected build's
    # SDK. Source/model/inference files remain verified, immutable hard links.
    [switch]$RefreshFfmpegRuntime,
    # Create a new explicitly portable package with a permanent, checksummed
    # marker and empty independent data directory, suitable for isolated QA.
    [switch]$PortableMode
)

# Local preview only. Hard links save space on the same NTFS volume. The new
# names remain usable if the old directory is removed, but modifying the linked
# files in place changes both previews; the checksum manifest detects that.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ffmpeg-sdk-helpers.ps1")
. (Join-Path $PSScriptRoot "native-package-options.ps1")
. (Join-Path $PSScriptRoot "native-build-dependencies.ps1")
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "out"))
$modelSelection = Resolve-NativeModelBundleSelection -BaseModelsOnly:$BaseModelsOnly -IncludeTemporalRestoration:$IncludeTemporalRestoration -IncludeCompressionRestoration:$IncludeCompressionRestoration
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $outputRoot "RifeLoveNative-local-nvenc" }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repositoryRoot "build/native-ui-review" }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $outputRoot "RifeLoveNative-local-ui-preview" }
if (-not $TemporalRestorationModelDirectory) { $TemporalRestorationModelDirectory = Join-Path $repositoryRoot "models/cache/fastdvdnet-clipped-five-frame-sdr8-eval" }
if (-not $CompressionRestorationModelDirectory -and $IncludeCompressionRestoration) { $CompressionRestorationModelDirectory = Join-Path $repositoryRoot "models/cache/stdf-r3-y7-sdr8-local-eval" }
$source = [IO.Path]::GetFullPath($SourceDirectory)
$build = [IO.Path]::GetFullPath($BuildDirectory)
$target = [IO.Path]::GetFullPath($OutputDirectory)
$rootPrefix = $outputRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $source.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    -not $target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    [string]::Equals($source, $target, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Source and new output must be distinct directories inside the repository out directory."
}
Assert-NativePackageDirectoriesDisjoint -SourceDirectory $source -OutputDirectory $target
if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Source package is missing: $source" }
if (Test-Path -LiteralPath $target) { throw "Refusing to overwrite an existing package: $target" }
foreach ($directory in @($outputRoot, $source, $build)) {
    $item = Get-Item -LiteralPath $directory -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package roots must not be links: $directory" }
}
$exe = Join-Path $build "RifeLoveNative.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Native executable is missing: $exe" }

function Get-SafeFiles([string]$directory, [string[]]$ExcludedRoots = @(), [string]$TreeRoot = $directory, [object]$Selection = $null) {
    foreach ($item in (Get-ChildItem -LiteralPath $directory -Force)) {
        $relative = [IO.Path]::GetRelativePath($TreeRoot, $item.FullName).Replace('\','/')
        if ($relative -ieq 'data' -or (Test-NativeRelativePathExcluded $relative $ExcludedRoots $Selection)) { continue }
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source package contains a link: $($item.FullName)" }
        if ($item.PSIsContainer) {
            Get-SafeFiles $item.FullName $ExcludedRoots $TreeRoot $Selection
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

function Get-NativeModelRole([string]$relative) {
    switch ($relative.Replace('\', '/')) {
        "models/fastdvdnet-clipped-five-frame-sdr8-eval" { return "experimental-optional-temporal-denoise" }
        "models/stdf-r3-y7-sdr8-local-eval" { return "experimental-optional-compression-repair" }
        "models/realesrgan-x2plus-sdr8-v2" { return "optional-super-resolution" }
        default { return "default-interpolation" }
    }
}

function Get-NativeOptionalRestorationPacks(
    [object]$Manifest, [string]$SourceDirectory,
    [string]$TemporalModelDirectory, [string]$CompressionModelDirectory,
    [object]$Selection = $null) {
    foreach ($spec in @(
        [pscustomobject]@{ id = "fastdvdnet-clipped-five-frame-sdr8-eval"; local = $TemporalModelDirectory; label = "FastDVDnet" },
        [pscustomobject]@{ id = "stdf-r3-y7-sdr8-local-eval"; local = $CompressionModelDirectory; label = "STDF-R3" }
    )) {
        if ($null -ne $Selection -and $spec.id -ceq 'fastdvdnet-clipped-five-frame-sdr8-eval' -and -not $Selection.includeTemporalRestoration) { continue }
        if ($null -ne $Selection -and $spec.id -ceq 'stdf-r3-y7-sdr8-local-eval' -and -not $Selection.includeCompressionRestoration) { continue }
        $relative = "models/$($spec.id)"
        $entries = @($Manifest.models | Where-Object { ([string]$_.directory).Replace('\', '/') -ieq $relative })
        if ($entries.Count -gt 1) { throw "Duplicate source model manifest entry: $relative" }
        $inSource = $entries.Count -eq 1
        # An older three-model bundle remains usable without claiming it has
        # STDF. Adding STDF from a local cache requires the explicit parameter.
        if (-not $inSource -and $spec.id -ceq "stdf-r3-y7-sdr8-local-eval" -and
            [string]::IsNullOrWhiteSpace($spec.local) -and
            ($null -eq $Selection -or -not $Selection.requireCompressionRestoration)) { continue }
        $packRoot = if ($inSource) { Join-Path $SourceDirectory $relative } else { $spec.local }
        if ([string]::IsNullOrWhiteSpace($packRoot)) { throw "$($spec.label) model directory is missing; selected models require an existing source or explicit local pack." }
        if (-not $inSource -and (Test-Path -LiteralPath (Join-Path $SourceDirectory $relative))) {
            throw "Source model directory is not declared in its manifest: $relative"
        }
        $pack = Read-ValidatedModelPack $packRoot "$($spec.label) model" $spec.id "video-restoration"
        $licenseRelative = "licenses/Model-$($spec.id)-$([IO.Path]::GetFileName($pack.licensePath))"
        if ($inSource) {
            if ([string]$entries[0].sha256 -ine $pack.graphHash -or
                [string]$entries[0].id -cne [string]$pack.descriptor.id -or
                [string]$entries[0].family -cne [string]$pack.descriptor.family -or
                [string]$entries[0].version -cne [string]$pack.descriptor.version) {
                throw "Source model manifest identity mismatch: $relative"
            }
            $bundledLicense = Join-Path $SourceDirectory $licenseRelative
            if (-not (Test-Path -LiteralPath $bundledLicense -PathType Leaf) -or
                (Get-FileHash -LiteralPath $bundledLicense -Algorithm SHA256).Hash -ine [string]$pack.descriptor.license.sha256) {
                throw "Source model bundled license mismatch: $relative"
            }
        }
        [pscustomobject]@{
            relative = $relative; inSource = $inSource; pack = $pack
            licenseRelative = $licenseRelative; role = Get-NativeModelRole $relative
        }
    }
}

function Set-NativeRestorationDefaults(
    [System.Collections.IDictionary]$Defaults, [bool]$CompressionModelIncluded = $true,
    [bool]$TemporalModelIncluded = $true) {
    $Defaults["temporalRestorationModelDirectory"] = if ($TemporalModelIncluded) { "models/fastdvdnet-clipped-five-frame-sdr8-eval" } else { $null }
    $Defaults["temporalRestorationDefaultEnabled"] = $false
    $Defaults["temporalRestorationMode"] = if ($TemporalModelIncluded) { "experimental-optional" } else { "not-bundled" }
    $Defaults["compressionRestorationModelDirectory"] = if ($CompressionModelIncluded) { "models/stdf-r3-y7-sdr8-local-eval" } else { $null }
    $Defaults["compressionRestorationDefaultEnabled"] = $false
    $Defaults["compressionRestorationTiled"] = $false
    $Defaults["compressionRestorationStrength"] = 0.75
    $Defaults["compressionRestorationMode"] = if ($CompressionModelIncluded) { "experimental-local-evaluation" } else { "not-bundled" }
}

function Update-NativeOptionalRestorationManifest(
    [System.Collections.IDictionary]$Manifest, [object[]]$OptionalPacks) {
    foreach ($optional in $OptionalPacks) {
        $pack = $optional.pack
        $rows = @($Manifest.models | Where-Object { ([string]$_.directory).Replace('\', '/') -ieq $optional.relative })
        if ($rows.Count -gt 1) { throw "Duplicate output model manifest entry: $($optional.relative)" }
        $entry = if ($rows.Count -eq 1) { $rows[0] } else { [ordered]@{} }
        $entry["directory"] = $optional.relative
        $entry["id"] = [string]$pack.descriptor.id
        $entry["family"] = [string]$pack.descriptor.family
        $entry["version"] = [string]$pack.descriptor.version
        $entry["sha256"] = $pack.graphHash
        $entry["licenseId"] = [string]$pack.descriptor.license.id
        $entry["validatedBackends"] = @($pack.descriptor.validation.validatedBackends)
        $entry["legacy"] = $false
        $entry["hiddenFromDefaultSelection"] = $false
        $entry["canonicalBasename"] = [string]$pack.descriptor.id
        $entry["role"] = $optional.role
        $entry["redistribution"] = "local-only-until-license-review"
        if ($rows.Count -eq 0) { $Manifest["models"] = @($Manifest.models) + $entry }
    }
    $Manifest["temporalRestorationDefaultEnabled"] = $false
    $Manifest["compressionRestorationDefaultEnabled"] = $false
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

function Read-NativeSourceChecksums([string]$ChecksumFile, [string[]]$ExcludedRoots = @(), [object]$Selection = $null) {
    $result = @{}
    $seen = @{}
    foreach ($line in (Get-Content -LiteralPath $ChecksumFile)) {
        if ($line -eq 'RifeLove native local bundle SHA-256' -or [string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line -notmatch '^([a-fA-F0-9]{64})  (.+)$') { throw 'Malformed source checksum entry.' }
        $hash = $Matches[1].ToLowerInvariant()
        $relative = $Matches[2].Replace('\','/')
        if ($seen.ContainsKey($relative)) { throw "Duplicate source checksum path: $relative" }
        $seen[$relative] = $true
        $null = Resolve-ManifestPath $relative
        if ($relative.StartsWith('data/',[StringComparison]::OrdinalIgnoreCase) -or
            (Test-NativeRelativePathExcluded $relative $ExcludedRoots $Selection)) { continue }
        $result[$relative] = $hash
    }
    return $result
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
    $selectedRoot = if ($FfmpegPath -ceq "local-sdk:ffmpeg") { $cacheRoot } else { Resolve-RepositoryPath $FfmpegPath }
    if (-not [string]::Equals($cacheRoot, $selectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "打包拒绝：CMakeCache 的 RIFELOVE_FFMPEG_ROOT 与复用包 FFmpeg 根目录不一致。CMake=$cacheRoot；复用包=$selectedRoot"
    }
    $linkInputs = Assert-FfmpegBuildLinkIdentity -BuildDirectory $BuildPath -SdkRoot $selectedRoot
    foreach ($entry in @(@('RIFELOVE_FFMPEG_TOOL','ffmpeg.exe'), @('RIFELOVE_FFPROBE_TOOL','ffprobe.exe'))) {
        $cachedTool = Resolve-RepositoryPath (Read-CMakeCacheValue $cachePath $entry[0])
        $expectedTool = Normalize-ComparablePath (Join-Path $selectedRoot "bin/$($entry[1])")
        if (-not [string]::Equals($cachedTool,$expectedTool,[StringComparison]::OrdinalIgnoreCase)) {
            throw "Reuse refused: $($entry[0]) does not point to the selected SDK. Reconfigure both FFmpeg/ffprobe tool cache paths explicitly."
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

$manifestFile = Join-Path $source "PACKAGE-MANIFEST.json"
$sumFile = Join-Path $source "SHA256SUMS.txt"
if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf) -or -not (Test-Path -LiteralPath $sumFile -PathType Leaf)) {
    throw "Source package manifest or checksum list is missing."
}
$manifest = Get-Content -Raw -LiteralPath $manifestFile | ConvertFrom-Json -AsHashtable
if ($manifest.packageKind -ne "local-native-windows-x64" -or $manifest.qtIncluded -or $manifest.cliIncluded -or $manifest.ossEnabled) {
    throw "Source package is not the expected local, non-Qt desktop bundle."
}
$excludedModelRoots = @(Get-NativeExcludedModelRoots $modelSelection)
if ($modelSelection.mode -ceq 'base') {
    # Filter metadata before optional model validation and staging. This is the
    # key isolation boundary: excluded model directories and their licenses are
    # never opened, hashed, or hard-linked for a base bundle.
    foreach ($entry in @($manifest.models)) {
        $relative = ([string]$entry.directory).Replace('\','/')
        if (Test-NativeRelativePathExcluded $relative $excludedModelRoots $modelSelection) {
            $excludedModelRoots += @($relative, ('licenses/Model-' + ($relative -split '/')[-1]))
        }
    }
    $manifest.models = @($manifest.models | Where-Object {
        -not (Test-NativeRelativePathExcluded ([string]$_.directory) $excludedModelRoots $modelSelection)
    })
    $manifest['migration'] = [ordered]@{ legacyModelIncluded = $false; evidence = @(); sourceRecoveryDataCopied = $false }
}
if ([string]::IsNullOrWhiteSpace([string]$manifest.ffmpeg.root)) { throw "Source package has no FFmpeg SDK root; rebuild a full package first." }
$ffmpegSdk = Read-FfmpegSdkIdentity $build $(if ($RefreshFfmpegRuntime) { 'local-sdk:ffmpeg' } else { [string]$manifest.ffmpeg.root })
if ($null -eq $manifest.ffmpeg.binaries -or @($manifest.ffmpeg.binaries).Count -eq 0) {
    throw "Source package has no FFmpeg runtime DLL list; create a full package first."
}
# A source lock identifies input versions, not the compiled configuration. Both
# baseline and x86asm builds share it, so verify the actual reused DLL bytes.
$ffmpegFiles = @($manifest.ffmpeg.binaries)
$ffmpegRuntimeHashes = Assert-FfmpegRuntimeIdentity -SdkRoot $ffmpegSdk.root -RuntimeDirectory $build -FileNames $ffmpegFiles
$ffmpegRefreshPlan = Get-FfmpegRuntimeRefreshPlan -SdkRoot $ffmpegSdk.root -SourceDirectory $source -FileNames $ffmpegFiles
$refreshedFfmpegFiles = @{}
if ($RefreshFfmpegRuntime) {
    $supportedFiles = @('avcodec-62.dll','avformat-62.dll','avutil-60.dll','swresample-6.dll','swscale-9.dll','avfilter-11.dll','avdevice-62.dll','libaom.dll','libkvazaar.dll','libopenh264-8.dll')
    if ($ffmpegFiles.Count -ne $supportedFiles.Count -or (Compare-Object ($supportedFiles | Sort-Object) ($ffmpegFiles | Sort-Object))) {
        throw "SDK refresh supports only the complete reviewed FFmpeg 8.1.3 DLL inventory; create a full package for changed dependency versions."
    }
    if (-not $manifest.ffmpeg.ContainsKey('sdkManifest')) { throw "SDK refresh requires recorded source lock identity; create a full package first." }
    foreach ($entry in $ffmpegRefreshPlan.files) { if ($entry.replace) { $refreshedFfmpegFiles[$entry.name] = $entry.selectedSha256 } }
} else {
    $null = Assert-FfmpegRuntimeIdentity -SdkRoot $ffmpegSdk.root -RuntimeDirectory $source -FileNames $ffmpegFiles
}
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
        throw "Source package lacks the required validated model: $relative. Run package-native-local.ps1 first."
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
    if (Test-NativeRelativePathExcluded $relative $excludedModelRoots $modelSelection) { continue }
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
$optionalPacks = @(Get-NativeOptionalRestorationPacks $manifest $source $TemporalRestorationModelDirectory $CompressionRestorationModelDirectory $modelSelection)
$additionalModelExpected = @{}
foreach ($optional in @($optionalPacks | Where-Object { -not $_.inSource })) {
    foreach ($file in $optional.pack.files) {
        $relative = $optional.relative + "/" + $file.FullName.Substring($optional.pack.source.Length).TrimStart('\', '/').Replace('\', '/')
        $additionalModelExpected[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $additionalModelExpected[$optional.licenseRelative] = (Get-FileHash -LiteralPath $optional.pack.licensePath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$expected = Read-NativeSourceChecksums $sumFile $excludedModelRoots $modelSelection
$files = @(Get-SafeFiles $source $excludedModelRoots $source $modelSelection)
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

# A source package's own checksums do not bind it to this build. Compare its
# complete ORT/TRT/CUDA DLL inventory and version sidecar to the build reference.
Assert-NativeInferenceRuntimeIdentity ([pscustomobject]@{
    cache = (Read-NativePackageBuildCache $build)
    inferenceRuntimeDirectory = (Join-Path $source 'runtime')
})

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
foreach ($name in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
    $selectedCompilerRuntime = Join-Path $build $name
    $sourceCompilerRuntime = Join-Path $source $name
    if (-not (Test-Path -LiteralPath $selectedCompilerRuntime -PathType Leaf) -or
        -not (Test-Path -LiteralPath $sourceCompilerRuntime -PathType Leaf) -or
        (Get-FileHash -LiteralPath $selectedCompilerRuntime -Algorithm SHA256).Hash -cne
        (Get-FileHash -LiteralPath $sourceCompilerRuntime -Algorithm SHA256).Hash) {
        throw "Compiler runtime identity differs for ${name}; create a full package instead of reusing incompatible DLLs."
    }
}
$dependencyImports = Get-Content -Raw -LiteralPath (Join-Path $source 'dependency-imports.json') | ConvertFrom-Json -AsHashtable
$dependencyImports.native = $newImports
$dependencyImports.ffmpeg = [ordered]@{}
foreach ($name in $ffmpegFiles) {
    $selectedImports = @(Get-Imports (Join-Path $ffmpegSdk.root "bin/$name"))
    $sourceImports = @(Get-Imports (Join-Path $source $name))
    if ($selectedImports.Count -eq 0 -or (Compare-Object $sourceImports $selectedImports)) {
        throw "Selected FFmpeg DLL dependencies differ for ${name}; create a full package instead of a configuration refresh."
    }
    $dependencyImports.ffmpeg[$name] = $selectedImports
}
Write-Output "FFmpeg refresh plan: $($refreshedFfmpegFiles.Count) changed DLLs; $($ffmpegRefreshPlan.replacementBytes) bytes; verified models/inference runtime remain hard linked."

$stage = "$target.staging-$([guid]::NewGuid().ToString('N'))"
if (-not $stage.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Invalid staging path." }
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($source.Length).TrimStart('\', '/').Replace('\', '/')
        if ($BaseModelsOnly -and $relative -ceq 'models/default-model.txt') { continue }
        if ($relative -in @("SHA256SUMS.txt", "PACKAGE-MANIFEST.json", "RifeLoveNative.exe", "README-本机原生包.txt", "config/defaults.json", "dependency-imports.json", "portable.flag", "portable-mode.txt")) { continue }
        if ($relative -match '(?i)(^|/)qt|qml|(^|/)rifelove-cli(\.exe)?$') { throw "Unexpected Qt or CLI artifact: $relative" }
        $destination = Join-Path $stage ($relative.Replace('/', '\'))
        if ($refreshedFfmpegFiles.ContainsKey($relative)) {
            Copy-Item -LiteralPath (Join-Path $ffmpegSdk.root "bin/$relative") -Destination $destination
        } else { New-HardLinkChecked $file.FullName $destination }
    }
    foreach ($optional in @($optionalPacks | Where-Object { -not $_.inSource })) {
        foreach ($file in $optional.pack.files) {
            $relative = $optional.relative + "/" + $file.FullName.Substring($optional.pack.source.Length).TrimStart('\', '/').Replace('\', '/')
            New-HardLinkChecked $file.FullName (Join-Path $stage ($relative.Replace('/', '\')))
        }
        New-HardLinkChecked $optional.pack.licensePath (Join-Path $stage $optional.licenseRelative)
    }
    New-Item -ItemType Directory -Path (Join-Path $stage "data") | Out-Null
    $dataMode = Initialize-NativePackageDataMode -PackageRoot $stage -PortableMode:$PortableMode
    Copy-Item -LiteralPath $exe -Destination (Join-Path $stage "RifeLoveNative.exe")
    $dependencyImports | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $stage 'dependency-imports.json') -Encoding utf8NoBOM

    $defaultsPath = Join-Path $source "config/defaults.json"
    if (-not (Test-Path -LiteralPath $defaultsPath -PathType Leaf)) { throw "Source package defaults are missing: $defaultsPath" }
    $defaults = Get-Content -Raw -LiteralPath $defaultsPath | ConvertFrom-Json -AsHashtable
    Set-NativeRestorationDefaults $defaults (@($optionalPacks | Where-Object { $_.relative -ceq "models/stdf-r3-y7-sdr8-local-eval" }).Count -eq 1) (@($optionalPacks | Where-Object { $_.relative -ceq "models/fastdvdnet-clipped-five-frame-sdr8-eval" }).Count -eq 1)
    $defaults['modelBundleMode'] = if ($BaseModelsOnly) { 'base' } else { 'compatible-source' }
    if ($BaseModelsOnly) {
        $defaults['defaultModelDirectory'] = 'models/rife-4.25-fp32-sdr8-v3'
        $defaults['legacyModelDirectory'] = $null
        $defaults['legacyModelIncluded'] = $false
        Set-Content -LiteralPath (Join-Path $stage 'models/default-model.txt') -Value 'rife-4.25-fp32-sdr8-v3' -Encoding utf8NoBOM
        $additionalModelExpected['models/default-model.txt'] = (Get-FileHash -LiteralPath (Join-Path $stage 'models/default-model.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $defaults["ffmpeg"] = "Isolated FFmpeg 8.1.3 with NVENC, OpenH264, Kvazaar and libaom; x86asm enabled=$($ffmpegSdk.configuration.x86asmEnabled); workload-specific throughput requires validation"
    Set-NativePackageDataModeMetadata -Metadata $defaults -DataMode $dataMode
    New-Item -ItemType Directory -Path (Join-Path $stage "config") -Force | Out-Null
    $defaults | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $stage "config/defaults.json") -Encoding utf8NoBOM

    foreach ($entry in @($manifest.models)) {
        $entryRelative = ([string]$entry.directory).Replace('\', '/')
        $entryRoot = Join-Path $stage ($entryRelative.Replace('/', '\'))
        $entryDescriptor = Get-Content -Raw -LiteralPath (Join-Path $entryRoot "model.json") | ConvertFrom-Json
        $entry.licenseId = if ($entryDescriptor.license -is [string]) { [string]$entryDescriptor.license } else { [string]$entryDescriptor.license.id }
        $entry.validatedBackends = @($entryDescriptor.validation.validatedBackends)
        $entry.role = Get-NativeModelRole $entryRelative
    }
    Update-NativeOptionalRestorationManifest $manifest $optionalPacks
    $manifest.inferenceRuntime.requiredFiles = $requiredRuntimeFiles
    $manifest.inferenceRuntime.providers = @("TensorRT", "CUDA", "CPU-fallback")
    $manifest.storage = "local-only; no Alibaba OSS"
    Set-NativePackageDataModeMetadata -Metadata $manifest -DataMode $dataMode -Manifest
    $manifest.ffmpeg.root = "local-sdk:ffmpeg"
    $manifest.ffmpeg.source = "local SDK selected by the native build"
    $manifest.ffmpeg.configuration = $ffmpegSdk.configuration
    $manifest.ffmpeg.linkInputs = $ffmpegSdk.linkInputs
    $manifest.ffmpeg.runtimeSha256 = $ffmpegRuntimeHashes
    $manifest.ffmpeg.configurationRefresh = [ordered]@{ requested = [bool]$RefreshFfmpegRuntime; replacedFiles = @($refreshedFfmpegFiles.Keys | Sort-Object); addedRuntimeBytes = $ffmpegRefreshPlan.replacementBytes; sourceBundleUnchanged = $true }
    $manifest.ffmpeg.x86asm = if ($ffmpegSdk.configuration.x86asmEnabled) { "enabled; end-to-end throughput remains workload-specific" } else { "disabled; CPU throughput remains unqualified" }
    $manifest.inferenceRuntime.source = "local TensorRT/ONNX Runtime CUDA SDK"
    $manifest.packageName = Split-Path -Leaf $target
    $manifest.modelBundleMode = if ($BaseModelsOnly) { 'base' } else { 'compatible-source' }
    $manifest.generatedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
    $manifest.executableSha256 = (Get-FileHash -LiteralPath (Join-Path $stage "RifeLoveNative.exe") -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest.ffmpeg.sdkManifest = [ordered]@{ path = $ffmpegSdk.manifestRelativePath; sha256 = $ffmpegSdk.manifestSha256; nvencApiMajor = $ffmpegSdk.nvencApiMajor; nvencApiMinor = $ffmpegSdk.nvencApiMinor; nvencMinimumWindowsDriver = $ffmpegSdk.nvencMinimumWindowsDriver }
    $manifest.updatePolicy = "local preview; verified identical dependencies share NTFS hard links; explicit FFmpeg configuration refresh copies only changed SDK DLLs; source bundle is never modified"
    $manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $stage "PACKAGE-MANIFEST.json") -Encoding utf8NoBOM
    $dataModeDescription = if ($PortableMode) {
        '此包已显式启用便携模式：portable-mode.txt 为永久保留且纳入校验清单的 RifeLovePortable 1 标记。正常可写时，任务数据库与模型偏好使用本包独立的 data 目录；打包时该目录为空，不复制旧包或 Windows 用户目录的数据。请保留标记，并在界面详情中核对实际用户数据目录。现有程序若发现标记无效或 data 不可写，会回退到 Windows 用户数据目录；出现回退时不能将该次运行视为隔离验收。'
    } else {
        '此普通包未启用便携模式，未捆绑 portable-mode.txt。任务数据库与本机模型偏好使用 Windows 当前用户的 %LOCALAPPDATA%\RifeLove；本包 data 为空。需要专用隔离便携包时，请在新输出目录打包并显式传入 -PortableMode。'
    }
    $includedModelsDescription = (@($manifest.models | ForEach-Object { [string]$_.directory }) -join '；')
    $temporalDescription = if (@($optionalPacks | Where-Object { $_.relative -ceq 'models/fastdvdnet-clipped-five-frame-sdr8-eval' }).Count -eq 1) {
        '包内 FastDVDnet 五帧模型仅是 SDR8 实验性时序降噪模型，默认关闭；它可与超分同时选择，但不代表通用压缩修复、去模糊或细节增强能力。'
    } else { '本包未捆绑 FastDVDnet，相关默认路径为空。' }
    $compressionDescription = if (@($optionalPacks | Where-Object { $_.relative -ceq 'models/stdf-r3-y7-sdr8-local-eval' }).Count -eq 1) {
        '包内 STDF-R3 七帧模型仅供本地压缩修复评估，默认关闭；输入/尺寸/分块契约以模型描述和开发验证记录为准，不代表通用压缩修复或 Topaz 级质量。'
    } else { '本包未捆绑 STDF-R3，相关默认路径为空。' }
    @"
RifeLove 原生 Windows 本机预览

双击 RifeLoveNative.exe。请保留此目录中的 runtime、models 和其他运行库。
这是本机预览，不是完整产品或可公开再分发的安装包。包含本机模型中心与用户数据目录迁移；完整最终产品、长视频恢复和通用动态压缩/细节修复仍需继续验收。
$dataModeDescription
视频输出位置由用户选择；此包不包含用户视频、队列、日志或模型偏好。
高帧率与长视频仍需更广泛的兼容性验收；失败时不会发布未通过验证的输出。TensorRT 初次加载可能较慢。
FFmpeg x86asm：$($manifest.ffmpeg.x86asm)。实际 SDK 配置与运行库 SHA-256 记录在 PACKAGE-MANIFEST.json；复用包和新 EXE 旁的 FFmpeg DLL 必须与所选 SDK 逐字节匹配。
默认使用 RIFE 4.25 插帧模型；Real-ESRGAN x2plus 超分可选，默认关闭。
实际打包模型：$includedModelsDescription。
$temporalDescription
$compressionDescription
此目录与旧本机预览使用 NTFS 硬链接复用不可变依赖，删除旧目录不会使本目录失效；直接修改共享依赖会同时影响两个目录。
本次 FFmpeg 配置刷新：$($refreshedFfmpegFiles.Count) 个不同 DLL 独立复制（$($ffmpegRefreshPlan.replacementBytes) 字节）；旧包文件不被覆盖，模型和推理运行库保留校验后的硬链接。
模型、视频和任务留在本机；阿里云 OSS 已停用。模型及 NVIDIA 运行库的公开再分发许可仍需审核。
"@ | Set-Content -LiteralPath (Join-Path $stage "README-本机原生包.txt") -Encoding utf8NoBOM
    $lines = @("RifeLove native local bundle SHA-256")
    foreach ($file in @(Get-SafeFiles $stage | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($stage.Length).TrimStart('\', '/').Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $knownHash = if ($relative -ceq $dataMode.markerName) { $dataMode.markerSha256 } elseif ($refreshedFfmpegFiles.ContainsKey($relative)) { $refreshedFfmpegFiles[$relative] } elseif ($additionalModelExpected.ContainsKey($relative)) { $additionalModelExpected[$relative] } elseif ($expected.ContainsKey($relative)) { $expected[$relative] } else { $null }
        if ($relative -notin @("PACKAGE-MANIFEST.json", "RifeLoveNative.exe", "README-本机原生包.txt", "config/defaults.json", "dependency-imports.json") -and
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
