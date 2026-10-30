[CmdletBinding()]
param(
    [string]$BuildRoot = "",
    [string]$SourceArchive = "",
    [string]$CodecPrefix = "",
    [string]$NvCodecHeadersPrefix = "",
    [string]$NasmDirectory = "",
    [string]$Msys2Directory = "",
    [string]$OutputRoot = "",
    [int]$Jobs = 0,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "../.."))
$lockPath = Join-Path $repo "tools/development/ffmpeg-source-sdk.lock.json"
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json

if ([string]::IsNullOrWhiteSpace($BuildRoot)) { $BuildRoot = Join-Path $repo "work/codec-ffmpeg-build-x86asm-repro" }
if ([string]::IsNullOrWhiteSpace($SourceArchive)) { $SourceArchive = Join-Path $repo "work/downloads/ffmpeg-8.1.3.tar.xz" }
if ([string]::IsNullOrWhiteSpace($CodecPrefix)) { $CodecPrefix = Join-Path $repo "work/codec-deps/installed/x64-mingw-dynamic" }
if ([string]::IsNullOrWhiteSpace($NvCodecHeadersPrefix)) { $NvCodecHeadersPrefix = Join-Path $repo "work/nvenc-build/ffnvcodec-prefix" }
if ([string]::IsNullOrWhiteSpace($NasmDirectory)) { $NasmDirectory = Join-Path $repo "work/nasm-3.02-win64/nasm-3.02" }
if ([string]::IsNullOrWhiteSpace($Msys2Directory)) { $Msys2Directory = Join-Path $repo "work/msys64" }
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $BuildRoot "ffmpeg-install" }
if ($Jobs -le 0) { $Jobs = [Math]::Max(1, [Environment]::ProcessorCount - 1) }

function Full([string]$Path) { return [IO.Path]::GetFullPath($Path) }
function Require-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label 不存在：$Path" }
    return (Full $Path)
}
function Require-Directory([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw "$Label 不存在：$Path" }
    return (Full $Path)
}
function Assert-Sha256([string]$Path, [string]$Expected, [string]$Label) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($actual -ne $Expected.ToUpperInvariant()) { throw "$Label SHA-256 不匹配：$Path`n期望：$Expected`n实际：$actual" }
}
function To-MsysPath([string]$Path) {
    $full = (Full $Path).Replace('\','/')
    if ($full -match '^([A-Za-z]):/(.*)$') { return "/$(([string]$matches[1]).ToLowerInvariant())/$($matches[2])" }
    return $full
}
function Find-7Zip {
    foreach ($name in @("7z.exe", "7zz.exe")) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    throw "需要 7-Zip 以解压 FFmpeg tar.xz；请将 7z.exe 放入临时 PATH。"
}
function Expand-FfmpegSource([string]$Archive, [string]$Destination, [string]$SevenZip) {
    $stage = Join-Path $Destination "extract-stage"
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    & $SevenZip x -y $Archive "-o$stage" | Out-Host
    $tar = Get-ChildItem -LiteralPath $stage -Filter "*.tar" -File | Select-Object -First 1
    if (-not $tar) { throw "FFmpeg xz 解压后没有找到 tar：$stage" }
    & $SevenZip x -y $tar.FullName "-o$stage" | Out-Host
    $source = Join-Path $stage "ffmpeg-8.1.3"
    if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "FFmpeg 源码目录缺失：$source" }
    $target = Join-Path $Destination "ffmpeg-8.1.3"
    if (Test-Path -LiteralPath $target) { throw "源码目标已存在，避免覆盖：$target" }
    Move-Item -LiteralPath $source -Destination $target
    return $target
}
function Apply-Patch([string]$Source, [string]$PatchPath) {
    $check = & git -C $repo apply --check --unsafe-paths --directory=$Source $PatchPath 2>&1
    if ($LASTEXITCODE -eq 0) {
        & git -C $repo apply --unsafe-paths --directory=$Source $PatchPath
        return "applied"
    }
    $reverse = & git -C $repo apply --reverse --check --unsafe-paths --directory=$Source $PatchPath 2>&1
    if ($LASTEXITCODE -eq 0) { return "already-applied" }
    throw "无法应用 FFmpeg 补丁：$PatchPath`n$($check -join [Environment]::NewLine)"
}

