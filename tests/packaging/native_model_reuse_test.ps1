[CmdletBinding()]
param([string]$OutputDirectory = '', [string]$SourcePackageDirectory = '')

# Load the production function definitions without executing a package build.
# These tiny fake graphs test packaging contracts only, never model quality.
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outputRoot = Join-Path $repositoryRoot 'out'
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $outputRoot ('native-model-reuse-test-' + [guid]::NewGuid().ToString('N'))
}
$testRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $testRoot.StartsWith($outputRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    (Test-Path -LiteralPath $testRoot)) { throw 'Fixtures require a new directory under repository out.' }
if (-not (Test-Path -LiteralPath $outputRoot)) {
    New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
}
if ((Get-Item -LiteralPath $outputRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw 'Test output root must not be a link.'
}
New-Item -ItemType Directory -Path $testRoot | Out-Null
$checks = 0

function Assert-Equal($Expected, $Actual, [string]$Message) {
    if ($Expected -cne $Actual) { throw "${Message}: expected '$Expected', found '$Actual'" }
    $script:checks++
}
function Assert-Rejected([scriptblock]$Action, [string]$Pattern, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $_.Exception.Message -match $Pattern }
    Assert-Equal $true $rejected $Message
}
function Write-Fixture([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}
function Hash-File([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

foreach ($selection in @(
    @{ path = 'packaging/windows/package-native-reuse-local.ps1'; names = @(
        'Get-SafeTreeFiles','Read-ValidatedModelPack','New-HardLinkChecked','Get-NativeModelRole',
        'Get-NativeOptionalRestorationPacks','Set-NativeRestorationDefaults','Update-NativeOptionalRestorationManifest') },
    @{ path = 'packaging/windows/package-native-local.ps1'; names = @(
        'Assert-NoLinkTree','Resolve-ExistingFile','Read-Json','Get-DescriptorHash','Validate-ModelPack') }
)) {
    $tokens = $null; $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $repositoryRoot $selection.path), [ref]$tokens, [ref]$errors)
    Assert-Equal 0 @($errors).Count "PowerShell syntax: $($selection.path)"
    foreach ($name in $selection.names) {
        $definition = @($ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $false))
        Assert-Equal 1 $definition.Count "Production function found: $name"
        . ([scriptblock]::Create($definition[0].Extent.Text))
    }
}

function New-ModelFixture([string]$Id, [string]$Parent) {
    $directory = Join-Path $Parent $Id
    $licenseName = if ($Id -ceq 'stdf-r3-y7-sdr8-local-eval') { 'LICENSE-LOCAL-EVALUATION.txt' } else { 'LICENSE.txt' }
    foreach ($name in @('model.onnx',$licenseName,'source.json','validation.json')) {
        Write-Fixture (Join-Path $directory $name) "PACKAGING TEST FIXTURE ONLY: $Id $name"
    }
    $descriptor = [ordered]@{
        schemaVersion = 1; id = $Id; family = 'video-restoration'; version = 'fixture-v1'; modelFile = 'model.onnx'
        sha256 = Hash-File (Join-Path $directory 'model.onnx')
        license = @{ id = 'test-fixture-only'; file = $licenseName; sha256 = Hash-File (Join-Path $directory $licenseName) }
        provenance = @{ manifestFile = 'source.json'; manifestSha256 = Hash-File (Join-Path $directory 'source.json') }
        validation = @{ reportFile = 'validation.json'; reportSha256 = Hash-File (Join-Path $directory 'validation.json'); validatedBackends = @('fixture-not-inference') }
    }
    Write-Fixture (Join-Path $directory 'model.json') ($descriptor | ConvertTo-Json -Depth 8)
    return $directory
}

$temporalId = 'fastdvdnet-clipped-five-frame-sdr8-eval'
$compressionId = 'stdf-r3-y7-sdr8-local-eval'
$localTemporal = New-ModelFixture $temporalId (Join-Path $testRoot 'cache')
$localCompression = New-ModelFixture $compressionId (Join-Path $testRoot 'cache')
$source = Join-Path $testRoot 'source-four-model'
New-Item -ItemType Directory -Path $source | Out-Null
$manifest = @{ models = @(
    @{ directory = 'models/rife-4.25-fp32-sdr8-v3'; role = 'default-interpolation' },
    @{ directory = 'models/realesrgan-x2plus-sdr8-v2'; role = 'optional-super-resolution' }
) }
$optional = @(Get-NativeOptionalRestorationPacks $manifest $source $localTemporal $localCompression)
Assert-Equal 2 $optional.Count 'Both optional packs are available for an old two-model package'
Assert-Equal 2 @($optional | Where-Object { -not $_.inSource }).Count 'Missing optional models use validated local packs'

