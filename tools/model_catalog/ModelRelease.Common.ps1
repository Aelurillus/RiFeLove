Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-BoundedJsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][long]$MaximumBytes,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $file = Get-Item -LiteralPath $Path -Force
    if ($file.PSIsContainer -or
        ($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $file.Length -lt 1 -or $file.Length -gt $MaximumBytes) {
        throw "$Description must be a regular file between 1 and $MaximumBytes bytes."
    }
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.UTF8Encoding]::new($false, $true))
    $json = ConvertFrom-Json -InputObject $text -AsHashtable
    return [pscustomobject]@{ File = $file; Json = $json; Text = $text }
}

function Assert-ModelSha256 {
    param([Parameter(Mandatory = $true)][string]$Value, [Parameter(Mandatory = $true)][string]$Description)
    if ($Value -cnotmatch '^[0-9a-f]{64}$') { throw "$Description must be a lowercase SHA-256 value." }
}

function Assert-DistributionManifest {
    param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$Manifest)

    if ($Manifest.schemaVersion -ne 1 -or [string]::IsNullOrWhiteSpace([string]$Manifest.packageId)) {
        throw 'The distribution manifest must use schema version 1 and identify its package.'
    }
    if ($Manifest.model.family -ne 'rife' -or [string]::IsNullOrWhiteSpace([string]$Manifest.model.id) -or
        [string]::IsNullOrWhiteSpace([string]$Manifest.model.version) -or
        [string]$Manifest.model.id -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
        [string]$Manifest.model.version -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
        [string]$Manifest.packageId -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
        [string]::IsNullOrWhiteSpace([string]$Manifest.model.displayName) -or
        ([string]$Manifest.model.displayName).Length -gt 128) {
        throw 'The distribution manifest must identify a versioned RIFE model.'
    }
    Assert-ModelSha256 -Value ([string]$Manifest.model.modelSha256) -Description 'The model SHA-256'

    $artifact = $Manifest.artifact
    if (-not $artifact -or [string]$artifact.fileName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.zip$' -or
        [long]$artifact.sizeBytes -lt 1 -or [long]$artifact.sizeBytes -gt 1GB) {
        throw 'The distribution manifest must pin a safe ZIP archive no larger than the client 1 GiB limit.'
    }
    Assert-ModelSha256 -Value ([string]$artifact.sha256) -Description 'The archive SHA-256'
}

function Assert-ModelHttpsUrl {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [string]$ExpectedFileName = ''
    )

    $uri = $null
    if (-not [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri) -or $uri.Scheme -cne 'https' -or
        -not [string]::IsNullOrEmpty($uri.UserInfo) -or -not [string]::IsNullOrEmpty($uri.Fragment) -or
        -not [string]::IsNullOrEmpty($uri.Query) -or [string]::IsNullOrWhiteSpace($uri.DnsSafeHost)) {
        throw 'Every model URL must be a stable HTTPS URL without credentials, queries, or fragments.'
    }
    if ($ExpectedFileName) {
        $lastSegment = $uri.AbsolutePath.Substring($uri.AbsolutePath.LastIndexOf('/') + 1)
        if ([Uri]::UnescapeDataString($lastSegment) -cne $ExpectedFileName) {
            throw "The URL path must end with the exact expected file name '$ExpectedFileName'."
        }
    }
    return $uri
}

function Normalize-ModelReleaseChannels {
    param(
        [Parameter(Mandatory = $true)][object[]]$Channels,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Artifact
    )

    if ($channels.Count -lt 2 -or $channels.Count -gt 5) {
        throw 'Provide two to five independent model download channels.'
    }
    $manifestFileName = [System.IO.Path]::GetFileNameWithoutExtension([string]$Artifact.fileName) + '.distribution.json'
    $hosts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $names = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $normalized = [System.Collections.Generic.List[object]]::new()
    foreach ($channel in $channels) {
        if ($channel -isnot [System.Collections.IDictionary]) { throw 'Each model channel must be a JSON object.' }
        $name = [string]$channel.name
        $archiveUrl = [string]$channel.archiveUrl
        $manifestUrl = [string]$channel.manifestUrl
        if ([string]::IsNullOrWhiteSpace($name) -or $name.Length -gt 64 -or -not $names.Add($name)) {
            throw 'Each channel needs a unique non-empty name of at most 64 characters.'
        }
        $archiveUri = Assert-ModelHttpsUrl -Url $archiveUrl -ExpectedFileName ([string]$Artifact.fileName)
        $null = Assert-ModelHttpsUrl -Url $manifestUrl -ExpectedFileName $manifestFileName
        $hostName = $archiveUri.DnsSafeHost.TrimEnd('.').ToLowerInvariant()
        if (-not $hosts.Add($hostName)) { throw 'Archive channels must use at least two distinct host names.' }
        $normalized.Add([ordered]@{ name = $name; archiveUrl = $archiveUrl; manifestUrl = $manifestUrl })
    }
    if ($hosts.Count -lt 2) { throw 'Archive channels must use at least two distinct host names.' }
    return $normalized.ToArray()
}

