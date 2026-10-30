[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$FfmpegRoot = "",
    [string]$InferenceRuntimeDirectory = "",
    [string]$MinGWBinDirectory = "",
    [string]$CodecLicenseRoot = "",
    [string]$RifeModelDirectory = "",
    [string]$LegacyRifeModelDirectory = "",
    [string]$SuperResolutionModelDirectory = "",
    [string]$TemporalRestorationModelDirectory = "",
    [string]$OutputDirectory = "",
    [switch]$SkipStartupSmoke
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot "../.."))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "out"))

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $repositoryRoot "build/windows-release" }
if ([string]::IsNullOrWhiteSpace($FfmpegRoot)) { $FfmpegRoot = Join-Path $repositoryRoot "work/codec-ffmpeg-build/ffmpeg-install" }
if ([string]::IsNullOrWhiteSpace($InferenceRuntimeDirectory)) { $InferenceRuntimeDirectory = "C:/itzming/dependences/rifelove-tensorrt-10.9.0.34/onnxruntime-win-x64-gpu-1.22.0/lib" }
if ([string]::IsNullOrWhiteSpace($MinGWBinDirectory)) { $MinGWBinDirectory = "C:/Users/Administrator/scoop-toolchain/apps/mingw/current/bin" }
if ([string]::IsNullOrWhiteSpace($CodecLicenseRoot)) { $CodecLicenseRoot = Join-Path $repositoryRoot "work/codec-ffmpeg-build" }
if ([string]::IsNullOrWhiteSpace($RifeModelDirectory)) { $RifeModelDirectory = Join-Path $repositoryRoot "models/cache/rife-4.25-fp32-sdr8-v3" }
if ([string]::IsNullOrWhiteSpace($LegacyRifeModelDirectory)) { $LegacyRifeModelDirectory = Join-Path $repositoryRoot "models/cache/rife-4.25-fp32-sdr8-v2" }
if ([string]::IsNullOrWhiteSpace($SuperResolutionModelDirectory)) { $SuperResolutionModelDirectory = Join-Path $repositoryRoot "models/cache/realesrgan-x2plus-sdr8-candidate-v2" }
if ([string]::IsNullOrWhiteSpace($TemporalRestorationModelDirectory)) { $TemporalRestorationModelDirectory = Join-Path $repositoryRoot "models/cache/fastdvdnet-clipped-five-frame-sdr8-eval" }
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
    [pscustomobject]@{
        source = $Directory; destination = $DestinationName; descriptor = $descriptor; id = $id; family = [string]$descriptor.family;
        graphHash = $graphHash; licenseFile = $licenseFile; licensePath = $licensePath; legacy = $Legacy
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
$packageLeaf = Split-Path -Path $target -Leaf
if ($packageLeaf -notmatch '^RifeLoveNative-local(?:$|[-.])') { throw "为避免与 Qt 产物冲突，原生包目录必须以 RifeLoveNative-local 开头：$target" }
$packageParent = Split-Path -Path $target -Parent
$staging = Join-Path $packageParent (".{0}.staging-{1}" -f $packageLeaf, [Guid]::NewGuid().ToString("N"))
$backup = Join-Path $packageParent (".{0}.previous-{1}" -f $packageLeaf, [Guid]::NewGuid().ToString("N"))

$build = Resolve-ExistingDirectory $BuildDirectory "原生构建"
$ffmpeg = Resolve-ExistingDirectory $FfmpegRoot "FFmpeg"
$runtime = Resolve-ExistingDirectory $InferenceRuntimeDirectory "ONNX/TensorRT/CUDA/cuDNN 运行库"
$mingw = Resolve-ExistingDirectory $MinGWBinDirectory "MinGW 运行库"
$nativeExe = Resolve-ExistingFile (Join-Path $build "RifeLoveNative.exe") "非 Qt 原生 EXE"
$ffmpegSdk = Read-FfmpegSdkIdentity $build $ffmpeg
Assert-NoLinkTree $ffmpeg
Assert-NoLinkTree $runtime
Assert-NoLinkTree $mingw

$ffmpegFiles = @(
    "avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swresample-6.dll", "swscale-9.dll",
    "avfilter-11.dll", "avdevice-62.dll", "libaom.dll", "libkvazaar.dll", "libopenh264-8.dll"
)
foreach ($name in $ffmpegFiles) { Resolve-ExistingFile (Join-Path $ffmpeg "bin/$name") "FFmpeg 运行库" | Out-Null }
$runtimeFiles = @(Get-ChildItem -LiteralPath $runtime -File -Filter "*.dll" | Sort-Object Name)
$requiredRuntimeFiles = @(
    # ONNX Runtime provider and core
    "onnxruntime.dll", "onnxruntime_providers_shared.dll", "onnxruntime_providers_cuda.dll", "onnxruntime_providers_tensorrt.dll",
    # CUDA 12 runtime and math libraries
    "cudart64_12.dll", "cublas64_12.dll", "cublasLt64_12.dll", "cufft64_11.dll", "cufftw64_11.dll", "nvblas64_12.dll",
    # cuDNN 9 runtime and engines
    "cudnn64_9.dll", "cudnn_adv64_9.dll", "cudnn_cnn64_9.dll", "cudnn_engines_precompiled64_9.dll",
    "cudnn_engines_runtime_compiled64_9.dll", "cudnn_graph64_9.dll", "cudnn_heuristic64_9.dll", "cudnn_ops64_9.dll",
    # TensorRT 10 runtime, plugins, builder resource and ONNX parser
    "nvinfer_10.dll", "nvinfer_builder_resource_10.dll", "nvinfer_plugin_10.dll", "nvonnxparser_10.dll"
)
$runtimeFileNames = @($runtimeFiles | ForEach-Object Name | ForEach-Object { $_.ToLowerInvariant() })
foreach ($name in $requiredRuntimeFiles) {
    if ($runtimeFileNames -notcontains $name.ToLowerInvariant()) {
        throw "推理运行库缺少必需 DLL：$name；拒绝生成可能静默回退的本机包。"
    }
}
$versionFile = Join-Path $runtime "tensorrt-runtime.version"
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) { throw "缺少 TensorRT 运行库版本/校验侧车：$versionFile" }
$versionLines = @(Get-Content -LiteralPath $versionFile | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($versionLines.Count -lt 2 -or $versionLines[0] -notmatch '^\d+\.\d+\.\d+\.\d+$' -or $versionLines[1] -notmatch '^[0-9a-fA-F]{64}$') {
    throw "TensorRT 运行库版本侧车格式无效，必须包含版本和来源 SHA-256：$versionFile"
}
$mingwFiles = @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")
foreach ($name in $mingwFiles) { Resolve-ExistingFile (Join-Path $mingw $name) "MinGW 运行库" | Out-Null }
$msvcFiles = @("MSVCP140.dll", "MSVCP140_1.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll")
foreach ($name in $msvcFiles) { Resolve-ExistingFile (Join-Path $env:SystemRoot "System32/$name") "MSVC 运行库" | Out-Null }

$legacyDescriptorPath = Resolve-ExistingFile (Join-Path $LegacyRifeModelDirectory "model.json") "RIFE 4.25 旧版描述"
$legacyDescriptor = Read-Json $legacyDescriptorPath
$legacyModelHash = Get-DescriptorHash $legacyDescriptor "sha256" "RIFE 4.25 旧版"
$legacyReference = Find-LegacyModelReference $target $legacyModelHash
$includeLegacyModel = [bool]$legacyReference.referenced

$packs = @(
    Validate-ModelPack (Resolve-ExistingDirectory $RifeModelDirectory "RIFE 4.25 v3") "rife-4.25-fp32-sdr8-v3" "rife" $false
    if ($includeLegacyModel) {
        # The v2 basename is part of the checkpoint identity in the local
        # queue database. Include it only when migrated data contains the
        # exact path token and matching model identity.
        Validate-ModelPack (Resolve-ExistingDirectory $LegacyRifeModelDirectory "RIFE 4.25 旧版") "rife-4.25-fp32-sdr8-v2" "rife" $true
    }
    Validate-ModelPack (Resolve-ExistingDirectory $SuperResolutionModelDirectory "Real-ESRGAN x2plus v2") "realesrgan-x2plus-sdr8-v2" "video-restoration" $false
    Validate-ModelPack (Resolve-ExistingDirectory $TemporalRestorationModelDirectory "FastDVDnet 时序降噪评估包") "fastdvdnet-clipped-five-frame-sdr8-eval" "video-restoration" $false
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
}
if (Test-Path -LiteralPath $staging) { throw "随机 staging 路径已存在：$staging" }
New-Item -ItemType Directory -Path $staging | Out-Null
$promoted = $false
try {
    Copy-FileChecked $nativeExe (Join-Path $staging "RifeLoveNative.exe")
    foreach ($name in $ffmpegFiles) { Copy-FileChecked (Join-Path $ffmpeg "bin/$name") (Join-Path $staging $name) }
    foreach ($name in $mingwFiles) { Copy-FileChecked (Join-Path $mingw $name) (Join-Path $staging $name) }
    foreach ($name in $msvcFiles) { Copy-FileChecked (Join-Path $env:SystemRoot "System32/$name") (Join-Path $staging $name) }

    $runtimeTarget = Join-Path $staging "runtime"
    New-Item -ItemType Directory -Path $runtimeTarget | Out-Null
    foreach ($file in $runtimeFiles) { Copy-FileChecked $file.FullName (Join-Path $runtimeTarget $file.Name) }
    Copy-FileChecked $versionFile (Join-Path $runtimeTarget "tensorrt-runtime.version")

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
        restorationModelDirectory = "models/realesrgan-x2plus-sdr8-v2"; temporalRestorationModelDirectory = "models/fastdvdnet-clipped-five-frame-sdr8-eval"; temporalRestorationDefaultEnabled = $false; temporalRestorationMode = "experimental-optional"; targetRate = "60/1"; encoder = "auto-h264-nvenc-with-software-fallback"
        ffmpeg = "Isolated FFmpeg 8.1.3 with NVENC, OpenH264, Kvazaar and libaom; CPU build has no x86asm optimization"; dataDirectory = "data"
        legacyModelIncluded = $includeLegacyModel
    }
    $defaults | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $configTarget "defaults.json") -Encoding utf8NoBOM
    New-Item -ItemType Directory -Path (Join-Path $staging "data") | Out-Null
    Set-Content -LiteralPath (Join-Path $staging "portable.flag") -Value "RifeLoveNative local package" -Encoding utf8NoBOM

    $licenseTarget = Join-Path $staging "licenses"
    New-Item -ItemType Directory -Path $licenseTarget | Out-Null
    Copy-FileChecked (Join-Path $repositoryRoot "LICENSE") (Join-Path $licenseTarget "RifeLove-MIT.txt")
    $ffmpegSourceRoot = Join-Path $CodecLicenseRoot "ffmpeg-8.1.3"
    Copy-FileChecked (Resolve-CodecLicense $CodecLicenseRoot "ffmpeg-8.1.3/LICENSE.md" "FFmpeg 许可") (Join-Path $licenseTarget "FFmpeg-LICENSE.md")
    Copy-FileChecked (Resolve-CodecLicense $CodecLicenseRoot "ffmpeg-8.1.3/COPYING.LGPLv2.1" "FFmpeg LGPL 许可") (Join-Path $licenseTarget "FFmpeg-LGPL-2.1.txt")
    $codecDepsRoot = Join-Path $repositoryRoot "work/codec-deps/buildtrees"
    Copy-FileChecked (Find-CodecSourceLicense $codecDepsRoot "openh264/src" "OpenH264") (Join-Path $licenseTarget "OpenH264-LICENSE.txt")
    Copy-FileChecked (Find-CodecSourceLicense $codecDepsRoot "kvazaar/src" "Kvazaar") (Join-Path $licenseTarget "Kvazaar-LICENSE.txt")
    Copy-FileChecked (Find-CodecSourceLicense $codecDepsRoot "aom/src" "libaom") (Join-Path $licenseTarget "libaom-LICENSE.txt")
    $nvCodecNotice = @(
        "This local build uses NVIDIA nv-codec-headers n12.2.72.0 for NVENC/NVDEC integration.",
        "The source archive SHA-256 and build configuration are recorded in work/codec-ffmpeg-build/BUILD_RECORD.md.",
        "Review NVIDIA Video Codec SDK terms before any redistribution."
    ) -join [Environment]::NewLine
    Set-Content -LiteralPath (Join-Path $licenseTarget "NVIDIA-nv-codec-headers-notice.txt") -Value $nvCodecNotice -Encoding utf8NoBOM
    $onnxRoot = Split-Path -Parent $runtime
    Copy-FileChecked (Join-Path $onnxRoot "LICENSE") (Join-Path $licenseTarget "ONNX-Runtime-MIT.txt")
    Copy-FileChecked (Join-Path $onnxRoot "ThirdPartyNotices.txt") (Join-Path $licenseTarget "ONNX-Runtime-ThirdPartyNotices.txt")
    $tensorRtRoot = Split-Path (Split-Path $runtime -Parent) -Parent
    Copy-FileChecked (Join-Path $tensorRtRoot "licenses/TensorRT-10.9-LICENSE.txt") (Join-Path $licenseTarget "TensorRT-10.9-LICENSE.txt")
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
        ffmpeg = [ordered]@{ source = "work/codec-ffmpeg-build/ffmpeg-install"; root = $ffmpeg; binaries = $ffmpegFiles; nvenc = "integrated in isolated SDK; validated H.264 NVENC and software fallback"; x86asm = "disabled; CPU throughput remains unqualified"; sdkManifest = [ordered]@{ path = $ffmpegSdk.manifestRelativePath; sha256 = $ffmpegSdk.manifestSha256; nvencApiMajor = $ffmpegSdk.nvencApiMajor; nvencApiMinor = $ffmpegSdk.nvencApiMinor; nvencMinimumWindowsDriver = $ffmpegSdk.nvencMinimumWindowsDriver } }
        inferenceRuntime = [ordered]@{ source = $runtime; files = @($runtimeFiles | ForEach-Object Name); requiredFiles = $requiredRuntimeFiles; tensorRtVersion = $versionLines[0]; sourceSha256 = $versionLines[1]; providers = @("TensorRT", "CUDA", "CPU-fallback") }
        models = @($packs | ForEach-Object { [ordered]@{ directory = "models/$($_.destination)"; id = $_.id; family = $_.family; version = [string]$_.descriptor.version; sha256 = $_.graphHash; licenseId = if ($_.descriptor.license -is [string]) { [string]$_.descriptor.license } else { [string]$_.descriptor.license.id }; validatedBackends = @($_.descriptor.validation.validatedBackends); legacy = $_.legacy; hiddenFromDefaultSelection = $_.legacy; canonicalBasename = $_.destination; role = if ($_.destination -eq "fastdvdnet-clipped-five-frame-sdr8-eval") { "experimental-optional-temporal-denoise" } elseif ($_.destination -eq "realesrgan-x2plus-sdr8-v2") { "optional-super-resolution" } else { "default-interpolation" }; redistribution = if ($_.legacy) { "local-legacy-required-by-verified-migration-reference" } else { "local-only-until-license-review" } } })
        migration = [ordered]@{ legacyModelIncluded = $includeLegacyModel; legacyModelPathToken = $legacyReference.token; legacyModelSha256 = $legacyModelHash; evidence = @($legacyReference.evidence); cleanPackageOmitsUnreferencedLegacy = $true }
        defaults = "config/defaults.json"; dataDirectory = "data"; updatePolicy = "transactional; refuse running target; preserve data"; storage = "local-only; no Alibaba OSS"
    }
    $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $staging "PACKAGE-MANIFEST.json") -Encoding utf8NoBOM
    $readme = @"
