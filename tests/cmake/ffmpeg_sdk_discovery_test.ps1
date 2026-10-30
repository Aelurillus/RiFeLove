[CmdletBinding()]
param([string]$OutputDirectory = '', [string]$BaselineSdkRoot = '', [string]$CandidateSdkRoot = '')
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repositoryRoot 'packaging/windows/ffmpeg-sdk-helpers.ps1')
$project = Join-Path $PSScriptRoot 'ffmpeg_sdk_discovery'
$outputRoot = Join-Path $repositoryRoot 'out'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $outputRoot ('ffmpeg-sdk-discovery-test-' + [guid]::NewGuid().ToString('N')) }
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $root.StartsWith($outputRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture root must be under repository out.' }
if (Test-Path -LiteralPath $root) { throw "Refusing to overwrite test fixtures: $root" }
New-Item -ItemType Directory -Path $root | Out-Null
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$components = @('avformat','avcodec','avutil','swscale')
$script:cases = @()
function Write-Fixture([string]$Path,[string]$Contents) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path,$Contents,[Text.UTF8Encoding]::new($false))
}
function New-Sdk([string]$Path,[string]$Identity,[string]$Missing = '') {
    Write-Fixture (Join-Path $Path 'include/libavformat/avformat.h') "/* test SDK $Identity */"
    foreach ($component in $components) {
        if ($component -cne $Missing) { Write-Fixture (Join-Path $Path "lib/lib$component.dll.a") "test library $Identity $component" }
    }
    Write-Fixture (Join-Path $Path 'bin/swscale-9.dll') "same DLL basename, different bytes: $Identity"
}
function Invoke-Configure([string]$Name,[string]$Build,[string]$Sdk,[string[]]$Extra = @(),[string]$ExpectedFailure = '') {
    $arguments = @('-S',$project,'-B',$Build,'-G','Ninja',"-DRIFELOVE_MODULE_ROOT=$repositoryRoot/cmake","-DRIFELOVE_FFMPEG_ROOT=$Sdk") + $Extra
    $output = (& $cmake @arguments 2>&1 | Out-String)
    $code = $LASTEXITCODE
    Write-Fixture (Join-Path $root "$Name.log") $output
    if ($ExpectedFailure) {
        if ($code -eq 0 -or -not $output.Contains($ExpectedFailure)) { throw "Expected $Name to fail with '$ExpectedFailure': $output" }
    } elseif ($code -ne 0) { throw "CMake case $Name failed: $output" }
    $script:cases += [ordered]@{name=$Name;passed=$true;expectedFailure=[bool]$ExpectedFailure}
}
function Assert-SelectedSdk([string]$Build,[string]$Sdk) {
    $identity = Assert-FfmpegBuildLinkIdentity -BuildDirectory $Build -SdkRoot $Sdk
    $prefix = [IO.Path]::GetFullPath($Sdk).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $Build 'resolved-targets.txt'))) {
        if (-not ([IO.Path]::GetFullPath($line)).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw "Imported target retained another SDK: $line" }
    }
    return $identity
}
function Assert-Rejected([scriptblock]$Action,[string]$Fragment) {
    $rejected=$false
    try { & $Action | Out-Null } catch { $rejected=$_.Exception.Message.Contains($Fragment) }
    if(-not $rejected){throw "Expected rejection containing: $Fragment"}
}