function Read-ModelReleaseChannels {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Artifact
    )

    $inputFile = Read-BoundedJsonFile -Path $Path -MaximumBytes 64KB -Description 'The channel file'
    return Normalize-ModelReleaseChannels -Channels @($inputFile.Json) -Artifact $Artifact
}

function Get-ModelRemoteDigest {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][long]$ExpectedSizeBytes,
        [Parameter(Mandatory = $true)][string]$ExpectedSha256,
        [Parameter(Mandatory = $true)][long]$MaximumBytes,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ($ExpectedSizeBytes -lt 1 -or $ExpectedSizeBytes -gt $MaximumBytes) {
        throw "$Description has an invalid expected size."
    }
    $null = Assert-ModelHttpsUrl -Url $Url
    Add-Type -AssemblyName System.Net.Http
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $true
    $handler.MaxAutomaticRedirections = 5
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(20)
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $Url)
    $null = $request.Headers.UserAgent.ParseAdd('RifeLove-model-release-verifier/1.0')
    $response = $null
    $stream = $null
    $cancellation = [System.Threading.CancellationTokenSource]::new()
    $cancellation.CancelAfter([TimeSpan]::FromMinutes(20))
    $hash = [System.Security.Cryptography.IncrementalHash]::CreateHash([System.Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        $response = $client.SendAsync($request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        if ([int]$response.StatusCode -ne 200) { throw "$Description returned HTTP $([int]$response.StatusCode); expected HTTP 200." }
        $finalUri = $response.RequestMessage.RequestUri
        if ($finalUri.Scheme -cne 'https' -or -not [string]::IsNullOrEmpty($finalUri.UserInfo) -or
            -not [string]::IsNullOrEmpty($finalUri.Fragment)) { throw "$Description redirected to an unsafe URL." }
        if ($null -ne $response.Content.Headers.ContentLength -and $response.Content.Headers.ContentLength -ne $ExpectedSizeBytes) {
            throw "$Description returned an unexpected Content-Length."
        }
        $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $buffer = [byte[]]::new(65536)
        [long]$count = 0
        while (($read = $stream.ReadAsync($buffer, 0, $buffer.Length, $cancellation.Token).GetAwaiter().GetResult()) -gt 0) {
            $count += $read
            if ($count -gt $ExpectedSizeBytes -or $count -gt $MaximumBytes) { throw "$Description exceeded its pinned byte count." }
            $hash.AppendData($buffer, 0, $read)
        }
        $digest = [BitConverter]::ToString($hash.GetHashAndReset()).Replace('-', '').ToLowerInvariant()
        if ($count -ne $ExpectedSizeBytes -or $digest -cne $ExpectedSha256) {
            throw "$Description failed its byte-count or SHA-256 read-back check."
        }
        return [pscustomobject]@{
            status = [int]$response.StatusCode
            finalUrl = $finalUri.AbsoluteUri
            sizeBytes = $count
            sha256 = $digest
        }
    } finally {
        $hash.Dispose()
        $cancellation.Dispose()
        if ($stream) { $stream.Dispose() }
        if ($response) { $response.Dispose() }
        $request.Dispose()
        $client.Dispose()
        $handler.Dispose()
    }
}

function Assert-LocalArchive {
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Artifact
    )

    $archive = Get-Item -LiteralPath $ArchivePath -Force
    if ($archive.PSIsContainer -or ($archive.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $archive.Name -cne [string]$Artifact.fileName -or $archive.Length -ne [long]$Artifact.sizeBytes) {
        throw 'The local archive must be a regular file with the exact manifest name and byte count.'
    }
    $hash = (Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -cne [string]$Artifact.sha256) { throw 'The local archive SHA-256 does not match its distribution manifest.' }
    return $archive
}

function Write-ModelReleaseFile {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Content)

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $fullPath) { throw "Refusing to overwrite existing output '$fullPath'." }
    $parent = [System.IO.Path]::GetDirectoryName($fullPath)
    $null = [System.IO.Directory]::CreateDirectory($parent)
    $temporary = Join-Path $parent ('.' + [System.IO.Path]::GetFileName($fullPath) + '.' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [System.IO.File]::WriteAllText($temporary, $Content, [System.Text.UTF8Encoding]::new($false))
        [System.IO.File]::Move($temporary, $fullPath)
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { [System.IO.File]::Delete($temporary) }
    }
}

