# Read the selected SDK's actual tool configuration, not a source lock shared
# by several builds. This file has no packaging side effects when dot-sourced.
function Assert-FfmpegBuildLinkIdentity {
    param([Parameter(Mandatory=$true)][string]$BuildDirectory,
          [Parameter(Mandatory=$true)][string]$SdkRoot)
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) { throw "FFmpeg build cache missing: $cachePath" }
    $cache = @{}
    foreach ($line in [IO.File]::ReadAllLines($cachePath)) {
        if ($line -match '^([^/#:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    $root = [IO.Path]::GetFullPath($SdkRoot).TrimEnd('\','/')
    $include = [string]$cache['FFmpeg_INCLUDE_DIR']
    $expectedInclude = Join-Path $root 'include'
    if ([string]::IsNullOrWhiteSpace($include) -or -not [IO.Path]::IsPathRooted($include) -or
        [IO.Path]::GetFullPath($include).TrimEnd('\','/') -ine $expectedInclude) {
        throw 'Mixed FFmpeg SDK: FFmpeg_INCLUDE_DIR must point to the selected SDK include directory. Clear FFmpeg_* cache entries and reconfigure/rebuild.'
    }
    $paths = @((Join-Path $root 'include/libavformat/avformat.h'))
    $libraries = [ordered]@{}
    foreach ($component in @('avformat','avcodec','avutil','swscale')) {
        $key = "FFmpeg_${component}_LIBRARY"
        $library = [string]$cache[$key]
        if ([string]::IsNullOrWhiteSpace($library) -or -not [IO.Path]::IsPathRooted($library) -or
            [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($library)) -ine (Join-Path $root 'lib')) {
            throw "Mixed FFmpeg SDK: $key must be inside the selected SDK lib directory. Clear FFmpeg_* cache entries and reconfigure/rebuild."
        }
        $paths += [IO.Path]::GetFullPath($library)
        $libraries[$component] = [ordered]@{ relativePath = 'lib/' + [IO.Path]::GetFileName($library); sha256 = $null }
    }
    foreach ($path in $paths) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "FFmpeg link input missing: $path" }
        $cursor = $path
        while ($cursor.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "FFmpeg link input ancestry cannot contain a link: $cursor"
            }
            if ($cursor -ieq $root) { break }
            $cursor = [IO.Path]::GetDirectoryName($cursor)
        }
    }
    foreach ($component in $libraries.Keys) {
        $libraries[$component].sha256 = (Get-FileHash -LiteralPath (Join-Path $root $libraries[$component].relativePath) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    [pscustomobject]@{
        headerSha256 = (Get-FileHash -LiteralPath $paths[0] -Algorithm SHA256).Hash.ToLowerInvariant()
        libraries = $libraries
    }
}

function ConvertFrom-FfmpegConfiguration {
    param([Parameter(Mandatory=$true)][string]$VersionOutput)
    $configuration = [regex]::Match($VersionOutput, '(?m)^configuration:\s*(.+?)\s*$')
    $version = [regex]::Match($VersionOutput, '(?m)^ffmpeg version\s+([^\r\n]+)')
    if (-not $configuration.Success -or -not $version.Success) {
        throw "FFmpeg tool did not report an unambiguous version and configuration."
    }
    $flags = $configuration.Groups[1].Value.Trim()
    $enabled = [regex]::IsMatch($flags, '(^|\s)--enable-x86asm(\s|$)')
    $disabled = [regex]::IsMatch($flags, '(^|\s)--disable-x86asm(\s|$)')
    if ($enabled -eq $disabled) { throw "FFmpeg configuration must explicitly select exactly one x86asm mode." }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($flags))).ToLowerInvariant() }
    finally { $sha.Dispose() }
    [pscustomobject]@{ version = $version.Groups[1].Value.Trim(); x86asmEnabled = $enabled; configurationSha256 = $hash }
}

function Read-FfmpegBuildConfiguration {
    param([Parameter(Mandatory=$true)][string]$SdkRoot)
    $tool = Join-Path $SdkRoot "bin/ffmpeg.exe"
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Selected FFmpeg SDK tool is missing: $tool" }
    if ((Get-Item -LiteralPath $tool -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Selected FFmpeg SDK tool cannot be a link: $tool"
    }
    $output = (& $tool -hide_banner -version 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "Selected FFmpeg SDK tool failed to report its configuration: $tool" }
    $identity = ConvertFrom-FfmpegConfiguration $output
    $identity | Add-Member -NotePropertyName toolSha256 -NotePropertyValue ((Get-FileHash -LiteralPath $tool -Algorithm SHA256).Hash.ToLowerInvariant())
    return $identity
}

function Assert-FfmpegRuntimeIdentity {
    param([Parameter(Mandatory=$true)][string]$SdkRoot,
          [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
          [Parameter(Mandatory=$true)][string[]]$FileNames)
    $hashes = [ordered]@{}
    if ($FileNames.Count -eq 0) { throw "FFmpeg runtime file list cannot be empty." }
    foreach ($name in $FileNames) {
        if ([string]::IsNullOrWhiteSpace($name) -or [IO.Path]::GetFileName($name) -cne $name -or $name -notmatch '(?i)\.dll$') {
            throw "Unsafe FFmpeg runtime file name: $name"
        }
        $reference = Join-Path $SdkRoot "bin/$name"
        $runtime = Join-Path $RuntimeDirectory $name
        foreach ($path in @($reference, $runtime)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "FFmpeg runtime DLL is missing: $path" }
            if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "FFmpeg runtime DLL cannot be a link: $path" }
        }
        $expected = (Get-FileHash -LiteralPath $reference -Algorithm SHA256).Hash.ToLowerInvariant()
        $actual = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -cne $expected) {
            throw "FFmpeg runtime DLL differs from the selected SDK: $name. Rebuild/restage the native build or create a full package instead of reusing old DLLs."
        }
        $hashes[$name] = $expected
    }
    return $hashes
}

function Get-FfmpegRuntimeRefreshPlan {
    param([Parameter(Mandatory=$true)][string]$SdkRoot,
          [Parameter(Mandatory=$true)][string]$SourceDirectory,
          [Parameter(Mandatory=$true)][string[]]$FileNames)
    $selectedHashes = Assert-FfmpegRuntimeIdentity -SdkRoot $SdkRoot -RuntimeDirectory (Join-Path $SdkRoot 'bin') -FileNames $FileNames
    $files = @()
    [long]$replacementBytes = 0
    foreach ($name in $FileNames) {
        $oldPath = Join-Path $SourceDirectory $name
        if (-not (Test-Path -LiteralPath $oldPath -PathType Leaf)) { throw "Source FFmpeg runtime DLL is missing: $oldPath" }
        if ((Get-Item -LiteralPath $oldPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source FFmpeg runtime DLL cannot be a link: $oldPath" }
        $oldHash = (Get-FileHash -LiteralPath $oldPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $replace = $oldHash -cne $selectedHashes[$name]
        $bytes = if ($replace) { (Get-Item -LiteralPath (Join-Path $SdkRoot "bin/$name")).Length } else { 0 }
        $replacementBytes += $bytes
        $files += [pscustomobject]@{ name = $name; replace = $replace; sourceSha256 = $oldHash; selectedSha256 = $selectedHashes[$name]; replacementBytes = $bytes }
    }
    [pscustomobject]@{ files = $files; selectedHashes = $selectedHashes; replacementBytes = $replacementBytes }
}
