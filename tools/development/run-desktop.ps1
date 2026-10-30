[CmdletBinding()]
param([Parameter(Mandatory)][string]$QtRoot, [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildName = if ($Configuration -eq 'Debug') { 'windows-qt-dev' } else { 'windows-qt-release' }
$executable = Join-Path $projectRoot "build/$buildName/rifelove-desktop.exe"
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build the desktop with check-desktop.ps1 first.' }
$taskPath = $env:PATH
try {
    $env:PATH = "$(Join-Path $QtRoot 'bin');$(Join-Path $QtRoot 'Tools/mingw1510_64/mingw64/bin');$taskPath"
    # The desktop inherits this process-local SDK path. No system PATH changes.
    & $executable
} finally { $env:PATH = $taskPath }
