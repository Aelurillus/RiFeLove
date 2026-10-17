[CmdletBinding()]
param(
    [string]$Destination = '',
    [switch]$AllowDownload
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$sdkLock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ffmpeg-sdk.lock.json') -Raw | ConvertFrom-Json
$archiveDirectory = Join-Path $projectRoot 'work/downloads'
$archive = Join-Path $archiveDirectory 'ffmpeg-8.1.3-lgpl-shared.zip'
if (-not $Destination) { $Destination = Join-Path $projectRoot 'work/dependencies/ffmpeg-8.1.3' }
if (-not (Test-Path -LiteralPath $archive)) {
    if (-not $AllowDownload) { throw 'The pinned FFmpeg archive is not cached. Supply it locally or explicitly use -AllowDownload.' }
    New-Item -ItemType Directory -Path $archiveDirectory -Force | Out-Null
    Invoke-WebRequest -Uri $sdkLock.url -OutFile $archive
}
$actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $sdkLock.sha256) { throw 'FFmpeg archive checksum does not match the pinned upstream release.' }
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Expand-Archive -LiteralPath $archive -DestinationPath $Destination -Force
$sdkRoot = Join-Path $Destination $sdkLock.folderName
if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot 'include/libavformat/avformat.h'))) { throw 'SDK extraction is incomplete.' }
Write-Output "Verified local FFmpeg SDK: $sdkRoot"
Write-Output "Build: .\tools\development\check.ps1 -FFmpegRoot '$sdkRoot'"