function Assert-OfficialModelCatalogEnvelope {
    param([Parameter(Mandatory = $true)][string]$Path)

    $file = Get-Item -LiteralPath $Path -Force
    if ($file.PSIsContainer -or ($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $file.Length -lt 1 -or $file.Length -gt 384KB) {
        throw 'The signed catalog envelope must be a regular file between 1 byte and 384 KiB.'
    }
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $text = [System.Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    $lines = $text.Split([char[]]@(10), [System.StringSplitOptions]::None)
    if ($lines.Length -eq 5 -and $lines[4] -ceq '') { $lines = $lines[0..3] }
    if ($lines.Length -ne 4) { throw 'The signed catalog envelope must contain exactly four lines.' }
    for ($index = 0; $index -lt $lines.Length; $index++) {
        if ($lines[$index].EndsWith("`r")) { $lines[$index] = $lines[$index].Substring(0, $lines[$index].Length - 1) }
        if ($lines[$index].Contains("`r")) { throw 'The signed catalog envelope contains invalid line endings.' }
    }
    if ($lines[0] -cne 'RIFELOVE MODEL CATALOG 1' -or
        $lines[1] -cne 'key-id=rifelove-model-catalog-2026-v1' -or
        -not $lines[2].StartsWith('payload=') -or -not $lines[3].StartsWith('signature=')) {
        throw 'The signed catalog envelope has an unsupported header or key ID.'
    }
    $payloadEncoded = $lines[2].Substring('payload='.Length)
    $signatureEncoded = $lines[3].Substring('signature='.Length)
    $payloadBytes = [Convert]::FromBase64String($payloadEncoded)
    $signature = [Convert]::FromBase64String($signatureEncoded)
    if ($payloadBytes.Length -lt 1 -or $payloadBytes.Length -gt 256KB -or $signature.Length -ne 64 -or
        [Convert]::ToBase64String($payloadBytes) -cne $payloadEncoded -or
        [Convert]::ToBase64String($signature) -cne $signatureEncoded) {
        throw 'The signed catalog envelope contains invalid or noncanonical base64 data.'
    }
    $payloadText = [System.Text.UTF8Encoding]::new($false, $true).GetString($payloadBytes)
    $catalog = ConvertFrom-Json -InputObject $payloadText -AsHashtable
    if ($catalog.schemaVersion -ne 1) { throw 'Only model catalog schema version 1 is supported.' }
    $models = @($catalog.models)
    $recommended = @($models | Where-Object { $_.recommended -is [bool] -and $_.recommended })
    if ($models.Count -lt 1 -or $models.Count -gt 32 -or $recommended.Count -ne 1) {
        throw 'The signed catalog must contain 1–32 models and exactly one recommended entry.'
    }
    $model = $recommended[0]
    if ($model.family -cne 'rife' -or [string]::IsNullOrWhiteSpace([string]$model.name) -or $model.name.Length -gt 128 -or
        [string]::IsNullOrWhiteSpace([string]$model.packageId) -or [string]::IsNullOrWhiteSpace([string]$model.id) -or
        [string]::IsNullOrWhiteSpace([string]$model.version)) {
        throw 'The signed catalog recommendation must identify a displayable RIFE model package.'
    }
    Assert-ModelSha256 -Value ([string]$model.modelSha256) -Description 'The recommended model SHA-256'
    if ([long]$model.manifest.sizeBytes -lt 1 -or [long]$model.manifest.sizeBytes -gt 1MB) {
        throw 'The signed catalog manifest pin has an invalid byte count.'
    }
    Assert-ModelSha256 -Value ([string]$model.manifest.sha256) -Description 'The recommended manifest SHA-256'
    $rawChannels = @($model.channels)
    if ($rawChannels.Count -lt 2 -or $rawChannels.Count -gt 5) {
        throw 'The recommended model must have two to five download channels.'
    }
    $firstArchive = Assert-ModelHttpsUrl -Url ([string]$rawChannels[0].archiveUrl)
    $archiveFileName = [Uri]::UnescapeDataString($firstArchive.AbsolutePath.Substring($firstArchive.AbsolutePath.LastIndexOf('/') + 1))
    if ($archiveFileName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}\.zip$') {
        throw 'The recommended model channels must use a safe ZIP archive file name.'
    }
    $artifact = [ordered]@{ fileName = $archiveFileName; sizeBytes = 1; sha256 = ('0' * 64) }
    $null = Normalize-ModelReleaseChannels -Channels $rawChannels -Artifact $artifact

    $rootHex = '10caa8c48b21b159d9d644507152f577ba8ebdeaa92cb822e684197445a7f6230ec1e21d1ef144e96c916db492e72eeb50df147f7900ee228773c3f677391985'
    [byte[]]$root = for ($index = 0; $index -lt $rootHex.Length; $index += 2) {
        [Convert]::ToByte($rootHex.Substring($index, 2), 16)
    }
    $parameters = [System.Security.Cryptography.ECParameters]::new()
    $parameters.Curve = [System.Security.Cryptography.ECCurve]::NamedCurves.nistP256
    $point = [System.Security.Cryptography.ECPoint]::new()
    $point.X = [byte[]]$root[0..31]
    $point.Y = [byte[]]$root[32..63]
    $parameters.Q = $point
    $publicKey = [System.Security.Cryptography.ECDsa]::Create()
    try {
        $publicKey.ImportParameters($parameters)
        if (-not $publicKey.VerifyData($payloadBytes, $signature,
                [System.Security.Cryptography.HashAlgorithmName]::SHA256,
                [System.Security.Cryptography.DSASignatureFormat]::IeeeP1363FixedFieldConcatenation)) {
            throw 'The signed model catalog does not match the client-compiled publisher trust root.'
        }
    } finally {
        $publicKey.Dispose()
    }
    return $catalog
}
