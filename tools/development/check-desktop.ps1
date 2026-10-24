[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$QtRoot,
    [Parameter(Mandatory)][string]$FFmpegRoot,
    [Parameter(Mandatory)][string]$OnnxRoot,
    [Parameter(Mandatory)][string]$JsonRoot,
    [string]$ModelPack = '',
    [string]$ReferencePython = '',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [string]$SourceCache = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (-not $SourceCache) { $SourceCache = Join-Path $projectRoot 'build/windows-dev/_deps' }
$compilerRoot = Join-Path $QtRoot 'Tools/mingw1510_64/mingw64'
$buildName = if ($Configuration -eq 'Debug') { 'build/windows-qt-dev' } else { 'build/windows-qt-release' }
$build = Join-Path $projectRoot $buildName
$report = [ordered]@{schemaVersion=1;configuration=$Configuration;offline=$true;success=$false;startedAtUtc=[DateTime]::UtcNow.ToString('o');finishedAtUtc=$null;steps=@();error=$null}
function Invoke-Step([string]$Name, [string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    $code = $LASTEXITCODE
    $report.steps += [ordered]@{name=$Name;exitCode=$code}
    if ($code -ne 0) { throw "$Name failed (exit $code)" }
}
$taskPath = $env:PATH
try {
    New-Item -ItemType Directory -Path $build -Force | Out-Null
    $env:PATH = "$(Join-Path $QtRoot 'bin');$(Join-Path $compilerRoot 'bin');$taskPath"
    $arguments = @('-S', $projectRoot, '-B', $build, '-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Configuration",
        "-DCMAKE_C_COMPILER=$(Join-Path $compilerRoot 'bin/gcc.exe')", "-DCMAKE_CXX_COMPILER=$(Join-Path $compilerRoot 'bin/g++.exe')",
        "-DCMAKE_PREFIX_PATH=$QtRoot", '-DRIFELOVE_ALLOW_DEPENDENCY_DOWNLOADS=OFF', '-DRIFELOVE_BUILD_DESKTOP=ON', '-DRIFELOVE_REQUIRE_DESKTOP=ON', '-DRIFELOVE_BUILD_TESTS=ON',
        '-DRIFELOVE_BUILD_MEDIA=ON', '-DRIFELOVE_BUILD_ONNX_CUDA=ON', "-DRIFELOVE_FFMPEG_ROOT=$FFmpegRoot", "-DRIFELOVE_ONNX_ROOT=$OnnxRoot", "-DRIFELOVE_JSON_ROOT=$JsonRoot",
        "-DFETCHCONTENT_SOURCE_DIR_SQLITE3=$(Join-Path $SourceCache 'sqlite3-src')", "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$(Join-Path $SourceCache 'googletest-src')",
        "-DRIFELOVE_TEST_RIFE_PACK=$ModelPack", "-DRIFELOVE_MODEL_TOOLS_PYTHON=$ReferencePython")
    Invoke-Step 'configure' 'cmake' $arguments
    # Keep compiler memory bounded on ordinary development machines.
    Invoke-Step 'build' 'cmake' @('--build', $build, '--parallel', '4')
    Invoke-Step 'test' 'ctest' @('--test-dir', $build, '--output-on-failure', '--output-junit', (Join-Path $build 'test-report.xml'))
    $report.success = $true
} catch { $report.error = $_.Exception.Message; throw }
finally {
    $env:PATH = $taskPath
    $report.finishedAtUtc = [DateTime]::UtcNow.ToString('o')
    if (Test-Path -LiteralPath $build) { $report | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $build 'local-check.json') -Encoding utf8 }
}
