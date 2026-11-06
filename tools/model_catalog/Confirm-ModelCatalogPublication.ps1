param(
    [Parameter(Mandatory = $true)][string]$EnvelopePath,
    [Parameter(Mandatory = $true)][string]$ReadbackReceiptPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'ModelRelease.Common.ps1')

$null = Assert-OfficialModelCatalogEnvelope -Path $EnvelopePath
$envelope = Get-Item -LiteralPath $EnvelopePath -Force
if ($envelope.PSIsContainer -or ($envelope.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
    $envelope.Length -lt 1 -or $envelope.Length -gt 384KB) {
    throw 'The signed catalog envelope must be a regular file between 1 byte and 384 KiB.'
}
$envelopeSha = (Get-FileHash -LiteralPath $envelope.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$receiptFullPath = [System.IO.Path]::GetFullPath($ReadbackReceiptPath)
if (Test-Path -LiteralPath $receiptFullPath) { throw "Refusing to overwrite existing output '$receiptFullPath'." }
$catalogUrls = @(
    'https://rifelove-updatelog.pages.dev/model-catalog/v1/catalog.signed.txt',
    'https://raw.githubusercontent.com/Aelurillus/RiFeLove/main/model-catalog/v1/catalog.signed.txt'
)

$hosts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$readbacks = [System.Collections.Generic.List[object]]::new()
foreach ($url in $catalogUrls) {
    $uri = Assert-ModelHttpsUrl -Url $url
    if (-not $hosts.Add($uri.DnsSafeHost.TrimEnd('.').ToLowerInvariant())) {
        throw 'Catalog fallback URLs must be served by distinct hosts.'
    }
    $result = Get-ModelRemoteDigest -Url $url -ExpectedSizeBytes ([long]$envelope.Length) `
        -ExpectedSha256 $envelopeSha -MaximumBytes 384KB -Description "Signed catalog source '$url'"
    $readbacks.Add([ordered]@{ url = $url; readback = $result })
}

$receipt = [ordered]@{
    schemaVersion = 1
    verifiedAtUtc = [DateTime]::UtcNow.ToString('o')
    envelope = [ordered]@{ fileName = $envelope.Name; sizeBytes = [long]$envelope.Length; sha256 = $envelopeSha }
    sources = @($readbacks)
}
Write-ModelReleaseFile -Path $ReadbackReceiptPath -Content ((ConvertTo-Json -InputObject $receipt -Depth 12) + "`n")
Write-Output "All signed catalog fallback sources serve the exact local envelope."
Write-Output "Anonymous read-back receipt: $([System.IO.Path]::GetFullPath($ReadbackReceiptPath))"
