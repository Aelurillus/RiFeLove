[CmdletBinding()]
param([string]$QtRoot = '', [switch]$AllowDownload, [switch]$VerifyOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'qt-sdk.lock.json') -Raw | ConvertFrom-Json
if (-not $QtRoot) { $QtRoot = Join-Path $projectRoot 'work/dependencies/qt-6.12.0' }
$QtRoot = [IO.Path]::GetFullPath($QtRoot)
$cache = Join-Path $projectRoot 'work/downloads/qt-6.12.0'
New-Item -ItemType Directory -Path $cache -Force | Out-Null
$assets = @($lock.qtAssets) + @($lock.compiler)
foreach ($asset in $assets) {
    $archive = Join-Path $cache $asset.fileName
    if (-not (Test-Path -LiteralPath $archive)) {
        if (-not $AllowDownload -or $VerifyOnly) { throw "Missing cached SDK archive: $archive. Explicit -AllowDownload is required for acquisition." }
        Invoke-WebRequest -Uri $asset.url -OutFile $archive
    }
    if ((Get-Item -LiteralPath $archive).Length -ne $asset.bytes -or
        (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.sha256) {
        throw "SDK checksum/size mismatch: $($asset.fileName)"
    }
}
$compilerRoot = Join-Path $QtRoot 'Tools/mingw1510_64/mingw64'
if (-not $VerifyOnly) {
    $extractor = (Get-Command 7z -ErrorAction Stop).Source
    New-Item -ItemType Directory -Path $QtRoot -Force | Out-Null
    foreach ($asset in $lock.qtAssets) {
        & $extractor x -y "-o$QtRoot" (Join-Path $cache $asset.fileName) *> (Join-Path $cache 'extract.log')
        if ($LASTEXITCODE -ne 0) { throw "Extraction failed: $($asset.fileName)" }
    }
    & $extractor x -y "-o$(Join-Path $QtRoot 'Tools/mingw1510_64')" (Join-Path $cache $lock.compiler.fileName) *> (Join-Path $cache 'extract.log')
    if ($LASTEXITCODE -ne 0) { throw 'Compiler extraction failed' }
    # Qt's repository dependency metadata is stale. Match its actual 15.1
    # MSVCRT build and colocate only that compiler's runtime beside Qt tools.
    foreach ($name in @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $compilerRoot "bin/$name") -Destination (Join-Path $QtRoot "bin/$name") -Force
    }
}
$taskPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $QtRoot 'bin');$(Join-Path $compilerRoot 'bin');$taskPath"
    $version = & (Join-Path $QtRoot 'bin/qmake.exe') -query QT_VERSION
    if ($LASTEXITCODE -ne 0 -or $version -ne $lock.qtVersion) { throw 'Qt version check failed' }
    $compilerVersion = & (Join-Path $compilerRoot 'bin/g++.exe') -dumpfullversion
    if ($LASTEXITCODE -ne 0 -or $compilerVersion -ne $lock.compilerVersion) { throw 'Compiler version check failed' }
} finally { $env:PATH = $taskPath }
[ordered]@{qtRoot=$QtRoot;compilerRoot=$compilerRoot;qtVersion=$version;compilerVersion=$compilerVersion;offline=($VerifyOnly -or -not $AllowDownload);redistributionAudit='pending'} | ConvertTo-Json
