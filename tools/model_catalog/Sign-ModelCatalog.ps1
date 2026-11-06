param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [string]$DistributionManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$ReadbackReceiptPath,

    [string]$PublisherThumbprint = '588D95298D74130953132DA50A33A26799D7AEC0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ModelRelease.Common.ps1')

$payloadFile = (Resolve-Path -LiteralPath $PayloadPath).Path
$outputFile = [System.IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $outputFile) { throw "Refusing to overwrite existing output '$outputFile'." }
$payloadBytes = [System.IO.File]::ReadAllBytes($payloadFile)
if ($payloadBytes.Length -eq 0 -or $payloadBytes.Length -gt 256KB) {
    throw 'The signed model catalog payload must be between 1 byte and 256 KiB.'
}

$utf8 = [System.Text.UTF8Encoding]::new($false, $true)
$payloadText = $utf8.GetString($payloadBytes)
$catalog = ConvertFrom-Json -InputObject $payloadText -AsHashtable
if ($catalog.schemaVersion -ne 1) {
    throw 'Only model catalog schema version 1 can be signed.'
}
$distribution = Read-BoundedJsonFile -Path $DistributionManifestPath -MaximumBytes 1MB -Description 'The finalized distribution manifest'
$manifest = $distribution.Json
Assert-DistributionManifest -Manifest $manifest
if ($manifest.state -cne 'archive-channels-verified') { throw 'A signed catalog requires an archive-channel-verified distribution manifest.' }
$manifestHash = (Get-FileHash -LiteralPath $distribution.File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$models = @($catalog.models)
$recommended = @($models | Where-Object { $_.recommended -is [bool] -and $_.recommended })
if ($models.Count -lt 1 -or $models.Count -gt 32 -or $recommended.Count -ne 1) {
    throw 'The catalog must contain 1–32 models and exactly one recommended model.'
}
$model = $recommended[0]
if ($model.family -ne 'rife' -or [string]::IsNullOrWhiteSpace([string]$model.name) -or ([string]$model.name).Length -gt 128 -or
    [string]::IsNullOrWhiteSpace([string]$model.packageId) -or
    [string]::IsNullOrWhiteSpace([string]$model.id) -or [string]::IsNullOrWhiteSpace([string]$model.version) -or
    [string]$model.modelSha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'The recommended entry must identify a RIFE model package and its SHA-256.'
}
if ($model.manifest.sizeBytes -lt 1 -or $model.manifest.sizeBytes -gt 1MB -or
    [string]$model.manifest.sha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'The recommended entry must pin a distribution manifest under 1 MiB.'
}
$manifestChannels = @(Normalize-ModelReleaseChannels -Channels @($manifest.downloadChannels) -Artifact $manifest.artifact)
$catalogChannels = @(Normalize-ModelReleaseChannels -Channels @($model.channels) -Artifact $manifest.artifact)
if ([long]$model.manifest.sizeBytes -ne [long]$distribution.File.Length -or
    [string]$model.manifest.sha256 -cne $manifestHash -or
    [string]$model.packageId -cne [string]$manifest.packageId -or
    [string]$model.id -cne [string]$manifest.model.id -or
    [string]$model.version -cne [string]$manifest.model.version -or
    [string]$model.modelSha256 -cne [string]$manifest.model.modelSha256 -or
    $catalogChannels.Count -ne $manifestChannels.Count) {
    throw 'The catalog entry does not match the exact finalized distribution manifest.'
}
for ($index = 0; $index -lt $manifestChannels.Count; $index++) {
    foreach ($field in @('name', 'archiveUrl', 'manifestUrl')) {
        if ([string]$catalogChannels[$index][$field] -cne [string]$manifestChannels[$index][$field]) {
            throw 'Catalog channels do not match the finalized distribution manifest.'
        }
    }
}

$receiptFile = Read-BoundedJsonFile -Path $ReadbackReceiptPath -MaximumBytes 64KB -Description 'The anonymous read-back receipt'
$receipt = $receiptFile.Json
if ($receipt.schemaVersion -ne 1 -or [string]$receipt.packageId -cne [string]$manifest.packageId -or
    [long]$receipt.distributionManifest.sizeBytes -ne [long]$distribution.File.Length -or
    [string]$receipt.distributionManifest.sha256 -cne $manifestHash -or
    [string]$receipt.artifact.fileName -cne [string]$manifest.artifact.fileName -or
    [long]$receipt.artifact.sizeBytes -ne [long]$manifest.artifact.sizeBytes -or
    [string]$receipt.artifact.sha256 -cne [string]$manifest.artifact.sha256 -or
    [string]$receipt.model.id -cne [string]$model.id -or
    [string]$receipt.model.version -cne [string]$model.version -or
    [string]$receipt.model.family -cne [string]$model.family -or
    [string]$receipt.model.modelSha256 -cne [string]$model.modelSha256) {
    throw 'The anonymous read-back receipt does not pin the signed catalog package.'
}
$verifiedAt = [DateTimeOffset]::Parse([string]$receipt.verifiedAtUtc, [Globalization.CultureInfo]::InvariantCulture,
    [Globalization.DateTimeStyles]::RoundtripKind)
$receiptAge = [DateTimeOffset]::UtcNow - $verifiedAt.ToUniversalTime()
if ($receiptAge.TotalMinutes -lt -5 -or $receiptAge.TotalHours -gt 24) {
    throw 'The anonymous read-back receipt is more than 24 hours old or has a future timestamp.'
}
$receiptChannels = @($receipt.channels)
if ($receiptChannels.Count -ne $manifestChannels.Count) {
    throw 'The anonymous read-back receipt is missing one or more model channels.'
}
$finalArchiveHosts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
for ($index = 0; $index -lt $manifestChannels.Count; $index++) {
    $channel = $manifestChannels[$index]
    $receiptChannel = $receiptChannels[$index]
    if ([string]$receiptChannel.name -cne [string]$channel.name -or
        [string]$receiptChannel.archiveUrl -cne [string]$channel.archiveUrl -or
        [string]$receiptChannel.manifestUrl -cne [string]$channel.manifestUrl -or
        [int]$receiptChannel.archiveReadback.status -ne 200 -or
        [long]$receiptChannel.archiveReadback.sizeBytes -ne [long]$manifest.artifact.sizeBytes -or
        [string]$receiptChannel.archiveReadback.sha256 -cne [string]$manifest.artifact.sha256 -or
        [int]$receiptChannel.manifestReadback.status -ne 200 -or
        [long]$receiptChannel.manifestReadback.sizeBytes -ne [long]$distribution.File.Length -or
        [string]$receiptChannel.manifestReadback.sha256 -cne $manifestHash) {
        throw "Anonymous read-back receipt verification failed for model channel '$($channel.name)'."
    }
    foreach ($finalUrl in @([string]$receiptChannel.archiveReadback.finalUrl, [string]$receiptChannel.manifestReadback.finalUrl)) {
        $finalUri = $null
        if (-not [Uri]::TryCreate($finalUrl, [UriKind]::Absolute, [ref]$finalUri) -or $finalUri.Scheme -cne 'https' -or
            -not [string]::IsNullOrEmpty($finalUri.UserInfo) -or -not [string]::IsNullOrEmpty($finalUri.Fragment)) {
            throw "Anonymous read-back receipt contains an unsafe redirected URL for model channel '$($channel.name)'."
        }
    }
    $archiveFinalUri = [Uri]$receiptChannel.archiveReadback.finalUrl
    if (-not $finalArchiveHosts.Add($archiveFinalUri.DnsSafeHost.TrimEnd('.').ToLowerInvariant())) {
        throw 'Archive channels redirect to the same host and are not independent download sources.'
    }
}
$channels = @($model.channels)
if ($channels.Count -lt 2 -or $channels.Count -gt 5) {
    throw 'The recommended model must have two to five download channels.'
}
$hosts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($channel in $channels) {
    foreach ($urlText in @([string]$channel.archiveUrl, [string]$channel.manifestUrl)) {
        $uri = $null
        if (-not [uri]::TryCreate($urlText, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -ne 'https' -or
            $uri.UserInfo -or $uri.Fragment) {
            throw 'Every channel must use a direct HTTPS archive and manifest URL without credentials or fragments.'
        }
    }
    $archiveUri = [uri]$channel.archiveUrl
    if (-not $hosts.Add($archiveUri.DnsSafeHost)) {
        throw 'Download channels must use independent archive hosts.'
    }
}
if ($hosts.Count -lt 2) {
    throw 'At least two independent archive hosts are required.'
}

$thumbprint = ($PublisherThumbprint -replace '\s', '').ToUpperInvariant()
$certificatePath = "Cert:\CurrentUser\My\$thumbprint"
$certificate = Get-Item -LiteralPath $certificatePath
if ($certificate.Subject -ne 'CN=RifeLove Model Catalog Publisher' -or
    -not $certificate.HasPrivateKey -or $certificate.NotBefore -gt [DateTime]::Now -or
    $certificate.NotAfter -le [DateTime]::Now) {
    throw 'The selected publisher certificate is missing, expired, or has the wrong subject.'
}

$privateKey = [System.Security.Cryptography.X509Certificates.ECDsaCertificateExtensions]::GetECDsaPrivateKey($certificate)
if (-not $privateKey) {
    throw 'The publisher certificate does not expose an ECDSA private key in the current user store.'
}
$publicKey = [System.Security.Cryptography.X509Certificates.ECDsaCertificateExtensions]::GetECDsaPublicKey($certificate)
if (-not $publicKey) {
    $privateKey.Dispose()
    throw 'The publisher certificate does not expose an ECDSA public key.'
}
try {
    $publicParameters = $publicKey.ExportParameters($false)
    $certificatePoint = ([BitConverter]::ToString([byte[]]($publicParameters.Q.X + $publicParameters.Q.Y))).Replace('-', '').ToLowerInvariant()
    $compiledTrustPoint = '10caa8c48b21b159d9d644507152f577ba8ebdeaa92cb822e684197445a7f6230ec1e21d1ef144e96c916db492e72eeb50df147f7900ee228773c3f677391985'
    if ($certificatePoint -cne $compiledTrustPoint) {
        throw 'The publisher certificate public key does not match the trust root compiled into the native client.'
    }
    $signature = $privateKey.SignData(
        $payloadBytes,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    if ($signature.Length -ne 64 -or -not $publicKey.VerifyData(
        $payloadBytes,
        $signature,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) {
        throw 'The model catalog signature did not pass its local verification.'
    }
} finally {
    $privateKey.Dispose()
    $publicKey.Dispose()
}

$keyId = 'rifelove-model-catalog-2026-v1'
$envelope = @(
    'RIFELOVE MODEL CATALOG 1'
    "key-id=$keyId"
    "payload=$([Convert]::ToBase64String($payloadBytes))"
    "signature=$([Convert]::ToBase64String($signature))"
) -join "`n"
Write-ModelReleaseFile -Path $outputFile -Content "$envelope`n"
Write-Output "Signed catalog payload: $($payloadBytes.Length) bytes"
Write-Output "Matched anonymous read-back receipt: $([System.IO.Path]::GetFullPath($ReadbackReceiptPath))"
Write-Output "Catalog envelope: $outputFile"
