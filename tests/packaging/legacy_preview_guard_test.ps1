[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$probeRoot = Join-Path $repositoryRoot ('out/legacy-preview-guard-test-' + [guid]::NewGuid().ToString('N'))
$missingBuild = Join-Path $probeRoot 'missing-build'
$missingSource = Join-Path $probeRoot 'missing-source'
$checks = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}

function Get-Rejection([string]$ScriptPath, [hashtable]$Arguments, [bool]$Allow) {
    $failure = $null
    try {
        if ($Allow) { & $ScriptPath @Arguments -AllowLegacyPreview | Out-Null }
        else { & $ScriptPath @Arguments | Out-Null }
    } catch { $failure = $_ }
    return $failure
}

$entries = @(
    @{ name = 'package-local'; file = 'packaging/windows/package-local.ps1';
       arguments = @{ BuildDirectory = $missingBuild; OutputDirectory = (Join-Path $probeRoot 'local') };
       missing = 'missing-build' },
    @{ name = 'package-linked-preview'; file = 'packaging/windows/package-linked-preview.ps1';
       arguments = @{ BuildDirectory = $missingBuild; OutputDirectory = (Join-Path $probeRoot 'linked') };
       missing = 'missing-build' },
    @{ name = 'package-portable-local'; file = 'packaging/windows/package-portable-local.ps1';
       arguments = @{ SourcePackage = $missingSource; OutputDirectory = (Join-Path $probeRoot 'portable') };
       missing = 'missing-source' }
)

foreach ($entry in $entries) {
    $scriptPath = Join-Path $repositoryRoot $entry.file
    $blocked = Get-Rejection $scriptPath $entry.arguments $false
    Assert-True ($null -ne $blocked) "$($entry.name) rejects implicit legacy packaging"
    Assert-True ($blocked.Exception.Message.Contains('Legacy Qt/CLI preview packaging is disabled by default')) "$($entry.name) explains the native-only route"
    Assert-True (-not (Test-Path -LiteralPath $probeRoot)) "$($entry.name) creates no implicit output"

    $optedIn = Get-Rejection $scriptPath $entry.arguments $true
    Assert-True ($null -ne $optedIn) "$($entry.name) reaches its missing prerequisite"
    Assert-True ($optedIn.Exception.Message.Contains($entry.missing)) "$($entry.name) passes the opt-in gate"
    Assert-True (-not (Test-Path -LiteralPath $probeRoot)) "$($entry.name) creates no output with missing prerequisites"
}

Write-Host "Legacy preview opt-in guard: $checks checks passed."