$sevenZip = Find-7Zip
$sourceArchive = Require-File $SourceArchive "FFmpeg 源码归档"
$codecPrefix = Require-Directory $CodecPrefix "编解码依赖前缀"
$headersPrefix = Require-Directory $NvCodecHeadersPrefix "nv-codec-headers 前缀"
$nasmDirectory = Require-Directory $NasmDirectory "NASM 目录"
$msysRoot = Require-Directory $Msys2Directory "MSYS2 目录"
$bash = Require-File (Join-Path $msysRoot "usr/bin/bash.exe") "MSYS2 bash"
$nasm = Require-File (Join-Path $nasmDirectory "nasm.exe") "NASM"
$pkgConfigShim = Require-File (Join-Path $repo "work/codec-ffmpeg-build/pkg-config-codecs.sh") "pkg-config shim"
Require-File (Join-Path $headersPrefix "include/ffnvcodec/nvEncodeAPI.h") "NVENC headers" | Out-Null

Assert-Sha256 $sourceArchive $lock.sources.ffmpeg.sha256 "FFmpeg 源码"
$lockedArchives = @(
    [pscustomobject]@{ Path = (Join-Path $repo "work/downloads/nv-codec-headers-n12.2.72.0.tar.gz"); Hash = $lock.sources.nvCodecHeaders.sha256; Label = "nv-codec-headers" },
    [pscustomobject]@{ Path = (Join-Path $repo "work/codec-deps/downloads/aom-af3dc9aadc793c1b00edc003e3fc19a44fdeb574.tar.gz"); Hash = $lock.sources.libaom.sha256; Label = "libaom" },
    [pscustomobject]@{ Path = (Join-Path $repo "work/codec-deps/downloads/cisco-openh264-v2.7.0.tar.gz"); Hash = $lock.sources.openH264.sha256; Label = "OpenH264" },
    [pscustomobject]@{ Path = (Join-Path $repo "work/codec-deps/downloads/ultravideo-kvazaar-v2.3.2.tar.gz"); Hash = $lock.sources.kvazaar.sha256; Label = "Kvazaar" }
)
foreach ($archive in $lockedArchives) {
    Assert-Sha256 (Require-File $archive.Path "$($archive.Label) 源码归档") $archive.Hash "$($archive.Label) 源码"
}
$nasmArchive = Join-Path $repo "work/downloads/nasm-3.02-win64.zip"
if (Test-Path -LiteralPath $nasmArchive -PathType Leaf) { Assert-Sha256 $nasmArchive $lock.target.nasm.sha256 "NASM 归档" }
$msysArchive = Join-Path $repo "work/downloads/msys2-base-x86_64-20260927.sfx.exe"
if (Test-Path -LiteralPath $msysArchive -PathType Leaf) { Assert-Sha256 $msysArchive $lock.target.msys2.sha256 "MSYS2 归档" }

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
$BuildRoot = Full $BuildRoot
$sourceRoot = Join-Path $BuildRoot "ffmpeg-8.1.3"
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) { $sourceRoot = Expand-FfmpegSource $sourceArchive $BuildRoot $sevenZip }

foreach ($patchName in $lock.configure.patches) {
    $patchPath = Require-File (Join-Path $repo $patchName) "FFmpeg 补丁"
    $status = Apply-Patch $sourceRoot $patchPath
    Write-Host "${patchName}: $status"
}

if ($SkipBuild) { Write-Host "源码和补丁已准备：$sourceRoot"; exit 0 }

$out = Full $OutputRoot
New-Item -ItemType Directory -Path $out -Force | Out-Null
$sourceMsys = To-MsysPath $sourceRoot
$outMsys = To-MsysPath $out
$pkgMsys = To-MsysPath $pkgConfigShim
$nasmMsys = To-MsysPath $nasmDirectory
$headersMsys = To-MsysPath $headersPrefix
$mingwMsys = To-MsysPath (Join-Path $env:USERPROFILE 'scoop-toolchain/apps/mingw/current/bin')

$bashScript = Join-Path $BuildRoot "build-x86asm-repro.sh"
$scriptLines = @(
    "set -e",
    "export MSYSTEM=UCRT64",
    "export MINGW_PREFIX=/ucrt64",
    "export PATH=/usr/bin:/bin/${mingwMsys}:${nasmMsys}:`$PATH",
    "cd '$sourceMsys'",
    "./configure --prefix='$outMsys' --enable-shared --disable-static --disable-doc --disable-debug --enable-x86asm --enable-nvenc --enable-libopenh264 --enable-libkvazaar --enable-libaom --pkg-config='$pkgMsys' --extra-cflags=-I$headersMsys/include",
    "make -j$Jobs",
    "make install"
)
$scriptLines | Set-Content -LiteralPath $bashScript -Encoding ascii

$oldPath = $env:PATH
try {
    $env:MSYSTEM = "UCRT64"
    $env:MINGW_PREFIX = "/ucrt64"
    & $bash --noprofile --norc (To-MsysPath $bashScript)
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg x86asm 构建失败，退出码：$LASTEXITCODE" }
} finally {
    $env:PATH = $oldPath
}

Write-Host "FFmpeg x86asm SDK 已安装到：$out"