foreach ($item in $optional) {
    $destination = Join-Path $source $item.relative
    Copy-Item -LiteralPath $item.pack.source -Destination $destination -Recurse -Force
    $licenseDestination = Join-Path $source $item.licenseRelative
    New-Item -ItemType Directory -Path (Split-Path -Parent $licenseDestination) -Force | Out-Null
    Copy-Item -LiteralPath $item.pack.licensePath -Destination $licenseDestination
}
Update-NativeOptionalRestorationManifest $manifest $optional
Assert-Equal 4 @($manifest.models).Count 'Manifest adds exactly two optional models'
Update-NativeOptionalRestorationManifest $manifest $optional
Assert-Equal 4 @($manifest.models).Count 'Metadata refresh does not duplicate optional models'
Assert-Equal $false $manifest.compressionRestorationDefaultEnabled 'Manifest leaves compression repair disabled'
$stdfEntry = @($manifest.models | Where-Object { $_.id -ceq $compressionId })[0]
Assert-Equal 'experimental-optional-compression-repair' $stdfEntry.role 'STDF is never assigned the interpolation role'
Assert-Equal 'local-only-until-license-review' $stdfEntry.redistribution 'STDF remains local evaluation only'
Assert-Equal $compressionId $stdfEntry.canonicalBasename 'Canonical STDF basename survives reuse'
Assert-Equal 'experimental-optional-compression-repair' (Get-NativeModelRole 'models\stdf-r3-y7-sdr8-local-eval') 'Backslash paths retain the STDF role'

$fromSource = @(Get-NativeOptionalRestorationPacks $manifest $source 'missing-temporal-cache' 'missing-stdf-cache')
Assert-Equal 2 @($fromSource | Where-Object { $_.inSource }).Count 'Four-model source needs no duplicate local caches'
$defaults = @{ compressionRestorationDefaultEnabled = $true; temporalRestorationDefaultEnabled = $true; targetRate = '144/1' }
Set-NativeRestorationDefaults $defaults
Assert-Equal $false $defaults.compressionRestorationDefaultEnabled 'Existing enabled STDF setting is reset to safe default'
Assert-Equal $false $defaults.compressionRestorationTiled 'Packaging cannot silently opt a user into tiled restoration'
Assert-Equal $false $defaults.temporalRestorationDefaultEnabled 'Existing enabled FastDVDnet setting is reset to safe default'
Assert-Equal "models/$compressionId" $defaults.compressionRestorationModelDirectory 'STDF defaults reference the packaged model'
Assert-Equal 0.75 $defaults.compressionRestorationStrength 'STDF selectable strength is retained'
Assert-Equal 'experimental-local-evaluation' $defaults.compressionRestorationMode 'STDF evaluation scope is explicit'
Assert-Equal '144/1' $defaults.targetRate 'Unrelated source defaults are preserved'

$threeSource = Join-Path $testRoot 'source-three-model'
New-Item -ItemType Directory -Path (Join-Path $threeSource 'models') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $source "models/$temporalId") -Destination (Join-Path $threeSource 'models') -Recurse
New-Item -ItemType Directory -Path (Join-Path $threeSource 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $source "licenses/Model-$temporalId-LICENSE.txt") -Destination (Join-Path $threeSource 'licenses')
$threeManifest = @{ models = @($manifest.models | Where-Object { $_.id -cne $compressionId }) }
$compatible = @(Get-NativeOptionalRestorationPacks $threeManifest $threeSource 'missing-temporal-cache' '')
Assert-Equal 1 $compatible.Count 'Old three-model package does not invent an undeclared STDF model'
$compatibleDefaults = @{}
Set-NativeRestorationDefaults $compatibleDefaults $false
Assert-Equal $null $compatibleDefaults.compressionRestorationModelDirectory 'Old three-model package has no nonexistent compression model path'
Assert-Equal $false $compatibleDefaults.compressionRestorationDefaultEnabled 'Old three-model package leaves compression disabled'
Update-NativeOptionalRestorationManifest $threeManifest $compatible
Assert-Equal 3 @($threeManifest.models).Count 'Compatible old package remains a three-model package'
$upgrade = @(Get-NativeOptionalRestorationPacks $threeManifest $threeSource 'missing-temporal-cache' $localCompression)
Assert-Equal 1 @($upgrade | Where-Object { $_.inSource }).Count 'Three-model source reuses its temporal model'
Assert-Equal "models/$compressionId" @($upgrade | Where-Object { -not $_.inSource })[0].relative 'Three-model upgrade adds only STDF'
Update-NativeOptionalRestorationManifest $threeManifest $upgrade
Assert-Equal 4 @($threeManifest.models).Count 'Three-model upgrade produces a four-model manifest'

