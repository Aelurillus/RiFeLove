param(
    [Parameter(Mandatory = $true)][string]$DistributionManifestPath,
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [Parameter(Mandatory = $true)][string]$OutputPayloadPath,
    [Parameter(Mandatory = $true)][string]$ReadbackReceiptPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ModelRelease.Common.ps1')

$source = Read-BoundedJsonFile -Path $DistributionManifestPath -MaximumBytes 1MB -Description 'The finalized distribution manifest'
$manifest = $source.Json
Assert-DistributionManifest -Manifest $manifest
if ($manifest.state -cne 'archive-channels-verified') { throw 'Archive channel read-backs have not been finalized in the distribution manifest.' }
$payloadFullPath = [System.IO.Path]::GetFullPath($OutputPayloadPath)
$receiptFullPath = [System.IO.Path]::GetFullPath($ReadbackReceiptPath)
if ($payloadFullPath -ieq $receiptFullPath) { throw 'Payload and receipt outputs must use different paths.' }
foreach ($output in @($payloadFullPath, $receiptFullPath)) {
    if (Test-Path -LiteralPath $output) { throw "Refusing to overwrite existing output '$output'." }
}
$channels = @(Normalize-ModelReleaseChannels -Channels @($manifest.downloadChannels) -Artifact $manifest.artifact)
if (@($manifest.downloadChannels).Count -ne $channels.Count) { throw 'The finalized manifest has an invalid channel list.' }
for ($index = 0; $index -lt $channels.Count; $index++) {
    foreach ($field in @('name', 'archiveUrl', 'manifestUrl')) {
        if ([string]$manifest.downloadChannels[$index][$field] -cne [string]$channels[$index][$field]) {
            throw 'The finalized manifest download channels do not match the validated channel contract.'
        }
    }
}
$null = Assert-LocalArchive -ArchivePath $ArchivePath -Artifact $manifest.artifact

$manifestSize = [long]$source.File.Length
$manifestSha = (Get-FileHash -LiteralPath $source.File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$channelReadbacks = [System.Collections.Generic.List[object]]::new()
for ($index = 0; $index -lt $channels.Count; $index++) {
    $channel = $channels[$index]
    Write-Output "Verifying anonymous archive and manifest read-back $($index + 1)/$($channels.Count): $($channel.name)"
    $archiveResult = Get-ModelRemoteDigest -Url $channel.archiveUrl -ExpectedSizeBytes ([long]$manifest.artifact.sizeBytes) `
        -ExpectedSha256 ([string]$manifest.artifact.sha256) -MaximumBytes 1GB -Description "Archive channel '$($channel.name)'"
    $manifestResult = Get-ModelRemoteDigest -Url $channel.manifestUrl -ExpectedSizeBytes $manifestSize `
        -ExpectedSha256 $manifestSha -MaximumBytes 1MB -Description "Manifest channel '$($channel.name)'"
    $channelReadbacks.Add([ordered]@{
        name = [string]$channel.name
        archiveUrl = [string]$channel.archiveUrl
        archiveReadback = $archiveResult
        manifestUrl = [string]$channel.manifestUrl
        manifestReadback = $manifestResult
    })
}

$model = $manifest.model
$payload = [ordered]@{
    schemaVersion = 1
    models = @([ordered]@{
        id = [string]$model.id
        version = [string]$model.version
        family = 'rife'
        name = [string]$model.displayName
        packageId = [string]$manifest.packageId
        modelSha256 = [string]$model.modelSha256
        recommended = $true
        manifest = [ordered]@{ sizeBytes = $manifestSize; sha256 = $manifestSha }
        channels = @($channels)
    })
}
$payloadJson = ConvertTo-Json -InputObject $payload -Depth 16
if ([System.Text.Encoding]::UTF8.GetByteCount($payloadJson) -gt 256KB) { throw 'The model catalog payload exceeds the native client limit.' }

$receipt = [ordered]@{
    schemaVersion = 1
    verifiedAtUtc = [DateTime]::UtcNow.ToString('o')
    packageId = [string]$manifest.packageId
    model = [ordered]@{
        id = [string]$model.id
        version = [string]$model.version
        family = [string]$model.family
        modelSha256 = [string]$model.modelSha256
    }
    artifact = [ordered]@{
        fileName = [string]$manifest.artifact.fileName
        sizeBytes = [long]$manifest.artifact.sizeBytes
        sha256 = [string]$manifest.artifact.sha256
    }
    distributionManifest = [ordered]@{ sizeBytes = $manifestSize; sha256 = $manifestSha }
    channels = @($channelReadbacks)
}
$receiptJson = ConvertTo-Json -InputObject $receipt -Depth 16

Write-ModelReleaseFile -Path $OutputPayloadPath -Content ($payloadJson + "`n")
try {
    Write-ModelReleaseFile -Path $ReadbackReceiptPath -Content ($receiptJson + "`n")
} catch {
    [System.IO.File]::Delete([System.IO.Path]::GetFullPath($OutputPayloadPath))
    throw
}
Write-Output "Catalog payload ready for signing: $([System.IO.Path]::GetFullPath($OutputPayloadPath))"
Write-Output "Anonymous read-back receipt: $([System.IO.Path]::GetFullPath($ReadbackReceiptPath))"