$sdkA = Join-Path $root 'sdk-a'
$sdkB = Join-Path $root 'sdk-b'
$sdkC = Join-Path $root 'sdk-c'
New-Sdk $sdkA 'A'
New-Sdk $sdkB 'B'
New-Sdk $sdkC 'C'
$build = Join-Path $root 'switch-build'
Invoke-Configure 'switch-a' $build $sdkA
$a = Assert-SelectedSdk $build $sdkA
Invoke-Configure 'switch-b' $build $sdkB
$b = Assert-SelectedSdk $build $sdkB
if ($a.headerSha256 -ceq $b.headerSha256 -or $a.libraries.avcodec.sha256 -ceq $b.libraries.avcodec.sha256) { throw 'Fixture SDKs must differ despite identical basenames.' }
Invoke-Configure 'switch-back-a' $build $sdkA
$null = Assert-SelectedSdk $build $sdkA
Invoke-Configure 'unchanged-root-foreign-library' $build $sdkA @("-DFFmpeg_avcodec_LIBRARY=$sdkB/lib/libavcodec.dll.a") 'Mixed FFmpeg SDK'
Invoke-Configure 'fresh-foreign-include' (Join-Path $root 'bad-include-build') $sdkB @("-DFFmpeg_INCLUDE_DIR=$sdkA/include") 'Mixed FFmpeg SDK'
Invoke-Configure 'fresh-foreign-library' (Join-Path $root 'bad-library-build') $sdkB @("-DFFmpeg_avcodec_LIBRARY=$sdkA/lib/libavcodec.dll.a") 'Mixed FFmpeg SDK'
$injectedBuild = Join-Path $root 'switch-injected-build'
Invoke-Configure 'injected-initial-a' $injectedBuild $sdkA
Invoke-Configure 'root-switch-with-foreign-override' $injectedBuild $sdkB @("-DFFmpeg_avcodec_LIBRARY=$sdkC/lib/libavcodec.dll.a") 'Mixed FFmpeg SDK'
$partial = Join-Path $root 'partial-sdk'
New-Sdk $partial 'partial' 'avutil'
Invoke-Configure 'no-system-sdk-fallback' (Join-Path $root 'partial-build') $partial @("-DCMAKE_PREFIX_PATH=$sdkA") 'Could NOT find FFmpeg'
Invoke-Configure 'explicit-root-required' (Join-Path $root 'missing-root-build') '' @("-DCMAKE_PREFIX_PATH=$sdkA") 'RIFELOVE_FFMPEG_ROOT must name'
$twice = Join-Path $root 'same-configure-twice-build'
Invoke-Configure 'same-configure-root-switch' $twice $sdkA @("-DRIFELOVE_SECOND_SDK=$sdkB")
$null = Assert-SelectedSdk $twice $sdkB

foreach ($badKey in @('FFmpeg_INCLUDE_DIR') + @($components | ForEach-Object { "FFmpeg_${_}_LIBRARY" })) {
    $entries = @("RIFELOVE_FFMPEG_ROOT:PATH=$sdkB","RIFELOVE_FFMPEG_TOOL:FILEPATH=$sdkB/bin/ffmpeg.exe","RIFELOVE_FFPROBE_TOOL:FILEPATH=$sdkB/bin/ffprobe.exe")
    $entries += 'FFmpeg_INCLUDE_DIR:PATH=' + $(if($badKey -ceq 'FFmpeg_INCLUDE_DIR'){"$sdkA/include"}else{"$sdkB/include"})
    foreach ($component in $components) {
        $key = "FFmpeg_${component}_LIBRARY"
        $entries += "${key}:FILEPATH=" + $(if($badKey -ceq $key){"$sdkA/lib/lib$component.dll.a"}else{"$sdkB/lib/lib$component.dll.a"})
    }
    $badBuild = Join-Path $root "package-preflight-$badKey"
    Write-Fixture (Join-Path $badBuild 'CMakeCache.txt') ($entries -join "`n")
    Assert-Rejected { Assert-FfmpegBuildLinkIdentity $badBuild $sdkB } 'Mixed FFmpeg SDK'
    $script:cases += [ordered]@{name="package-preflight-$badKey";passed=$true;expectedFailure=$true}
}
$null = Assert-FfmpegRuntimeIdentity $sdkB (Join-Path $sdkB 'bin') @('swscale-9.dll')
Assert-Rejected { Assert-FfmpegRuntimeIdentity $sdkB (Join-Path $sdkA 'bin') @('swscale-9.dll') } 'differs from the selected SDK'
$script:cases += [ordered]@{name='same-dll-basename-different-bytes';passed=$true;expectedFailure=$true}

if ($BaselineSdkRoot -or $CandidateSdkRoot) {
    if (-not $BaselineSdkRoot -or -not $CandidateSdkRoot) { throw 'Specify both actual SDK roots for local SDK-switch validation.' }
    $actual = Join-Path $root 'actual-sdk-switch-build'
    Invoke-Configure 'actual-baseline' $actual ([IO.Path]::GetFullPath($BaselineSdkRoot))
    $null = Assert-SelectedSdk $actual $BaselineSdkRoot
    Invoke-Configure 'actual-x86asm' $actual ([IO.Path]::GetFullPath($CandidateSdkRoot))
    $null = Assert-SelectedSdk $actual $CandidateSdkRoot
    Invoke-Configure 'actual-back-to-baseline' $actual ([IO.Path]::GetFullPath($BaselineSdkRoot))
    $null = Assert-SelectedSdk $actual $BaselineSdkRoot
}
[ordered]@{result='passed';fixtures=$root;cases=$script:cases;caseCount=$script:cases.Count;launchedProductExe=$false;cleanupPerformed=$false} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'test-result.json') -Encoding utf8NoBOM
Write-Output "FFmpeg SDK discovery checks passed ($($script:cases.Count) cases). Fixtures retained at $root"
