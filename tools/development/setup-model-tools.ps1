[CmdletBinding()]
param([switch]$AllowDownload, [string]$PythonRoot = '')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$toolchain = Get-Content -LiteralPath (Join-Path $projectRoot 'tools/model_export/toolchain.lock.json') -Raw | ConvertFrom-Json
$workDirectory = Join-Path $projectRoot 'work'
$archive = Join-Path $workDirectory "downloads/uv-$($toolchain.uvVersion).zip"
$uvDirectory = Join-Path $workDirectory 'tools/uv'
$uvExecutable = Join-Path $uvDirectory "uv-$($toolchain.uvVersion).data/scripts/uv.exe"
$environmentDirectory = Join-Path $workDirectory 'model-tools'
$environmentPython = Join-Path $environmentDirectory 'Scripts/python.exe'
if (-not $PythonRoot) { $PythonRoot = Join-Path $workDirectory 'dependencies/python' }
if (-not (Test-Path -LiteralPath $archive)) {
    if (-not $AllowDownload) { throw 'The pinned uv wheel is not cached. Supply it locally or use -AllowDownload for setup.' }
    New-Item -ItemType Directory -Path (Split-Path $archive) -Force | Out-Null
    Invoke-WebRequest -Uri $toolchain.uvWheelUrl -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $toolchain.uvWheelSha256) { throw 'uv wheel checksum mismatch.' }
Expand-Archive -LiteralPath $archive -DestinationPath $uvDirectory -Force
$previousCache = $env:UV_CACHE_DIR
$env:UV_CACHE_DIR = Join-Path $workDirectory 'uv-cache'
try {
    if (-not (Test-Path -LiteralPath $environmentPython)) {
        $python = Join-Path $PythonRoot "cpython-$($toolchain.pythonVersion)-windows-x86_64-none/python.exe"
        if (-not (Test-Path -LiteralPath $python)) {
            if (-not $AllowDownload) { throw 'The pinned Python interpreter is not installed locally. Supply it or use -AllowDownload.' }
            & $uvExecutable python install $toolchain.pythonVersion --install-dir $PythonRoot --no-bin --no-registry --system-certs
            if ($LASTEXITCODE -ne 0) { throw 'Managed Python setup failed.' }
        }
        & $uvExecutable venv --python $python --no-python-downloads $environmentDirectory
        if ($LASTEXITCODE -ne 0) { throw 'Model virtual environment creation failed.' }
    }
    & $environmentPython -c 'import sys; assert sys.version_info[:2] == (3, 11), "RIFE export requires Python 3.11"'
    if ($LASTEXITCODE -ne 0) { throw 'Unexpected model environment interpreter.' }
    $arguments = @('pip', 'install', '--python', $environmentPython, '--torch-backend', 'cpu', '--system-certs', '-r', (Join-Path $projectRoot 'tools/model_export/requirements.cpu.txt'))
    if (-not $AllowDownload) { $arguments += '--offline' }
    & $uvExecutable @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Model package setup failed; normal setup uses only the local cache.' }
    & $uvExecutable pip freeze --python $environmentPython | Set-Content -LiteralPath (Join-Path $environmentDirectory 'requirements.resolved.txt') -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw 'Could not record resolved model-tool package versions.' }
    Write-Output "Model development interpreter: $environmentPython"
}
finally { $env:UV_CACHE_DIR = $previousCache }
