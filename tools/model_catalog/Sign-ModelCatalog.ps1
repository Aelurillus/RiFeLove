param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [string]$PublisherThumbprint = '588D95298D74130953132DA50A33A26799D7AEC0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$payloadFile = (Resolve-Path -LiteralPath $PayloadPath).Path
$outputFile = [System.IO.Path]::GetFullPath($OutputPath)
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
$models = @($catalog.models)
$recommended = @($models | Where-Object { $_.recommended -is [bool] -and $_.recommended })
if ($models.Count -lt 1 -or $models.Count -gt 32 -or $recommended.Count -ne 1) {
    throw 'The catalog must contain 1–32 models and exactly one recommended model.'
}
$model = $recommended[0]
if ($model.family -ne 'rife' -or [string]::IsNullOrWhiteSpace([string]$model.packageId) -or
    [string]::IsNullOrWhiteSpace([string]$model.id) -or [string]::IsNullOrWhiteSpace([string]$model.version) -or
    [string]$model.modelSha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'The recommended entry must identify a RIFE model package and its SHA-256.'
}
if ($model.manifest.sizeBytes -lt 1 -or $model.manifest.sizeBytes -gt 1MB -or
    [string]$model.manifest.sha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'The recommended entry must pin a distribution manifest under 1 MiB.'
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
    -not $certificate.HasPrivateKey -or $certificate.NotAfter -le [DateTime]::Now) {
    throw 'The selected publisher certificate is missing, expired, or has the wrong subject.'
}

$privateKey = [System.Security.Cryptography.X509Certificates.ECDsaCertificateExtensions]::GetECDsaPrivateKey($certificate)
if (-not $privateKey) {
    throw 'The publisher certificate does not expose an ECDSA private key in the current user store.'
}
try {
    $signature = $privateKey.SignData(
        $payloadBytes,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)
    if ($signature.Length -ne 64 -or -not $privateKey.VerifyData(
        $payloadBytes,
        $signature,
        [System.Security.Cryptography.HashAlgorithmName]::SHA256,
        [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) {
        throw 'The model catalog signature did not pass its local verification.'
    }
} finally {
    $privateKey.Dispose()
}

$keyId = 'rifelove-model-catalog-2026-v1'
$envelope = @(
    'RIFELOVE MODEL CATALOG 1'
    "key-id=$keyId"
    "payload=$([Convert]::ToBase64String($payloadBytes))"
    "signature=$([Convert]::ToBase64String($signature))"
) -join "`n"
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($outputFile)) | Out-Null
[System.IO.File]::WriteAllText($outputFile, "$envelope`n", [System.Text.UTF8Encoding]::new($false))
Write-Output "Signed catalog payload: $($payloadBytes.Length) bytes"
Write-Output "Catalog envelope: $outputFile"
