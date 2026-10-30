[CmdletBinding()]
param([string]$OutputDirectory = '')

# These are permanent, tiny test fixtures. In particular the portable marker is
# never created temporarily around an EXE launch and is never removed by tests.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../packaging/windows/native-package-options.ps1')
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'out'))
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $outputRoot ('native-portable-helper-test-' + [guid]::NewGuid().ToString('N'))
}
$testRoot = [IO.Path]::GetFullPath($OutputDirectory)
$prefix = $outputRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if (-not $testRoot.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test fixtures must be in a new directory under repository out.'
}
if (Test-Path -LiteralPath $testRoot) { throw "Refusing to overwrite test fixtures: $testRoot" }
if ((Get-Item -LiteralPath $outputRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Test output root must not be a link.'
}

function Assert-Equal($Expected, $Actual, [string]$Message) {
    if ($Expected -cne $Actual) { throw "${Message}: expected '$Expected', found '$Actual'" }
}

$normalPackage = Join-Path $testRoot 'normal-package'
$portablePackage = Join-Path $testRoot 'portable-package'
foreach ($root in @($normalPackage,$portablePackage)) {
    New-Item -ItemType Directory -Path (Join-Path $root 'data') -Force | Out-Null
}
Assert-NativePackageDirectoriesDisjoint $normalPackage $portablePackage
Assert-NativePackageDirectoriesDisjoint $normalPackage ($normalPackage + '-sibling')
foreach ($pair in @(
    @($normalPackage,$normalPackage),
    @($normalPackage,(Join-Path $normalPackage 'nested-target')),
    @((Join-Path $normalPackage 'nested-source'),$normalPackage)
)) {
    $rejected=$false
    try { Assert-NativePackageDirectoriesDisjoint $pair[0] $pair[1] }
    catch { $rejected=$_.Exception.Message.Contains('must be disjoint') }
    Assert-Equal $true $rejected 'Equal or ancestor package paths are rejected before staging'
}
$normalData = Initialize-NativePackageDataMode -PackageRoot $normalPackage
Assert-Equal $false $normalData.portableModeEnabled 'Normal mode is the default'
Assert-Equal '%LOCALAPPDATA%\RifeLove' $normalData.dataDirectory 'Normal effective data path'
Assert-Equal $false (Test-Path -LiteralPath (Join-Path $normalPackage 'portable-mode.txt')) 'Normal mode creates no marker'
Assert-Equal 0 @(Get-ChildItem -LiteralPath (Join-Path $normalPackage 'data') -Force).Count 'Normal package data starts empty'

$portableData = Initialize-NativePackageDataMode -PackageRoot $portablePackage -PortableMode
$marker = Join-Path $portablePackage 'portable-mode.txt'
Assert-Equal $true $portableData.portableModeEnabled 'Portable mode is explicit'
Assert-Equal 'data' $portableData.dataDirectory 'Portable effective data path'
Assert-Equal "RifeLovePortable 1`n" ([IO.File]::ReadAllText($marker)) 'Permanent marker exact contents'
Assert-Equal 19 ([IO.File]::ReadAllBytes($marker)).Length 'Permanent marker has no BOM or extra bytes'
Assert-Equal 0 @(Get-ChildItem -LiteralPath (Join-Path $portablePackage 'data') -Force).Count 'Portable package data starts empty'
$markerHash = (Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-Equal $markerHash $portableData.markerSha256 'Marker hash matches its exact bytes'
foreach ($requestedMode in @($false,$true)) {
    $rejected = $false
    try { Initialize-NativePackageDataMode -PackageRoot $portablePackage -PortableMode:$requestedMode | Out-Null }
    catch { $rejected = $_.Exception.Message.Contains('existing portable marker') }
    Assert-Equal $true $rejected 'A package mode is never silently overwritten'
}
Assert-Equal $markerHash (Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash.ToLowerInvariant() 'Rejected mode changes preserve marker'

$dataSentinel = Join-Path $normalPackage 'data/queue.sqlite'
[IO.File]::WriteAllText($dataSentinel,'test fixture: preserve this existing task data',[Text.UTF8Encoding]::new($false))
$sentinelHash = (Get-FileHash -LiteralPath $dataSentinel -Algorithm SHA256).Hash
foreach ($requestedMode in @($false,$true)) {
    $rejected = $false
    try { Initialize-NativePackageDataMode -PackageRoot $normalPackage -PortableMode:$requestedMode | Out-Null }
    catch { $rejected = $_.Exception.Message.Contains('new empty package data directory') }
    Assert-Equal $true $rejected 'Existing data blocks package initialization in both modes'
}
Assert-Equal $sentinelHash (Get-FileHash -LiteralPath $dataSentinel -Algorithm SHA256).Hash 'Rejected initialization preserves existing data'
Assert-Equal $false (Test-Path -LiteralPath (Join-Path $normalPackage 'portable-mode.txt')) 'Rejected initialization never creates a marker'

[ordered]@{
    result = 'passed'
    fixtures = $testRoot
    normalUsesUserData = $true
    portableMarkerExactAndPermanent = $true
    existingMarkerRejectedBothModes = $true
    populatedDataRejectedBothModes = $true
    existingContentsUnchanged = $true
    ancestorOverlapRejectedBothDirections = $true
    prefixSimilarSiblingAllowed = $true
    launchedExe = $false
    cleanupPerformed = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'test-result.json') -Encoding utf8NoBOM
Write-Output "Portable package helper checks passed. Fixtures retained at $testRoot"