$duplicate = @{ models = @($manifest.models) + $stdfEntry }
Assert-Rejected { Get-NativeOptionalRestorationPacks $duplicate $source $localTemporal $localCompression } 'Duplicate source model' 'Duplicate STDF manifest entries are rejected'
foreach ($field in @('sha256','version','id','family')) {
    $badManifest = $manifest | ConvertTo-Json -Depth 12 | ConvertFrom-Json -AsHashtable
    $badEntry = @($badManifest.models | Where-Object { $_.id -ceq $compressionId })[0]
    $badEntry[$field] = 'wrong-identity'
    Assert-Rejected { Get-NativeOptionalRestorationPacks $badManifest $source $localTemporal $localCompression } 'manifest identity mismatch' "Mismatched STDF $field is rejected"
}
$undeclared = @{ models = @($manifest.models | Where-Object { $_.id -cne $compressionId }) }
Assert-Rejected { Get-NativeOptionalRestorationPacks $undeclared $source $localTemporal $localCompression } 'not declared in its manifest' 'Undeclared source model directories cannot be silently replaced'

foreach ($name in @('model.onnx','LICENSE-LOCAL-EVALUATION.txt','source.json','validation.json')) {
    $badRoot = New-ModelFixture $compressionId (Join-Path $testRoot ('tampered-' + $name.Replace('.','-')))
    Write-Fixture (Join-Path $badRoot $name) 'TAMPERED PACKAGING FIXTURE'
    Assert-Rejected { Read-ValidatedModelPack $badRoot 'STDF fixture' $compressionId 'video-restoration' } 'SHA-256 mismatch' "Changed $name is rejected during reuse"
    Assert-Rejected { Validate-ModelPack $badRoot $compressionId 'video-restoration' $false } 'SHA-256' "Changed $name is rejected during full packaging"
}

$link = Join-Path $testRoot 'hardlink-preview/model.onnx'
$graph = Join-Path $localCompression 'model.onnx'
$beforeHash = Hash-File $graph
New-HardLinkChecked $graph $link
Assert-Equal $beforeHash (Hash-File $link) 'Hard-link staging preserves model bytes'
$sourceId = (& fsutil.exe file queryfileid $graph) -join ' '
$targetId = (& fsutil.exe file queryfileid $link) -join ' '
Assert-Equal $true ($sourceId -match '0x[0-9a-fA-F]+') 'NTFS reports an original file identity'
$sourceFileId = $Matches[0]
Assert-Equal $true ($targetId -match '0x[0-9a-fA-F]+') 'NTFS reports the preview file identity'
Assert-Equal $sourceFileId $Matches[0] 'Preview uses an NTFS hard link rather than another copy'
Assert-Equal $beforeHash (Hash-File $graph) 'Hard-link staging leaves its source unchanged'
Assert-Rejected { New-HardLinkChecked $graph $link } 'already exists' 'Existing linked files are never overwritten'

if ($SourcePackageDirectory) {
    $realSource = [IO.Path]::GetFullPath($SourcePackageDirectory)
    $realManifestPath = Join-Path $realSource 'PACKAGE-MANIFEST.json'
    $originalManifestHash = Hash-File $realManifestPath
    $realManifest = Get-Content -Raw -LiteralPath $realManifestPath | ConvertFrom-Json -AsHashtable
    $realPacks = @(Get-NativeOptionalRestorationPacks $realManifest $realSource '' '')
    Assert-Equal 2 @($realPacks | Where-Object { $_.inSource }).Count 'Real four-model source is reusable without caches'
    Update-NativeOptionalRestorationManifest $realManifest $realPacks
    Assert-Equal 4 @($realManifest.models | Where-Object { -not $_.legacy }).Count 'Real source retains its four nonlegacy models'
    Assert-Equal $originalManifestHash (Hash-File $realManifestPath) 'Real source manifest stays unchanged'
}

[ordered]@{
    result = 'passed'; checks = $checks; fixtures = $testRoot
    fourModelSourceReused = $true; threeModelSourceUpgraded = $true
    stdfRoleAndDefaultsPreserved = $true; tamperedModelsRejected = $true
    ntfsHardLinkIdentityVerified = $true; launchedExe = $false; cleanupPerformed = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'test-result.json') -Encoding utf8NoBOM
Write-Output "Native model reuse checks passed ($checks). Tiny fixtures retained at $testRoot"
