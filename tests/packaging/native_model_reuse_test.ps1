[CmdletBinding()]
param([string]$OutputDirectory = '', [string]$SourcePackageDirectory = '')

# Load the production function definitions without executing a package build.
# These tiny fake graphs test packaging contracts only, never model quality.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../packaging/windows/native-package-options.ps1')
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
        'Get-SafeFiles','Get-SafeTreeFiles','Read-ValidatedModelPack','New-HardLinkChecked','Get-NativeModelRole',
        'Resolve-ManifestPath','Read-NativeSourceChecksums',
        'Get-NativeOptionalRestorationPacks','Set-NativeRestorationDefaults','Update-NativeOptionalRestorationManifest') },
    @{ path = 'packaging/windows/package-native-local.ps1'; names = @(
        'Assert-NoLinkTree','Resolve-ExistingDirectory','Resolve-ExistingFile','Read-Json','Get-DescriptorHash','Validate-ModelPack',
        'Get-NativeFullModelPacks','Get-PackageTreeItems','Find-LegacyModelReference','Resolve-NativeLegacyModelMigration') }
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

function New-ModelFixture([string]$Id, [string]$Parent, [string]$Family = 'video-restoration') {
    $directory = Join-Path $Parent $Id
    $licenseName = if ($Id -ceq 'stdf-r3-y7-sdr8-local-eval') { 'LICENSE-LOCAL-EVALUATION.txt' } else { 'LICENSE.txt' }
    foreach ($name in @('model.onnx',$licenseName,'source.json','validation.json')) {
        Write-Fixture (Join-Path $directory $name) "PACKAGING TEST FIXTURE ONLY: $Id $name"
    }
    $descriptor = [ordered]@{
        schemaVersion = 1; id = $Id; family = $Family; version = 'fixture-v1'; modelFile = 'model.onnx'
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

# Explicit base mode must resolve its model set before any optional graph,
# descriptor, license, or checksum target is opened. These are fake graphs;
# they prove bundle/data contracts only, never inference quality.
$baseSelection = Resolve-NativeModelBundleSelection -BaseModelsOnly
$rifeOnlySelection = Resolve-NativeModelBundleSelection -RifeOnly
Assert-Equal 'rife-only' $rifeOnlySelection.mode 'RIFE-only bundle has an explicit mode'
Assert-Equal 1 @($rifeOnlySelection.modelIds).Count 'RIFE-only selects just the interpolation model'
Assert-Equal $false $rifeOnlySelection.includeSuperResolution 'RIFE-only excludes super resolution'
Assert-Rejected { Resolve-NativeModelBundleSelection -RifeOnly -IncludeTemporalRestoration } 'cannot be combined' 'RIFE-only rejects ambiguous additional models'
Assert-Rejected { Resolve-NativeModelBundleSelection -RifeOnly -BaseModelsOnly } 'cannot be combined' 'RIFE-only and base modes are mutually exclusive'
$fourSelection = Resolve-NativeModelBundleSelection
$temporalSelection = Resolve-NativeModelBundleSelection -BaseModelsOnly -IncludeTemporalRestoration
$compressionSelection = Resolve-NativeModelBundleSelection -BaseModelsOnly -IncludeCompressionRestoration
$deblurSelection = Resolve-NativeModelBundleSelection -BaseModelsOnly -IncludeDeblurRestoration
$fiveSelection = Resolve-NativeModelBundleSelection -IncludeDeblurRestoration
$allSelection = Resolve-NativeModelBundleSelection -BaseModelsOnly -IncludeTemporalRestoration -IncludeCompressionRestoration
Assert-Equal 2 @($baseSelection.modelIds).Count 'Base explicitly selects one RIFE and one SR'
Assert-Equal 4 @($fourSelection.modelIds).Count 'Default full packaging preserves four-model selection'
Assert-Equal $false $fourSelection.includeDeblurRestoration 'Default full packaging never opts into NAFNet'
Assert-Equal 3 @($temporalSelection.modelIds).Count 'FastDVDnet can be added independently'
Assert-Equal 3 @($compressionSelection.modelIds).Count 'STDF can be added independently'
Assert-Equal 3 @($deblurSelection.modelIds).Count 'NAFNet can be added independently of other experiments'
Assert-Equal 5 @($fiveSelection.modelIds).Count 'Full packaging includes NAFNet only when explicitly selected'
Assert-Equal 4 @($allSelection.modelIds).Count 'Both experiments can be added to a fresh base package'
$rifeFixture = New-ModelFixture 'rife-4.25-fp32-sdr8-v3' (Join-Path $testRoot 'base-cache') 'rife'
$srFixture = New-ModelFixture 'realesrgan-x2plus-sdr8-v2' (Join-Path $testRoot 'base-cache')
$missing = Join-Path $testRoot 'excluded-cache-does-not-exist'
$basePacks = @(Get-NativeFullModelPacks $baseSelection $rifeFixture $srFixture $missing $missing)
Assert-Equal 2 $basePacks.Count 'Full packager base mode does not require excluded caches'
$rifeOnlyPacks = @(Get-NativeFullModelPacks $rifeOnlySelection $rifeFixture $missing $missing $missing)
Assert-Equal 1 $rifeOnlyPacks.Count 'RIFE-only full packaging does not open missing restoration caches'
Assert-Equal 4 @(Get-NativeFullModelPacks $fourSelection $rifeFixture $srFixture $localTemporal $localCompression).Count 'Full packager default still validates all four packs'
Assert-Equal 3 @(Get-NativeFullModelPacks $temporalSelection $rifeFixture $srFixture $localTemporal $missing).Count 'Full packager can include temporal only'
Assert-Equal 3 @(Get-NativeFullModelPacks $compressionSelection $rifeFixture $srFixture $missing $localCompression).Count 'Full packager can include compression only'
Assert-Rejected { Get-NativeFullModelPacks $deblurSelection $rifeFixture $srFixture $missing $missing $missing } '不存在' 'Selected missing NAFNet is rejected'
Assert-Rejected { Get-NativeFullModelPacks $temporalSelection $rifeFixture $srFixture $missing $localCompression } '不存在' 'Selected missing FastDVDnet is rejected'
Assert-Rejected { Get-NativeFullModelPacks $compressionSelection $rifeFixture $srFixture $localTemporal $missing } '不存在' 'Selected missing STDF is rejected'

# Keep the source's declared experiments deliberately invalid, then prove a
# base copy never parses them. Missing excluded files remain in SHA256SUMS.
$baseSource = Join-Path $testRoot 'source-base-selection'
foreach ($pack in $basePacks) {
    $destination = Join-Path $baseSource ('models/' + $pack.destination)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $pack.source -Destination $destination -Recurse
    $licenseDestination = Join-Path $baseSource ('licenses/Model-' + $pack.destination + '-LICENSE.txt')
    Write-Fixture $licenseDestination ([IO.File]::ReadAllText($pack.licensePath))
}
Write-Fixture (Join-Path $baseSource "models/$temporalId/model.json") 'THIS IS INTENTIONALLY INVALID JSON'
Write-Fixture (Join-Path $baseSource "models/$compressionId/model.json") 'THIS IS INTENTIONALLY INVALID JSON'
Write-Fixture (Join-Path $baseSource 'models/nafnet-gopro-deblur-sdr8-fp32-eval/model.json') 'THIS IS INTENTIONALLY INVALID JSON'
Write-Fixture (Join-Path $baseSource "licenses/Model-$temporalId-LICENSE.txt") 'EXCLUDED LICENSE FIXTURE'
Write-Fixture (Join-Path $baseSource "licenses/Model-$compressionId-LICENSE-LOCAL-EVALUATION.txt") 'EXCLUDED LICENSE FIXTURE'
Write-Fixture (Join-Path $baseSource 'licenses/Model-nafnet-gopro-deblur-sdr8-fp32-eval-LICENSE') 'EXCLUDED LICENSE FIXTURE'
Write-Fixture (Join-Path $baseSource 'models/rife-4.25-fp32-sdr8-v2/model.json') 'EXCLUDED LEGACY FIXTURE'
Write-Fixture (Join-Path $baseSource 'models/user-custom-model/model.json') 'USER MODEL MUST NOT BE COPIED'
Write-Fixture (Join-Path $baseSource 'models/legacy-models.json') '{"recoveryFixture":"must remain in source"}'
Write-Fixture (Join-Path $baseSource 'data/queue.sqlite') 'USER QUEUE FIXTURE - DO NOT COPY'
Write-Fixture (Join-Path $baseSource 'data/checkpoints/recovery.json') '{"recoveryFixture":"must remain in source"}'
$queueHash = Hash-File (Join-Path $baseSource 'data/queue.sqlite')
$recoveryHash = Hash-File (Join-Path $baseSource 'data/checkpoints/recovery.json')
$sourceBefore = @(Get-ChildItem -LiteralPath $baseSource -File -Recurse | ForEach-Object { $_.FullName + ':' + (Hash-File $_.FullName) }) -join "`n"
$baseExcluded = @(Get-NativeExcludedModelRoots $baseSelection)
Assert-Equal 0 @(Get-NativeOptionalRestorationPacks $manifest $baseSource $missing $missing $baseSelection).Count 'Reuse base skips declared but invalid excluded optional descriptors'
Assert-Equal 2 @(Get-NativeOptionalRestorationPacks $manifest $source $missing $missing $fourSelection).Count 'Default full reuse keeps existing experiments without adding NAFNet'
Assert-Rejected { Get-NativeOptionalRestorationPacks @{ models = @() } $testRoot $missing $missing $deblurSelection $missing } 'missing' 'Selected missing NAFNet is never silently omitted'
Assert-Rejected { Get-NativeOptionalRestorationPacks @{ models = @() } $baseSource $missing $missing $compressionSelection } 'not declared' 'A selected undeclared STDF source is rejected'
Assert-Rejected { Get-NativeOptionalRestorationPacks @{ models = @() } $testRoot $missing '' $compressionSelection } 'missing' 'Explicitly selected missing STDF is never silently omitted'
$baseFiles = @(Get-SafeFiles $baseSource $baseExcluded $baseSource $baseSelection)
Assert-Equal 12 $baseFiles.Count 'Base copy plan contains only two six-file model/license packs'
$rifeOnlyExcluded = @(Get-NativeExcludedModelRoots $rifeOnlySelection)
$rifeOnlyFiles = @(Get-SafeFiles $baseSource $rifeOnlyExcluded $baseSource $rifeOnlySelection)
Assert-Equal 6 $rifeOnlyFiles.Count 'RIFE-only reuse plan contains only the RIFE model and license'
Assert-Equal 0 @($rifeOnlyFiles | Where-Object { $_.FullName -match 'realesrgan|fastdvdnet|stdf-r3|nafnet|[\\/]data[\\/]' }).Count 'RIFE-only excludes all restoration models and user data'
Assert-Equal 0 @($baseFiles | Where-Object { $_.FullName -match 'fastdvdnet|stdf-r3|rife-4.25-fp32-sdr8-v2[\\/]|user-custom|legacy-models|[\\/]data[\\/]' }).Count 'Excluded models, metadata and user data stay outside base copy plan'
$baseOutput = Join-Path $testRoot 'staged-base-bundle'
foreach ($file in $baseFiles) {
    $relative = [IO.Path]::GetRelativePath($baseSource, $file.FullName)
    New-HardLinkChecked $file.FullName (Join-Path $baseOutput $relative)
}
New-Item -ItemType Directory -Path (Join-Path $baseOutput 'data') | Out-Null
Initialize-NativePackageDataMode $baseOutput | Out-Null
Assert-Equal 2 @(Get-ChildItem -LiteralPath (Join-Path $baseOutput 'models') -Directory).Count 'Staged base contains exactly RIFE plus SR'
Assert-Equal 0 @(Get-ChildItem -LiteralPath (Join-Path $baseOutput 'data') -Force).Count 'Staged base creates empty independent data'
Assert-Equal $queueHash (Hash-File (Join-Path $baseSource 'data/queue.sqlite')) 'Source queue remains byte-identical'
Assert-Equal $recoveryHash (Hash-File (Join-Path $baseSource 'data/checkpoints/recovery.json')) 'Source recovery remains byte-identical'
$sourceAfter = @(Get-ChildItem -LiteralPath $baseSource -File -Recurse | ForEach-Object { $_.FullName + ':' + (Hash-File $_.FullName) }) -join "`n"
Assert-Equal $sourceBefore $sourceAfter 'Staging never modifies any source file'

$source = $baseSource # Resolve-ManifestPath uses the packager's explicit source root.
$checksumFixture = Join-Path $testRoot 'base-selection-SHA256SUMS.txt'
$checksumLines = @('RifeLove native local bundle SHA-256')
foreach ($file in $baseFiles) {
    $checksumLines += (Hash-File $file.FullName) + '  ' + [IO.Path]::GetRelativePath($baseSource, $file.FullName).Replace('\','/')
}
$fakeHash = '0' * 64
foreach ($relative in @("models/$temporalId/model.onnx", "models/$compressionId/model.onnx", "licenses/Model-$compressionId-MISSING.txt", 'models/rife-4.25-fp32-sdr8-v2/model.onnx', 'models/user-custom-model/model.onnx', 'models/legacy-models.json', 'data/queue.sqlite')) {
    $checksumLines += "$fakeHash  $relative"
}
Write-Fixture $checksumFixture ($checksumLines -join "`n")
$selectedChecksums = Read-NativeSourceChecksums $checksumFixture $baseExcluded $baseSelection
Assert-Equal 12 $selectedChecksums.Count 'Excluded missing checksum targets cannot block base bundle'
$rifeOnlyChecksums = Read-NativeSourceChecksums $checksumFixture $rifeOnlyExcluded $rifeOnlySelection
Assert-Equal 6 $rifeOnlyChecksums.Count 'RIFE-only checksum plan excludes all restoration files and licenses'
foreach ($relative in $selectedChecksums.Keys) {
    Assert-Equal $true (Test-Path -LiteralPath (Resolve-ManifestPath $relative) -PathType Leaf) 'Every selected checksum target exists'
}
$baseDefaults = @{ targetRate = '144/1'; temporalRestorationDefaultEnabled = $true; compressionRestorationDefaultEnabled = $true }
Set-NativeRestorationDefaults $baseDefaults $false $false
Assert-Equal $null $baseDefaults.temporalRestorationModelDirectory 'Base defaults never reference excluded FastDVDnet'
Assert-Equal $null $baseDefaults.compressionRestorationModelDirectory 'Base defaults never reference excluded STDF'
Assert-Equal 'not-bundled' $baseDefaults.temporalRestorationMode 'Temporal exclusion is explicit in defaults'
Assert-Equal 'not-bundled' $baseDefaults.compressionRestorationMode 'Compression exclusion is explicit in defaults'
Assert-Equal $false $baseDefaults.temporalRestorationDefaultEnabled 'Base temporal mode remains disabled'
Assert-Equal $false $baseDefaults.compressionRestorationDefaultEnabled 'Base compression mode remains disabled'
Assert-Equal $null $baseDefaults.deblurRestorationModelDirectory 'Default bundle has no NAFNet model path'
Assert-Equal $false $baseDefaults.deblurRestorationDefaultEnabled 'NAFNet is never enabled by packaging'
Assert-Equal 'not-bundled' $baseDefaults.deblurRestorationMode 'Default bundle records NAFNet as absent'
Assert-Equal '144/1' $baseDefaults.targetRate 'Base selection preserves unrelated defaults'

# The real local NAFNet cache is optional in CI. When available, validate its
# fixed graph identity without duplicating 274 MB: test package trees hard-link
# the graph and copy only the small descriptor, license and sidecar files.
$deblurCache = Join-Path $repositoryRoot 'models/cache/nafnet-gopro-deblur-sdr8-fp32-eval'
$deblurVerified = $false
if (Test-Path -LiteralPath (Join-Path $deblurCache 'model.onnx') -PathType Leaf) {
    $deblurDirectory = 'nafnet-gopro-deblur-sdr8-fp32-eval'
    $deblurId = 'nafnet-zoo-gopro-sdr8-local'
    $deblurHash = 'b99b39f0bdf29f15205aa50dd23b7a14b571b0c7ae9576ba2f9417fccad4f056'
    $deblurFullPack = Validate-ModelPack $deblurCache $deblurDirectory 'video-restoration' $false
    Assert-Equal $deblurHash $deblurFullPack.graphHash 'Full packaging pins the verified NAFNet graph'
    Assert-Equal $deblurId $deblurFullPack.id 'NAFNet descriptor ID differs safely from its package directory'
    Assert-Equal 3 @(Get-NativeFullModelPacks $deblurSelection $rifeFixture $srFixture $missing $missing $deblurCache).Count 'Full packager accepts independently selected NAFNet'
    Assert-Equal 'experimental-optional-deblur' (Get-NativeModelRole "models\$deblurDirectory") 'NAFNet has a distinct deblur role'
    $deblurSource = Join-Path $testRoot 'source-with-deblur'
    New-Item -ItemType Directory -Path $deblurSource | Out-Null
    $deblurManifest = @{ models = @() }
    $deblurLocal = @(Get-NativeOptionalRestorationPacks $deblurManifest $deblurSource $missing $missing $deblurSelection $deblurCache)
    Assert-Equal 1 $deblurLocal.Count 'Explicit NAFNet selection adds one local pack'
    Assert-Equal $false $deblurLocal[0].inSource 'Initially missing NAFNet comes from the validated cache'
    Assert-Equal 6 @($deblurLocal[0].pack.files | Where-Object { $_.Name -cin @('model.onnx','model.json','LICENSE','conversion.json','source-manifest.json','validation.json') }).Count 'Model graph, descriptor, license, conversion, source and validation all travel in pack'
    $deblurModelRoot = Join-Path $deblurSource "models/$deblurDirectory"
    New-Item -ItemType Directory -Path $deblurModelRoot -Force | Out-Null
    foreach ($file in $deblurLocal[0].pack.files) {
        $destination = Join-Path $deblurModelRoot ([IO.Path]::GetRelativePath($deblurCache,$file.FullName))
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        if ($file.Name -ceq 'model.onnx') { New-HardLinkChecked $file.FullName $destination }
        else { Copy-Item -LiteralPath $file.FullName -Destination $destination }
    }
    $deblurLicense = "licenses/Model-$deblurDirectory-LICENSE"
    Write-Fixture (Join-Path $deblurSource $deblurLicense) ([IO.File]::ReadAllText((Join-Path $deblurCache 'LICENSE')))
    Assert-Rejected { Get-NativeOptionalRestorationPacks $deblurManifest $deblurSource $missing $missing $deblurSelection $deblurCache } 'not declared in its manifest' 'NAFNet files cannot bypass source manifest declaration'
    Update-NativeOptionalRestorationManifest $deblurManifest $deblurLocal
    $deblurEntry = @($deblurManifest.models)[0]
    Assert-Equal $deblurId $deblurEntry.id 'Manifest preserves NAFNet model ID'
    Assert-Equal $deblurDirectory $deblurEntry.canonicalBasename 'Manifest records the actual packaged directory'
    Assert-Equal $deblurHash $deblurEntry.sha256 'Manifest records pinned graph hash'
    Assert-Equal 'experimental-optional-deblur' $deblurEntry.role 'Manifest distinguishes NAFNet from super resolution'
    Assert-Equal $deblurLicense $deblurEntry.licensePath 'Manifest names the separate model license'
    Assert-Equal "models/$deblurDirectory/source-manifest.json" $deblurEntry.provenancePath 'Manifest names NAFNet provenance'
    Assert-Equal $false $deblurManifest.deblurRestorationDefaultEnabled 'Manifest leaves NAFNet disabled'
    $deblurDefaults = @{ deblurRestorationDefaultEnabled = $true }
    Set-NativeRestorationDefaults $deblurDefaults $false $false $true
    Assert-Equal "models/$deblurDirectory" $deblurDefaults.deblurRestorationModelDirectory 'Selected NAFNet is discoverable in model directory'
    Assert-Equal $false $deblurDefaults.deblurRestorationDefaultEnabled 'Selected NAFNet still defaults off'
    $deblurFromSource = @(Get-NativeOptionalRestorationPacks $deblurManifest $deblurSource $missing $missing $deblurSelection $missing)
    Assert-Equal 1 @($deblurFromSource | Where-Object { $_.inSource }).Count 'NAFNet source package reuses verified bytes without a cache'
    Assert-Equal $deblurHash (Hash-File (Join-Path $deblurModelRoot 'model.onnx')) 'Linked fixture has the verified graph bytes'
    $badDeblurDescriptor = Get-Content -LiteralPath (Join-Path $deblurModelRoot 'model.json') -Raw | ConvertFrom-Json
    $badDeblurDescriptor.adapter.id = 'wrong-adapter'
    Assert-Rejected { Assert-NativeDeblurModelIdentity -Descriptor $badDeblurDescriptor -GraphHash $deblurHash -Directory $deblurModelRoot } 'identity/capabilities mismatch' 'Wrong NAFNet adapter is rejected'
    Assert-Rejected { Assert-NativeDeblurModelIdentity -Descriptor $deblurFullPack.descriptor -GraphHash ('0' * 64) -Directory $deblurCache } 'identity/capabilities mismatch' 'Wrong NAFNet graph is rejected even if a descriptor claims it'
    $badDeblurManifest = $deblurManifest | ConvertTo-Json -Depth 12 | ConvertFrom-Json -AsHashtable
    $badDeblurManifest.models[0].id = 'wrong-id'
    Assert-Rejected { Get-NativeOptionalRestorationPacks $badDeblurManifest $deblurSource $missing $missing $deblurSelection $missing } 'manifest identity mismatch' 'Reused NAFNet must match its source manifest identity'
    $missingSidecarRoot = Join-Path $testRoot 'nafnet-with-missing-source-sidecar'
    New-Item -ItemType Directory -Path $missingSidecarRoot | Out-Null
    foreach ($name in @('model.json','LICENSE','validation.json','conversion.json')) {
        Copy-Item -LiteralPath (Join-Path $deblurCache $name) -Destination (Join-Path $missingSidecarRoot $name)
    }
    New-HardLinkChecked (Join-Path $deblurCache 'model.onnx') (Join-Path $missingSidecarRoot 'model.onnx')
    Assert-Rejected { Read-ValidatedModelPack $missingSidecarRoot 'NAFNet model' $deblurId 'video-restoration' } 'provenance sidecar is missing' 'Missing NAFNet source record is rejected'
    $badDeblurLicense = Join-Path $testRoot 'source-with-tampered-deblur-license'
    $badDeblurModelRoot = Join-Path $badDeblurLicense "models/$deblurDirectory"
    New-Item -ItemType Directory -Path $badDeblurModelRoot -Force | Out-Null
    foreach ($file in (Get-ChildItem -LiteralPath $deblurModelRoot -Recurse -File)) {
        $destination = Join-Path $badDeblurModelRoot ([IO.Path]::GetRelativePath($deblurModelRoot,$file.FullName))
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        if ($file.Name -ceq 'model.onnx') { New-HardLinkChecked $file.FullName $destination }
        else { Copy-Item -LiteralPath $file.FullName -Destination $destination }
    }
    Write-Fixture (Join-Path $badDeblurLicense $deblurLicense) 'TAMPERED LICENSE'
    Assert-Rejected { Get-NativeOptionalRestorationPacks $deblurManifest $badDeblurLicense $missing $missing $deblurSelection $missing } 'bundled license mismatch' 'Reused NAFNet rejects a changed separate license'
    $deblurVerified = $true
}

# No legacy descriptor or graph is required when the exact recovery identity
# is absent. A precise match makes validation mandatory, not a silent fallback.
$unreferenced = Resolve-NativeLegacyModelMigration (Join-Path $testRoot 'clean-new-package') $missing
Assert-Equal $false $unreferenced.reference.referenced 'Fresh package does not require legacy v2 files'
Assert-Equal $null $unreferenced.pack 'Fresh package never validates an unused v2 pack'
$legacyRoot = Join-Path $testRoot 'legacy-reference-package'
Write-Fixture (Join-Path $legacyRoot 'data/unrelated.json') '{"path":"models/cache/rife-4.25-fp32-sdr8-v2"}'
Assert-Equal $false (Resolve-NativeLegacyModelMigration $legacyRoot $missing).reference.referenced 'A path token alone never activates v2 migration'
$legacyFixture = New-ModelFixture 'rife-4.25-fp32-sdr8' (Join-Path $testRoot 'legacy-cache') 'rife'
$legacyDescriptorPath = Join-Path $legacyFixture 'model.json'
$legacyDescriptor = Get-Content -Raw -LiteralPath $legacyDescriptorPath | ConvertFrom-Json -AsHashtable
$legacyDescriptor['version'] = '4.25'; $legacyDescriptor['licenseFile'] = 'LICENSE.txt'
$legacyDescriptor['validationFile'] = 'validation.json'; $legacyDescriptor['sourceManifestFile'] = 'source.json'
Write-Fixture $legacyDescriptorPath ($legacyDescriptor | ConvertTo-Json -Depth 8)
$legacyHash = Hash-File (Join-Path $legacyFixture 'model.onnx')
$referenceFile = Join-Path $legacyRoot 'data/checkpoint.json'
Write-Fixture $referenceFile (@{ path = 'models/cache/rife-4.25-fp32-sdr8-v2'; id = 'rife-4.25-fp32-sdr8'; version = '4.25'; sha256 = $legacyHash } | ConvertTo-Json -Compress)
$referenceHash = Hash-File $referenceFile
Assert-Rejected { Resolve-NativeLegacyModelMigration $legacyRoot $missing $legacyHash } '不存在' 'Referenced missing legacy pack is rejected'
$migrated = Resolve-NativeLegacyModelMigration $legacyRoot $legacyFixture $legacyHash
Assert-Equal $true $migrated.reference.referenced 'Exact path plus pinned identity activates legacy validation'
Assert-Equal 'rife-4.25-fp32-sdr8-v2' $migrated.pack.destination 'Recovery keeps the exact historical v2 basename'
Assert-Equal $referenceHash (Hash-File $referenceFile) 'Legacy probing never changes recovery data'

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
    baseModelSelectionVerified = $true; excludedModelsNotRead = $true; userDataNotCopied = $true
    legacyModelValidatedOnlyWhenReferenced = $true; deblurCacheVerified = $deblurVerified
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'test-result.json') -Encoding utf8NoBOM
Write-Output "Native model reuse checks passed ($checks). Fixtures retained at $testRoot; real NAFNet cache verified=$deblurVerified"
