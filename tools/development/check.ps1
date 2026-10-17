[CmdletBinding()]
param(
    [ValidateSet('windows-dev', 'windows-release')]
    [string]$Preset = 'windows-dev',
    [switch]$AllowDependencyDownload,
    [string]$FFmpegRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$reportDirectory = Join-Path $projectRoot "build/$Preset"
$reportPath = Join-Path $reportDirectory 'local-check.json'
$testReportPath = Join-Path $reportDirectory 'test-report.xml'
$checkReport = [ordered]@{
    schemaVersion = 1
    preset = $Preset
    offline = -not $AllowDependencyDownload.IsPresent
    startedAtUtc = [DateTime]::UtcNow.ToString('o')
    finishedAtUtc = $null
    success = $false
    steps = @()
    error = $null
}

function Invoke-CheckStep {
    param([string]$Step, [string]$Executable, [string[]]$CommandArguments)
    & $Executable @CommandArguments
    $stepExitCode = $LASTEXITCODE
    $checkReport.steps += [ordered]@{ name = $Step; exitCode = $stepExitCode }
    if ($stepExitCode -ne 0) { throw "$Step failed (exit $stepExitCode)." }
}

Push-Location -LiteralPath $projectRoot
try {
    New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    $downloadSetting = if ($AllowDependencyDownload) { 'ON' } else { 'OFF' }
    $configureArguments = @('--preset', $Preset, "-DRIFELOVE_ALLOW_DEPENDENCY_DOWNLOADS=$downloadSetting")
    if ($FFmpegRoot) { $configureArguments += @('-DRIFELOVE_BUILD_MEDIA=ON', "-DRIFELOVE_FFMPEG_ROOT=$FFmpegRoot") }
    Invoke-CheckStep -Step 'configure' -Executable 'cmake' -CommandArguments $configureArguments
    Invoke-CheckStep -Step 'build' -Executable 'cmake' -CommandArguments @('--build', '--preset', $Preset)
    Invoke-CheckStep -Step 'test' -Executable 'ctest' -CommandArguments @('--preset', $Preset, '--output-junit', $testReportPath)
    $checkReport.success = $true
}
catch {
    $checkReport.error = $_.Exception.Message
    throw
}
finally {
    $checkReport.finishedAtUtc = [DateTime]::UtcNow.ToString('o')
    if (Test-Path -LiteralPath $reportDirectory) {
        $checkReport | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding utf8
        Write-Host "Local check report: $reportPath"
    }
    Pop-Location
}
