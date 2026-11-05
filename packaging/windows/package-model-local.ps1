param(
    [Parameter(Mandatory = $true)]
    [string]$ModelDirectory,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $scriptRoot '../../out/model-distribution'
}
$sourceRoot = (Resolve-Path -LiteralPath $ModelDirectory).Path
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$descriptorPath = Join-Path $sourceRoot 'model.json'
if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) {
    throw 'The selected directory does not contain model.json.'
}

$descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
if ($descriptor.schemaVersion -ne 2 -or $descriptor.family -ne 'rife' -or
    $descriptor.id -ne 'rife-4.25-fp32-sdr8' -or $descriptor.version -ne '4.25') {
    throw 'Only the locally qualified RIFE 4.25 SDR pack is eligible for this distribution package.'
}
if ($descriptor.license.id -ne 'MIT' -or $descriptor.license.file -ne 'LICENSE.txt' -or
    $descriptor.provenance.manifestFile -ne 'source-manifest.json' -or
    $descriptor.validation.reportFile -ne 'validation.json') {
    throw 'The model descriptor does not match the reviewed license and sidecar contract.'
}

$requiredFiles = @('LICENSE.txt', 'model.json', 'model.onnx', 'source-manifest.json', 'validation.json')
$actualFiles = @(Get-ChildItem -LiteralPath $sourceRoot -File -Force | ForEach-Object { $_.Name } | Sort-Object)
$expectedFiles = @($requiredFiles | Sort-Object)
if (Compare-Object -ReferenceObject $expectedFiles -DifferenceObject $actualFiles) {
    throw 'The model directory contains missing or unreviewed files; package only the exact validated pack.'
}
foreach ($entry in Get-ChildItem -LiteralPath $sourceRoot -Force) {
    if (($entry.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Model package entries cannot be reparse points: $($entry.Name)"
    }
}

$expectedHashes = @{
    'LICENSE.txt' = [string]$descriptor.license.sha256
    'model.onnx' = [string]$descriptor.sha256
    'source-manifest.json' = [string]$descriptor.provenance.manifestSha256
    'validation.json' = [string]$descriptor.validation.reportSha256
}
$fileRecords = [System.Collections.Generic.List[object]]::new()
foreach ($name in $requiredFiles) {
    $path = Join-Path $sourceRoot $name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($name -ne 'model.json' -and $hash -ne $expectedHashes[$name].ToLowerInvariant()) {
        throw "The $name SHA-256 does not match model.json."
    }
    $fileRecords.Add([ordered]@{
        path = $name
        sizeBytes = (Get-Item -LiteralPath $path).Length
        sha256 = $hash
    })
}

$null = New-Item -ItemType Directory -Path $outputRoot -Force
$archiveName = 'RifeLove-RIFE-4.25-FP32-SDR8-v3.zip'
$archivePath = Join-Path $outputRoot $archiveName
$manifestPath = Join-Path $outputRoot 'RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json'
if ((Test-Path -LiteralPath $archivePath) -or (Test-Path -LiteralPath $manifestPath)) {
    throw 'The output package already exists. Choose a new output directory to preserve previous artifacts.'
}

$stageRoot = Join-Path $outputRoot ('.model-stage-' + [guid]::NewGuid().ToString('N'))
$modelStage = Join-Path $stageRoot 'rife-4.25-fp32-sdr8-v3'
$zipStream = $null
$archive = $null
try {
    $null = New-Item -ItemType Directory -Path $modelStage -Force
    foreach ($name in $requiredFiles) {
        Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination (Join-Path $modelStage $name)
    }

    $readme = @'
RifeLove RIFE 4.25 FP32 SDR8 model pack

Install: extract this archive, then open RifeLove's Model Center, choose
"Register local folder", select the rife-4.25-fp32-sdr8-v3 folder, verify it,
and set it as the RIFE default. No Python environment or command-line tool is
required by the installed desktop application.

License and provenance: the trained model archive is linked by the author of
Practical-RIFE, whose pinned README states that linked trained model contents
are covered by the project's MIT license. The pack includes the exact license,
source manifest, validation report, and SHA-256-bound model descriptor.

Qualification: the validation.json included in this pack records six small-
profile SDR8 reference cases on PyTorch CPU and ONNX Runtime CPU. Separate
project records cover local CUDA/TensorRT FP32 numerical checks and a tagged
SDR video pipeline run; those records are not included in this archive. None
of these checks establishes a universal model ranking or perceptual quality
on all footage. HDR is not qualified. The project-pinned model hash verifies
package integrity; it is not an upstream author signature.
'@
    $readmePath = Join-Path $stageRoot 'README.txt'
    [System.IO.File]::WriteAllText($readmePath, $readme, [System.Text.UTF8Encoding]::new($false))

    $zipStream = [System.IO.File]::Open($archivePath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    $archive = [System.IO.Compression.ZipArchive]::new($zipStream, [System.IO.Compression.ZipArchiveMode]::Create, $false)
    $zipFiles = @(
        [pscustomobject]@{ Source = $readmePath; Relative = 'README.txt' }
    ) + @($requiredFiles | ForEach-Object {
        [pscustomobject]@{ Source = (Join-Path $modelStage $_); Relative = "rife-4.25-fp32-sdr8-v3/$_" }
    })
    foreach ($file in $zipFiles) {
        $entry = $archive.CreateEntry($file.Relative, [System.IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $input = [System.IO.File]::OpenRead($file.Source)
        $entryStream = $entry.Open()
        try { $input.CopyTo($entryStream) }
        finally { $entryStream.Dispose(); $input.Dispose() }
    }
    $archive.Dispose()
    $archive = $null
    $zipStream.Dispose()
    $zipStream = $null

    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $distribution = [ordered]@{
        schemaVersion = 1
        packageId = 'rifelove-rife-4.25-fp32-sdr8-v3'
        state = 'local-candidate-not-published'
        model = [ordered]@{
            id = $descriptor.id
            version = $descriptor.version
            family = $descriptor.family
            displayName = $descriptor.displayName
            license = $descriptor.license.id
            upstreamRepository = 'https://github.com/hzwer/Practical-RIFE'
            upstreamCommit = 'bbfd2ea90910789a860ea3e2b32a240cd577b75e'
            licenseScopeEvidence = 'https://github.com/hzwer/Practical-RIFE/blob/bbfd2ea90910789a860ea3e2b32a240cd577b75e/README.md#trained-model'
            supportedInputProfiles = @($descriptor.capabilities.supportedInputProfiles)
            hdrQualified = [bool]$descriptor.capabilities.hdrQualified
            validatedBackends = @($descriptor.validation.validatedBackends)
            modelSha256 = [string]$descriptor.sha256
        }
        files = @($fileRecords)
        artifact = [ordered]@{
            fileName = $archiveName
            sizeBytes = (Get-Item -LiteralPath $archivePath).Length
            sha256 = $archiveHash
        }
        downloadChannels = @()
        notes = @(
            'No remote download channel is claimed until the exact archive hash is independently verified on that host.'
            'The upstream author did not publish an authoritative checksum for the trained-weight archive.'
        )
    }
    $json = $distribution | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText($manifestPath, $json + "`n", [System.Text.UTF8Encoding]::new($false))
    Get-Item -LiteralPath $archivePath, $manifestPath | Select-Object FullName, Length
} catch {
    if ($archive) { $archive.Dispose() }
    if ($zipStream) { $zipStream.Dispose() }
    foreach ($partial in @($archivePath, $manifestPath)) {
        if (Test-Path -LiteralPath $partial -PathType Leaf) { Remove-Item -LiteralPath $partial }
    }
    throw
} finally {
    if (Test-Path -LiteralPath $stageRoot -PathType Container) {
        $resolvedStage = [System.IO.Path]::GetFullPath($stageRoot)
        $resolvedOutput = [System.IO.Path]::GetFullPath($outputRoot).TrimEnd('\') + '\'
        if (-not $resolvedStage.StartsWith($resolvedOutput, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to remove a staging directory outside the selected output directory.'
        }
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }
}
