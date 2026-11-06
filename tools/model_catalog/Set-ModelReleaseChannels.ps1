param(
    [Parameter(Mandatory = $true)][string]$DistributionManifestPath,
    [Parameter(Mandatory = $true)][string]$ArchivePath,
    [Parameter(Mandatory = $true)][string]$ChannelsPath,
    [Parameter(Mandatory = $true)][string]$OutputManifestPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ModelRelease.Common.ps1')

$source = Read-BoundedJsonFile -Path $DistributionManifestPath -MaximumBytes 1MB -Description 'The input distribution manifest'
$manifest = $source.Json
Assert-DistributionManifest -Manifest $manifest
$outputFullPath = [System.IO.Path]::GetFullPath($OutputManifestPath)
if (Test-Path -LiteralPath $outputFullPath) { throw "Refusing to overwrite existing output '$outputFullPath'." }
if ($manifest.state -cne 'local-candidate-not-published') {
    throw 'Only a local candidate manifest can be finalized into a published-channel manifest.'
}
if (@($manifest.downloadChannels).Count -ne 0) { throw 'The local candidate already declares download channels.' }
$null = Assert-LocalArchive -ArchivePath $ArchivePath -Artifact $manifest.artifact
$channels = @(Read-ModelReleaseChannels -Path $ChannelsPath -Artifact $manifest.artifact)

for ($index = 0; $index -lt $channels.Count; $index++) {
    $channel = $channels[$index]
    Write-Output "Checking anonymous archive read-back $($index + 1)/$($channels.Count): $($channel.name)"
    $null = Get-ModelRemoteDigest -Url $channel.archiveUrl -ExpectedSizeBytes ([long]$manifest.artifact.sizeBytes) `
        -ExpectedSha256 ([string]$manifest.artifact.sha256) -MaximumBytes 1GB -Description "Archive channel '$($channel.name)'"
}

$manifest.state = 'archive-channels-verified'
$manifest.downloadChannels = @($channels)
$manifest.notes = @($manifest.notes | Where-Object {
    [string]$_ -cne 'No remote download channel is claimed until the exact archive hash is independently verified on that host.'
})
$json = ConvertTo-Json -InputObject $manifest -Depth 32
Write-ModelReleaseFile -Path $OutputManifestPath -Content ($json + "`n")
$output = Get-Item -LiteralPath $OutputManifestPath
$outputHash = (Get-FileHash -LiteralPath $output.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "Finalized distribution manifest: $($output.FullName)"
Write-Output "Manifest read-back pin for upload: $($output.Length) bytes, SHA-256 $outputHash"
Write-Output 'Upload these exact sidecar bytes to every manifestUrl before building the signed catalog payload.'
