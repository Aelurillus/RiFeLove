[CmdletBinding()]
param([switch]$AllowDownload, [string]$DependencyRoot = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'onnx-cuda-sdk.lock.json') -Raw | ConvertFrom-Json
if (-not $DependencyRoot) { $DependencyRoot = Join-Path $projectRoot 'work/dependencies' }
$cache = Join-Path $projectRoot 'work/downloads'
New-Item -ItemType Directory -Path $cache,$DependencyRoot -Force | Out-Null
function Get-PinnedAsset($name, $url, $expected) {
    $path = Join-Path $cache $name
    if (-not (Test-Path -LiteralPath $path)) {
        if (-not $AllowDownload) { throw "Missing cached dependency: $name. Supply it or explicitly use -AllowDownload." }
        Invoke-WebRequest -Uri $url -OutFile $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw "SHA-256 mismatch: $name" }
    return $path
}
$runtimeArchive = Get-PinnedAsset $lock.runtime.fileName $lock.runtime.url $lock.runtime.sha256
Expand-Archive -LiteralPath $runtimeArchive -DestinationPath $DependencyRoot -Force
$runtimeRoot = Join-Path $DependencyRoot $lock.runtime.folderName
if ((Get-FileHash -LiteralPath (Join-Path $runtimeRoot 'include/onnxruntime_c_api.h') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $lock.runtime.cApiHeaderSha256) { throw 'ONNX Runtime C header checksum mismatch.' }
$jsonHeader = Get-PinnedAsset 'nlohmann-json-3.12.0.hpp' $lock.json.url $lock.json.sha256
$jsonRoot = Join-Path $DependencyRoot 'nlohmann-json-3.12.0'
New-Item -ItemType Directory -Path (Join-Path $jsonRoot 'include/nlohmann') -Force | Out-Null
Copy-Item -LiteralPath $jsonHeader -Destination (Join-Path $jsonRoot 'include/nlohmann/json.hpp') -Force
$jsonLicense = Get-PinnedAsset 'nlohmann-json-3.12.0-LICENSE.MIT' $lock.json.licenseUrl $lock.json.licenseSha256
Copy-Item -LiteralPath $jsonLicense -Destination (Join-Path $jsonRoot 'LICENSE.MIT') -Force
$nvidiaRoot = Join-Path $DependencyRoot 'onnx-cuda-12.8'
foreach ($package in $lock.nvidia) {
    $archive = Get-PinnedAsset $package.fileName $package.url $package.sha256
    $expanded = Join-Path $nvidiaRoot $package.name
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded -Force
    # Only native runtime DLLs are needed. No Python package is installed into
    # the application, and no PATH, driver or system runtime is changed.
    $libraries = @(Get-ChildItem -LiteralPath $expanded -Recurse -Filter '*.dll' -File)
    if (-not $libraries.Count) { throw "No runtime libraries in $($package.name)" }
    foreach ($library in $libraries) {
        $target = Join-Path (Join-Path $runtimeRoot 'lib') $library.Name
        if (Test-Path -LiteralPath $target) {
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $library.FullName -Algorithm SHA256).Hash) { throw "Refusing to overwrite a different runtime DLL: $target" }
        } else { Copy-Item -LiteralPath $library.FullName -Destination $target }
    }
}
[pscustomobject]@{onnxRoot=$runtimeRoot;jsonRoot=$jsonRoot;nativeRuntimeOnly=$true;redistributionAudit='pending'} | ConvertTo-Json
