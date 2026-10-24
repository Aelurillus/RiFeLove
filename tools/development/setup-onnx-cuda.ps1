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
$tensorRtArchive = Get-PinnedAsset $lock.tensorRt.fileName $lock.tensorRt.url $lock.tensorRt.sha256
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($tensorRtArchive)
try {
    $libraries = @($archive.Entries | Where-Object { $_.FullName -match '^tensorrt_libs/[^/]+\.dll$' })
    if (-not $libraries.Count) { throw 'TensorRT wheel has no native runtime libraries.' }
    $libraryRoot = Join-Path $runtimeRoot 'lib'
    New-Item -ItemType Directory -Path $libraryRoot -Force | Out-Null
    foreach ($entry in $libraries) {
        $target = Join-Path $libraryRoot $entry.Name
        if (Test-Path -LiteralPath $target) {
            $existingHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            $stream = $entry.Open()
            $hash = [System.Security.Cryptography.SHA256]::Create()
            try { $entryHash = [Convert]::ToHexString($hash.ComputeHash($stream)) } finally { $stream.Dispose(); $hash.Dispose() }
            if ($existingHash -ne $entryHash) { throw "Refusing to overwrite a different TensorRT DLL: $target" }
            continue
        }
        $inputStream = $entry.Open()
        $outputStream = [System.IO.File]::Open($target,[System.IO.FileMode]::CreateNew,[System.IO.FileAccess]::Write,[System.IO.FileShare]::None)
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
    }
    $licenseEntry = $archive.GetEntry('tensorrt_cu12_libs-10.9.0.34.dist-info/LICENSE.txt')
    if (-not $licenseEntry) { throw 'TensorRT wheel did not contain its license agreement.' }
    $licenseRoot = Join-Path $DependencyRoot 'licenses'
    New-Item -ItemType Directory -Path $licenseRoot -Force | Out-Null
    $licenseTarget = Join-Path $licenseRoot 'TensorRT-10.9-LICENSE.txt'
    if (Test-Path -LiteralPath $licenseTarget) {
        $licenseStream = $licenseEntry.Open()
        $hash = [System.Security.Cryptography.SHA256]::Create()
        try { $licenseHash = [Convert]::ToHexString($hash.ComputeHash($licenseStream)) } finally { $licenseStream.Dispose(); $hash.Dispose() }
        if ((Get-FileHash -LiteralPath $licenseTarget -Algorithm SHA256).Hash -ne $licenseHash) {
            throw "Refusing to replace a different TensorRT license file: $licenseTarget"
        }
    } else {
        $licenseStream = $licenseEntry.Open()
        $licenseOutput = [System.IO.File]::Open($licenseTarget,[System.IO.FileMode]::CreateNew,[System.IO.FileAccess]::Write,[System.IO.FileShare]::None)
        try { $licenseStream.CopyTo($licenseOutput) } finally { $licenseOutput.Dispose(); $licenseStream.Dispose() }
    }
    $versionManifest = Join-Path $libraryRoot 'tensorrt-runtime.version'
    $manifestContent = "$($lock.tensorRt.version)`n$($lock.tensorRt.sha256)"
    if (Test-Path -LiteralPath $versionManifest) {
        if ((Get-Content -LiteralPath $versionManifest -Raw -Encoding ASCII).TrimEnd() -ne $manifestContent) {
            throw "Refusing to replace a different TensorRT runtime identity: $versionManifest"
        }
    } else { Set-Content -LiteralPath $versionManifest -Value $manifestContent -Encoding ASCII }
} finally { $archive.Dispose() }
[pscustomobject]@{onnxRoot=$runtimeRoot;jsonRoot=$jsonRoot;tensorRtVersion=$lock.tensorRt.version;nativeRuntimeOnly=$true;redistributionAudit='pending'} | ConvertTo-Json
