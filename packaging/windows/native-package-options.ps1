# Explicit data-mode initialization for a newly staged native package. Never
# copies user state, removes a marker, or changes an existing package's mode.
function Assert-NativePackageDirectoriesDisjoint {
    param([Parameter(Mandatory=$true)][string]$SourceDirectory,
          [Parameter(Mandatory=$true)][string]$OutputDirectory)
    $source = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
    $target = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
    $separator = [IO.Path]::DirectorySeparatorChar
    if ([string]::Equals($source,$target,[StringComparison]::OrdinalIgnoreCase) -or
        $source.StartsWith($target + $separator,[StringComparison]::OrdinalIgnoreCase) -or
        $target.StartsWith($source + $separator,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Source and output package directories must be disjoint; neither may contain the other.'
    }
    foreach ($path in @($source,$target)) {
        $cursor = $path
        while ($cursor) {
            if ((Test-Path -LiteralPath $cursor) -and
                ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Package directory ancestry cannot contain a link: $cursor"
            }
            $parent = [IO.Path]::GetDirectoryName($cursor)
            if ($parent -eq $cursor) { break }
            $cursor = $parent
        }
    }
}

function Initialize-NativePackageDataMode {
    param([Parameter(Mandatory=$true)][string]$PackageRoot,
          [switch]$PortableMode)
    foreach ($directory in @($PackageRoot, (Join-Path $PackageRoot 'data'))) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) { throw "New package data directory is missing: $directory" }
        if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "New package data directory cannot be a link: $directory"
        }
    }
    $dataRoot = Join-Path $PackageRoot 'data'
    if (@(Get-ChildItem -LiteralPath $dataRoot -Force).Count -ne 0) {
        throw "Data mode can only be initialized for a new empty package data directory."
    }
    $marker = Join-Path $PackageRoot 'portable-mode.txt'
    if (Test-Path -LiteralPath $marker) {
        throw "New package must not inherit or overwrite an existing portable marker."
    }
    $markerHash = $null
    if ($PortableMode) {
        $contents = [Text.Encoding]::UTF8.GetBytes("RifeLovePortable 1`n")
        $file = [IO.File]::Open($marker,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        try { $file.Write($contents,0,$contents.Length); $file.Flush($true) }
        finally { $file.Dispose() }
        $markerHash = (Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    [pscustomobject]@{
        portableModeEnabled = [bool]$PortableMode
        dataDirectory = if ($PortableMode) { 'data' } else { '%LOCALAPPDATA%\RifeLove' }
        defaultDataDirectory = '%LOCALAPPDATA%\RifeLove'
        portableDataDirectory = 'data'
        markerName = 'portable-mode.txt'
        markerValue = 'RifeLovePortable 1'
        markerIncluded = [bool]$PortableMode
        markerSha256 = $markerHash
        dataState = 'new-empty-directory; no user queue, preferences or logs copied'
    }
}
