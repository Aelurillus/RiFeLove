[CmdletBinding()]
param(
    [string]$SourcePackage = "",
    [string]$OutputDirectory = "",
    [string]$ArchivePath = "",
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot "../.."))
$outRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "out"))
$today = [DateTime]::Now.ToString("yyyy-MM-dd")
if ([string]::IsNullOrWhiteSpace($SourcePackage)) { $SourcePackage = Join-Path $outRoot "RifeLove-local-$today" }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $outRoot "RifeLove-offline-$today" }
if ([string]::IsNullOrWhiteSpace($ArchivePath)) { $ArchivePath = Join-Path $outRoot "RifeLove-offline-$today-win-x64.zip" }

$source = [IO.Path]::GetFullPath($SourcePackage)
$target = [IO.Path]::GetFullPath($OutputDirectory)
$archive = [IO.Path]::GetFullPath($ArchivePath)
foreach ($path in @($source, $target, $archive)) {
    if (-not $path.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Source and output paths must stay inside the repository out directory."
    }
}
if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Source package was not found: $source" }
if (Test-Path -LiteralPath $target) { throw "Refusing to replace an existing offline package: $target" }
if (Test-Path -LiteralPath $archive) { throw "Refusing to replace an existing offline archive: $archive" }

$runtimePath = Join-Path $source "runtime"
$runtimeItem = Get-Item -LiteralPath $runtimePath -Force
$runtimePrefix = $runtimePath + [IO.Path]::DirectorySeparatorChar
if ($runtimeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
    if ($runtimeItem.LinkType -ne "Junction") {
        throw "Expected the local package's runtime path to be a directory or its known workstation junction."
    }
    $runtimeSource = [IO.Path]::GetFullPath([string]$runtimeItem.Target)
    if (-not (Test-Path -LiteralPath $runtimeSource -PathType Container)) { throw "The linked inference runtime folder is unavailable." }
} elseif ($runtimeItem.PSIsContainer) {
    $runtimeSource = $runtimePath
} else {
    throw "The local package's runtime path is not a directory."
}

$sourceItems = @(Get-ChildItem -LiteralPath $source -Force -Recurse)
foreach ($item in $sourceItems) {
    if ($item.FullName -ieq $runtimePath -or $item.FullName.StartsWith($runtimePrefix, [StringComparison]::OrdinalIgnoreCase)) { continue }
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "The source package contains an unexpected link or junction: $($item.Name)"
    }
}
$runtimeItems = @(Get-ChildItem -LiteralPath $runtimeSource -Force -Recurse)
if ($runtimeItems | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
    throw "The inference runtime contains a link; refusing to copy an unbounded runtime tree."
}

$requiredRootFiles = @("RifeLove.exe", "rifelove-cli.exe", "portable.flag", "SHA256SUMS.txt")
foreach ($name in $requiredRootFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $name) -PathType Leaf)) { throw "The source package is missing $name" }
}
$modelFiles = @(Get-ChildItem -LiteralPath (Join-Path $source "models") -Filter "model.onnx" -File -Recurse -ErrorAction Stop)
if (-not $modelFiles.Count) { throw "The source package has no installed RIFE model weights." }
foreach ($name in @("onnxruntime.dll", "nvinfer_10.dll", "nvinfer_builder_resource_10.dll")) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeSource $name) -PathType Leaf)) { throw "The inference runtime is missing $name" }
}
if ($ValidateOnly) {
    Write-Output "Source package validation passed: $($modelFiles.Count) RIFE model pack(s), local inference runtime resolved, no unexpected links, local data excluded from the copy plan."
    exit 0
}

