[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackageDirectory,
    [Parameter(Mandatory=$true)][ValidateSet('Normal','Portable')][string]$ExpectedMode,
    [switch]$RequireEmptyData,
    [switch]$VerifyAllImmutableFiles,
    [string]$ExpectedExecutable = ''
)

# Read-only package contract check. It does not launch an EXE, change the marker,
# touch Windows user data, create files or clean up any package/test artifacts.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PackageDirectory)
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Package directory missing: $root" }
function Assert-NotReparsePoint([string]$Path) {
    if ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Package path must not be a link: $Path"
    }
}
Assert-NotReparsePoint $root
foreach ($directory in @('data','config')) {
    $path = Join-Path $root $directory
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Package directory missing: $path" }
    Assert-NotReparsePoint $path
}
foreach ($name in @('PACKAGE-MANIFEST.json','config/defaults.json','README-本机原生包.txt','SHA256SUMS.txt','RifeLoveNative.exe')) {
    $path = Join-Path $root $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Package file missing: $name" }
    Assert-NotReparsePoint $path
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'PACKAGE-MANIFEST.json') -Raw | ConvertFrom-Json
$defaults = Get-Content -LiteralPath (Join-Path $root 'config/defaults.json') -Raw | ConvertFrom-Json
$portable = $ExpectedMode -ceq 'Portable'
$effectiveData = if ($portable) { 'data' } else { '%LOCALAPPDATA%\RifeLove' }
foreach ($metadata in @($manifest,$defaults)) {
    if ($metadata.portableModeEnabled -isnot [bool] -or $metadata.portableModeEnabled -ne $portable) {
        throw 'Metadata portable mode does not match the requested mode.'
    }
    if ($metadata.dataDirectory -cne $effectiveData -or
        $metadata.defaultDataDirectory -cne '%LOCALAPPDATA%\RifeLove' -or
        $metadata.portableDataDirectory -cne 'data' -or
        $metadata.portableModeMarker -cne 'portable-mode.txt' -or
        $metadata.portableModeMarkerValue -cne 'RifeLovePortable 1') {
        throw 'Package data-directory metadata is inconsistent.'
    }
}
if ($manifest.portableModeMarkerIncluded -isnot [bool] -or $manifest.portableModeMarkerIncluded -ne $portable) {
    throw 'Manifest portable marker inclusion is inconsistent.'
}

$sums = @{}
$sumPath = Join-Path $root 'SHA256SUMS.txt'
foreach ($line in [IO.File]::ReadAllLines($sumPath)) {
    if ($line -ceq 'RifeLove native local bundle SHA-256') { continue }
    if ($line -cnotmatch '^([a-fA-F0-9]{64})  (.+)$') { throw 'Invalid checksum manifest line.' }
    $hash = $Matches[1].ToLowerInvariant()
    $relative = $Matches[2]
    if ($relative.Contains('\') -or $relative.Contains(':') -or [IO.Path]::IsPathRooted($relative) -or
        @($relative.Split('/') | Where-Object { $_ -in @('','..','.') }).Count -ne 0 -or
        $relative -match '^(?i:data)/' -or $relative -ieq 'SHA256SUMS.txt') {
        throw "Unsafe or mutable checksum path: $relative"
    }
    if ($sums.ContainsKey($relative)) { throw "Duplicate checksum entry: $relative" }
    $sums[$relative] = $hash
}
$requiredHashes = @('PACKAGE-MANIFEST.json','config/defaults.json','README-本机原生包.txt','RifeLoveNative.exe')
$markerPath = Join-Path $root 'portable-mode.txt'
$markerHash = $null
if ($portable) {
    if (-not (Test-Path -LiteralPath $markerPath -PathType Leaf)) { throw 'Portable package has no permanent marker.' }
    Assert-NotReparsePoint $markerPath
    $markerBytes = [IO.File]::ReadAllBytes($markerPath)
    $expectedBytes = [Text.Encoding]::UTF8.GetBytes("RifeLovePortable 1`n")
    if ([Convert]::ToBase64String($markerBytes) -cne [Convert]::ToBase64String($expectedBytes)) {
        throw 'Portable marker must contain the exact UTF-8 marker and newline, without a BOM.'
    }
    $markerHash = (Get-FileHash -LiteralPath $markerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $requiredHashes += 'portable-mode.txt'
} elseif ((Test-Path -LiteralPath $markerPath) -or $sums.ContainsKey('portable-mode.txt')) {
    throw 'Normal package must not contain or checksum a portable marker.'
}
foreach ($relative in $requiredHashes) {
    if (-not $sums.ContainsKey($relative)) { throw "Required file not checksummed: $relative" }
    if ((Get-FileHash -LiteralPath (Join-Path $root $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sums[$relative]) {
        throw "Package checksum mismatch: $relative"
    }
}
$dataEntries = @(Get-ChildItem -LiteralPath (Join-Path $root 'data') -Force).Count
if ($RequireEmptyData -and $dataEntries -ne 0) { throw 'New acceptance package data directory is not empty.' }
$exeHash = (Get-FileHash -LiteralPath (Join-Path $root 'RifeLoveNative.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
if ($exeHash -cne $manifest.executableSha256) { throw 'Manifest EXE identity mismatch.' }
if ($ExpectedExecutable) {
    if ($exeHash -cne (Get-FileHash -LiteralPath $ExpectedExecutable -Algorithm SHA256).Hash.ToLowerInvariant()) {
        throw 'Packaged EXE differs from the final expected build.'
    }
}

function Get-ImmutableFiles([string]$Directory) {
    foreach ($entry in (Get-ChildItem -LiteralPath $Directory -Force)) {
        Assert-NotReparsePoint $entry.FullName
        if ($entry.PSIsContainer) {
            if ($entry.FullName -ine (Join-Path $root 'data')) { Get-ImmutableFiles $entry.FullName }
        } elseif ($entry.FullName -ine $sumPath) { $entry }
    }
}
if ($VerifyAllImmutableFiles) {
    $files = @(Get-ImmutableFiles $root)
    if ($files.Count -ne $sums.Count) { throw 'Immutable file inventory differs from checksum manifest.' }
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($root.Length).TrimStart('\','/').Replace('\','/')
        if (-not $sums.ContainsKey($relative) -or
            (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sums[$relative]) {
            throw "Immutable file not covered by its checksum: $relative"
        }
    }
}
[ordered]@{
    result = 'passed'
    package = $root
    expectedMode = $ExpectedMode
    effectiveDataDirectory = $effectiveData
    portableMarkerIncluded = $portable
    portableMarkerSha256 = $markerHash
    dataEntries = $dataEntries
    emptyDataRequired = [bool]$RequireEmptyData
    immutableFilesListed = $sums.Count
    allImmutableHashesVerified = [bool]$VerifyAllImmutableFiles
    executableSha256 = $exeHash
    expectedBuildMatched = [bool]$ExpectedExecutable
    runtimeDataPathVerified = $false
    launchedExe = $false
} | ConvertTo-Json -Depth 4
