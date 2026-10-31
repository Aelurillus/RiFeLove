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
if (-not (Test-Path -LiteralPath $outputRoot)) {
    New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
}
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

# Exercise full packaging's real preservation function after empty-directory
# initialization, without building/copying a runtime or launching a product.
$tokens = $null; $parseErrors = $null
$fullPackageAst = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $repositoryRoot 'packaging/windows/package-native-local.ps1'), [ref]$tokens, [ref]$parseErrors)
Assert-Equal 0 @($parseErrors).Count 'Full packaging syntax is valid'
foreach ($name in @('Assert-NoLinkTree','Get-PackageTreeItems','Resolve-ExistingFile','Copy-FileChecked',
    'Get-RunningNativePackageProcesses','Assert-PackageNotRunning','Copy-UserState')) {
    $definition = @($fullPackageAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $false))
    Assert-Equal 1 $definition.Count "Production preservation function found: $name"
    . ([scriptblock]::Create($definition[0].Extent.Text))
}
$oldPackage = Join-Path $testRoot 'old-package'
New-Item -ItemType Directory -Path (Join-Path $oldPackage 'data') -Force | Out-Null
$oldData = Join-Path $oldPackage 'data/preferences.json'
[IO.File]::WriteAllText($oldData,'{"packagingFixture":"preserve me"}',[Text.UTF8Encoding]::new($false))
$oldDataHash = (Get-FileHash -LiteralPath $oldData -Algorithm SHA256).Hash
Copy-UserState $oldPackage $normalPackage @() @{} ''
Assert-Equal $oldDataHash (Get-FileHash -LiteralPath (Join-Path $normalPackage 'data/preferences.json') -Algorithm SHA256).Hash 'Full refresh preserves old data after Normal initialization'
Assert-Equal $oldDataHash (Get-FileHash -LiteralPath $oldData -Algorithm SHA256).Hash 'Full refresh leaves original data intact'
Assert-Equal $sentinelHash (Get-FileHash -LiteralPath $dataSentinel -Algorithm SHA256).Hash 'Full refresh leaves unrelated target state intact'

# Serialize exactly the shared metadata used by both production packagers and
# run the independent read-only package verifier on both modes.
$verifier = Join-Path $repositoryRoot 'tools/validation/check_native_package_data_mode.ps1'
foreach ($fixture in @(
    @{ root = $normalPackage; mode = $normalData; expected = 'Normal' },
    @{ root = $portablePackage; mode = $portableData; expected = 'Portable' }
)) {
    $root = $fixture.root
    New-Item -ItemType Directory -Path (Join-Path $root 'config') | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'RifeLoveNative.exe'),'NOT AN EXECUTABLE: packaging contract fixture',[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $root 'README-本机原生包.txt'),'Packaging contract fixture; do not launch.',[Text.UTF8Encoding]::new($false))
    $exeHash = (Get-FileHash -LiteralPath (Join-Path $root 'RifeLoveNative.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = @{ executableSha256 = $exeHash }
    $defaults = @{ product = 'packaging-contract-fixture' }
    Set-NativePackageDataModeMetadata -Metadata $manifest -DataMode $fixture.mode -Manifest
    Set-NativePackageDataModeMetadata -Metadata $defaults -DataMode $fixture.mode
    if ($fixture.expected -ceq 'Normal') {
        $manifest.packagedDataState = 'preserved-existing-local-state; data excluded from immutable checksums'
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'PACKAGE-MANIFEST.json') -Encoding utf8NoBOM
    $defaults | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'config/defaults.json') -Encoding utf8NoBOM
    $lines = @('RifeLove native local bundle SHA-256')
    foreach ($file in @(Get-ChildItem -LiteralPath $root -File -Recurse | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($root.Length).TrimStart('\','/').Replace('\','/')
        if ($relative -match '^data/') { continue }
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $lines += "$hash  $relative"
    }
    [IO.File]::WriteAllLines((Join-Path $root 'SHA256SUMS.txt'),[string[]]$lines,[Text.UTF8Encoding]::new($false))
    $result = & $verifier -PackageDirectory $root -ExpectedMode $fixture.expected -VerifyAllImmutableFiles -RequireEmptyData:($fixture.expected -ceq 'Portable') | ConvertFrom-Json
    Assert-Equal 'passed' $result.result "Independent verifier accepts serialized $($fixture.expected) metadata"
    Assert-Equal $false $result.launchedExe 'Contract validation never launches the fixture EXE'
}
Assert-Equal $false (Test-Path -LiteralPath (Join-Path $normalPackage 'portable-mode.txt')) 'Normal metadata serialization never enables portable mode'
Assert-Equal $markerHash (Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash.ToLowerInvariant() 'Shared metadata serialization never changes the portable marker'

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
    fullRefreshPreservedExistingData = $true
    normalAndPortableVerifierPassed = $true
    launchedExe = $false
    cleanupPerformed = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'test-result.json') -Encoding utf8NoBOM
Write-Output "Portable package helper checks passed. Fixtures retained at $testRoot"