$archiveParent = Split-Path -Parent $archive
New-Item -ItemType Directory -Path $target | Out-Null
New-Item -ItemType Directory -Path $archiveParent -Force | Out-Null
$partialArchive = Join-Path $archiveParent (".RifeLove-offline-{0}.partial.zip" -f [Guid]::NewGuid().ToString("N"))
$promoted = $false
try {
    foreach ($item in $sourceItems) {
        if ($item.FullName -ieq $runtimePath -or $item.FullName.StartsWith($runtimePrefix, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $relative = $item.FullName.Substring($source.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
        $parts = $relative -split '[\\/]'
        if ($parts[0] -ieq "data" -or $relative -ieq "first-run.png" -or $relative -ieq "studio-smoke.png" -or
            $relative -ieq "README-本机运行.txt" -or $relative -ieq "SHA256SUMS.txt") { continue }
        if (-not $item.PSIsContainer -and [IO.Path]::GetExtension($item.Name) -match '^\.(pdb|lib|exp)$') { continue }
        $destination = Join-Path $target $relative
        if ($item.PSIsContainer) {
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
        } else {
            $destinationParent = Split-Path -Parent $destination
            New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
            Copy-Item -LiteralPath $item.FullName -Destination $destination
        }
    }

    $runtimeTarget = Join-Path $target "runtime"
    New-Item -ItemType Directory -Path $runtimeTarget | Out-Null
    foreach ($item in $runtimeItems) {
        $relative = $item.FullName.Substring($runtimeSource.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
        if (-not $relative -or [IO.Path]::GetExtension($item.Name) -match '^\.(pdb|lib|exp)$') { continue }
        $destination = Join-Path $runtimeTarget $relative
        if ($item.PSIsContainer) {
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
        } else {
            $destinationParent = Split-Path -Parent $destination
            New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
            Copy-Item -LiteralPath $item.FullName -Destination $destination
        }
    }

    $readme = @"
RifeLove offline Windows x64 bundle

Start the application with RifeLove.exe. The package contains the local desktop app, RIFE model packs, FFmpeg, and CUDA/ONNX Runtime/TensorRT runtime files. It requires a compatible NVIDIA GPU and driver.

This is a private local-use bundle generated from the current workstation package. It is not an approved public release: cross-device qualification and third-party redistribution review are still pending. Video files and task history are not included; new settings and queue data are created locally on first launch.
"@
    Set-Content -LiteralPath (Join-Path $target "使用说明.txt") -Value $readme -Encoding UTF8

    $hashLines = @("RifeLove offline bundle SHA-256")
    foreach ($file in (Get-ChildItem -LiteralPath $target -File -Recurse | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($target.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $hashLines += "$hash  $relative"
    }
    [IO.File]::WriteAllLines(
        (Join-Path $target "SHA256SUMS.txt"),
        [string[]]$hashLines,
        [Text.UTF8Encoding]::new($false)
    )
    if (Get-ChildItem -LiteralPath $target -Force -Recurse | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw "The offline package still contains a linked file or directory."
    }

    $privateExtensions = @(".sqlite", ".sqlite3", ".db", ".log", ".jsonl")
    $mediaExtensions = @(".3gp", ".aac", ".avi", ".flac", ".flv", ".m2ts", ".m4a", ".m4v", ".mkv", ".mov", ".mp3", ".mp4", ".mpeg", ".mpg", ".mts", ".ogg", ".ts", ".vob", ".wav", ".webm", ".wmv")
    $privateNames = @("language.ini", "preferences.ini", "queue.sqlite-wal", "queue.sqlite-shm", "first-run.png", "studio-smoke.png")
    $unsafePackageItems = @(
        foreach ($item in (Get-ChildItem -LiteralPath $target -Force -Recurse)) {
            $relative = $item.FullName.Substring($target.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
            $segments = @($relative.Replace('\', '/').Split('/') | Where-Object { $_ })
            $lowerSegments = @($segments | ForEach-Object { $_.ToLowerInvariant() })
            $extension = [IO.Path]::GetExtension($item.Name).ToLowerInvariant()
            $baseName = $item.Name.ToLowerInvariant()
            if ($lowerSegments -contains "data" -or $lowerSegments -contains "logs" -or
                $privateExtensions -contains $extension -or $mediaExtensions -contains $extension -or
                $privateNames -contains $baseName -or $extension -in @(".pem", ".p12", ".pfx") -or
                $baseName -eq ".env" -or $baseName.StartsWith(".env.")) {
                $relative
            }
        }
    )
    if ($unsafePackageItems.Count) {
        throw "The offline package contains local data, media, logs, or credentials: $($unsafePackageItems -join ', ')"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($target, $partialArchive, [IO.Compression.CompressionLevel]::Optimal, $false)
    if (-not (Test-Path -LiteralPath $partialArchive -PathType Leaf) -or (Get-Item -LiteralPath $partialArchive).Length -le 0) {
        throw "The offline archive was not created correctly."
    }
    Move-Item -LiteralPath $partialArchive -Destination $archive
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    $archiveSize = (Get-Item -LiteralPath $archive).Length
    $promoted = $true
    Write-Output "Offline package directory: $target"
    Write-Output "Offline archive: $archive"
    Write-Output "Archive size: $archiveSize bytes"
    Write-Output "Archive SHA-256: $archiveHash"
} finally {
    if (Test-Path -LiteralPath $partialArchive) {
        $resolvedPartial = [IO.Path]::GetFullPath($partialArchive)
        if ($resolvedPartial.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedPartial) -like ".RifeLove-offline-*.partial.zip") {
            Remove-Item -LiteralPath $resolvedPartial -Force
        }
    }
    if (-not $promoted -and (Test-Path -LiteralPath $target)) {
        $resolvedTarget = [IO.Path]::GetFullPath($target)
        if ($resolvedTarget.StartsWith($outRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedTarget) -eq (Split-Path -Leaf $target) -and
            -not ((Get-Item -LiteralPath $resolvedTarget -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
        }
    }
}