RifeLove 原生 Windows 本机包

启动：双击 RifeLoveNative.exe。此目录是 Win32/Direct2D/DirectWrite 原生桌面包，不包含 Qt、QML 或 CLI。
默认模型：models/rife-4.25-fp32-sdr8-v3；models/realesrgan-x2plus-sdr8-v2 为 Real-ESRGAN x2plus v2 本机评估权重；models/fastdvdnet-clipped-five-frame-sdr8-eval 为 FastDVDnet 五帧时序降噪本机评估权重，仅支持已声明的 SDR8 源率降噪。
视频、任务、设置和日志只保存在本机 data 目录。刷新本机包时，已有 data 和用户添加的模型会保留；发布包和 SHA256SUMS.txt 不包含 data。
当前包使用隔离构建的 FFmpeg 运行库，包含 H.264 NVENC、OpenH264、Kvazaar 和 libaom；自动编码优先使用 H.264 NVENC，并在不可用时软件回退。该构建关闭 x86asm，CPU 编码性能尚未完成资格确认。
TensorRT、ONNX Runtime CUDA、CUDA、cuDNN 和 FFmpeg DLL 随包放置。仍需要兼容的 NVIDIA 驱动；首次推理可能生成本机 TensorRT 缓存。
只有刷新时发现 data 中存在带精确 v2 路径 token、模型 ID、版本和 SHA-256 的队列/检查点记录，包才会额外保留 models/rife-4.25-fp32-sdr8-v2；干净包不会捆绑未被实际迁移数据引用的重复 RIFE 模型。
依赖来源、导入清单、模型 SHA-256、许可证和限制见 PACKAGE-MANIFEST.json、dependency-imports.json、SHA256SUMS.txt 和 licenses。
"@
    Set-Content -LiteralPath (Join-Path $staging "README-本机原生包.txt") -Value $readme -Encoding utf8NoBOM

    Copy-UserState $target $staging $bundledModelDirectories $bundledModelAliases $legacyModelHash
    if (-not (Test-Path -LiteralPath (Join-Path $staging "models/default-model.txt") -PathType Leaf)) {
        Set-Content -LiteralPath (Join-Path $staging "models/default-model.txt") -Value "rife-4.25-fp32-sdr8-v3" -Encoding utf8NoBOM
    }
    if (-not $SkipStartupSmoke) { Invoke-StartupSmoke $staging }

    if (Get-PackageTreeItems $staging | Where-Object { $_.Name -match '(?i)(^|-)rifelove-cli(\.exe)?$|(^|-)cli(\.exe)?$' }) { throw "包内出现 CLI 文件，拒绝交付。" }
    if (Get-PackageTreeItems $staging | Where-Object { $_.Name -match '(?i)^qt|qt[0-9]|qml' }) { throw "包内出现 Qt/QML 文件，拒绝交付。" }
    Write-Sha256Manifest $staging

    Assert-PackageNotRunning $target
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
