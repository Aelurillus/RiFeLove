[CmdletBinding()]
param([string]$OutputDirectory = '', [string]$RealBuildDirectory = '', [switch]$ValidateDefaultReleasePreset,
      [string]$DependencySourceDirectory = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../../packaging/windows/native-build-dependencies.ps1')
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outputRoot = Join-Path $repositoryRoot 'out'
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $outputRoot ('native-build-dependencies-test-' + [guid]::NewGuid().ToString('N')) }
$root = [IO.Path]::GetFullPath($OutputDirectory)
if (-not $root.StartsWith($outputRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture root must be under repository out.' }
if (Test-Path -LiteralPath $root) { throw "Refusing to overwrite test fixtures: $root" }
if ((Get-Item -LiteralPath $outputRoot -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Fixture output root must not be a link.' }
New-Item -ItemType Directory -Path $root | Out-Null
function Write-Fixture([string]$Path,[string]$Contents) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path,$Contents,[Text.UTF8Encoding]::new($false))
}
function Assert-Equal($Expected,$Actual,[string]$Message) {
    if ($Expected -cne $Actual) { throw "${Message}: expected '$Expected', found '$Actual'" }
}
function Assert-Rejected([scriptblock]$Action,[string]$Fragment) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $_.Exception.Message.Contains($Fragment) }
    if (-not $rejected) { throw "Expected rejection containing: $Fragment" }
}
function Write-CompilerBuildFixture {
    param([string]$Build,[string]$CachedCompiler,[string]$RecordedCompiler=$CachedCompiler,
          [hashtable]$RecordOverrides=@{},[switch]$SkipMetadata)
    Write-Fixture (Join-Path $Build 'CMakeCache.txt') ("CMAKE_CXX_COMPILER:FILEPATH=$CachedCompiler`nRIFELOVE_TENSORRT_RUNTIME_DIR:PATH=$cacheRuntime`n" +
        "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4`nCMAKE_CACHE_MINOR_VERSION:INTERNAL=4`nCMAKE_CACHE_PATCH_VERSION:INTERNAL=4`n")
    if ($SkipMetadata) { return }
    $record = [ordered]@{CMAKE_CXX_COMPILER=$RecordedCompiler.Replace('\','/');CMAKE_CXX_COMPILER_ID='GNU';CMAKE_CXX_COMPILER_VERSION='16.2.0';
        CMAKE_CXX_PLATFORM_ID='MinGW';CMAKE_CXX_COMPILER_WRAPPER='';CMAKE_CXX_COMPILER_ARG1=''}
    foreach ($key in $RecordOverrides.Keys) { $record[$key] = $RecordOverrides[$key] }
    $metadata = ($record.GetEnumerator() | ForEach-Object { 'set(' + $_.Key + ' "' + $_.Value + '")' }) -join "`n"
    Write-Fixture (Join-Path $Build 'CMakeFiles/4.4.4/CMakeCXXCompiler.cmake') $metadata
}
$compiler = Join-Path $root 'toolchain/bin/g++.exe'
Write-Fixture $compiler 'MZ test fixture only; never executed'
$build = Join-Path $root 'selection-build'
$cacheRuntime = Join-Path $root 'cache-runtime'
$envRuntime = Join-Path $root 'environment-runtime'
$parameterRuntime = Join-Path $root 'parameter-runtime'
$envBin = Join-Path $root 'environment-bin'
$parameterBin = Join-Path $root 'parameter-bin'
Write-CompilerBuildFixture $build $compiler
$environment = @{RIFELOVE_TENSORRT_RUNTIME_DIR=$envRuntime;RIFELOVE_MINGW_BIN=$envBin}
$selection = Resolve-NativePackageBuildDependencies $build -EnvironmentValues @{}
Assert-Equal $cacheRuntime $selection.inferenceRuntimeDirectory 'No environment uses runtime from selected build'
Assert-Equal (Split-Path -Parent $compiler) $selection.minGWBinDirectory 'No environment uses cached actual compiler directory'
$selection = Resolve-NativePackageBuildDependencies $build -EnvironmentValues $environment
Assert-Equal $envRuntime $selection.inferenceRuntimeDirectory 'Documented runtime environment takes precedence over cache'
Assert-Equal $envBin $selection.minGWBinDirectory 'Explicit MinGW environment takes precedence over compiler directory'
$selection = Resolve-NativePackageBuildDependencies $build -InferenceRuntimeDirectory $parameterRuntime -MinGWBinDirectory $parameterBin -EnvironmentValues $environment
Assert-Equal $parameterRuntime $selection.inferenceRuntimeDirectory 'Runtime parameter overrides environment'
Assert-Equal $parameterBin $selection.minGWBinDirectory 'MinGW parameter overrides environment'

$missing = Join-Path $root 'missing-runtime-build'
Write-Fixture (Join-Path $missing 'CMakeCache.txt') "CMAKE_CXX_COMPILER:FILEPATH=$compiler`n"
Assert-Rejected { Resolve-NativePackageBuildDependencies $missing -EnvironmentValues @{} } 'Pass -InferenceRuntimeDirectory'
foreach ($entry in @(
    @{name='shim-path';compiler=(Join-Path $root 'shims/g++.exe');fragment='shim or wrapper'},
    @{name='cmd-wrapper';compiler=(Join-Path $root 'wrapper/g++.cmd');fragment='shim or wrapper'}
)) {
    $fixture = Join-Path $root $entry.name
    Write-CompilerBuildFixture $fixture $entry.compiler
    Assert-Rejected { Resolve-NativePackageBuildDependencies $fixture -EnvironmentValues @{} } $entry.fragment
}
$sidecarCompiler = Join-Path $root 'sidecar/bin/g++.exe'
Write-Fixture $sidecarCompiler 'MZ shim fixture; never executed'
Write-Fixture ([IO.Path]::ChangeExtension($sidecarCompiler,'.shim')) 'path = real-compiler.exe'
$sidecarBuild = Join-Path $root 'sidecar-build'
Write-CompilerBuildFixture $sidecarBuild $sidecarCompiler
Assert-Rejected { Resolve-NativePackageBuildDependencies $sidecarBuild -EnvironmentValues @{} } 'shim or wrapper'

foreach ($name in @('g++','g++.exe')) {
    $fixture = Join-Path $root ('bare-' + $name.Replace('+','p'))
    Write-CompilerBuildFixture $fixture $name $compiler
    $selection = Resolve-NativePackageBuildDependencies $fixture -EnvironmentValues @{}
    Assert-Equal $compiler $selection.compiler 'Bare cache compiler resolved only through absolute generated identity'
    Assert-Equal 'CMakeCXXCompiler.cmake:CMAKE_CXX_COMPILER' $selection.compilerSource 'Bare cache resolution source'
}
$noMetadata = Join-Path $root 'bare-missing-metadata'
Write-CompilerBuildFixture $noMetadata 'g++' $compiler -SkipMetadata
Assert-Rejected { Resolve-NativePackageBuildDependencies $noMetadata -EnvironmentValues @{} } 'Generated CMake compiler identity missing'
foreach ($entry in @(
    @{name='different-bare-name';cache='clang++';record=$compiler;fragment='compiler name and generated compiler identity disagree'},
    @{name='relative-cache-path';cache='bin/g++.exe';record=$compiler;fragment='relative paths and PATH wrappers'},
    @{name='drive-relative-cache';cache='C:g++.exe';record=$compiler;fragment='relative paths and PATH wrappers'},
    @{name='different-absolute-path';cache=(Join-Path $root 'other/bin/g++.exe');record=$compiler;fragment='cache and generated compiler identity disagree'},
    @{name='relative-metadata';cache='g++';record='g++.exe';fragment='absolute compiler path'},
    @{name='metadata-shim';cache='g++';record=(Join-Path $root 'shims/g++.exe');fragment='shim or wrapper'},
    @{name='metadata-sidecar';cache='g++';record=$sidecarCompiler;fragment='shim or wrapper'}
)) {
    $fixture = Join-Path $root $entry.name
    Write-CompilerBuildFixture $fixture $entry.cache $entry.record
    Assert-Rejected { Resolve-NativePackageBuildDependencies $fixture -EnvironmentValues @{} } $entry.fragment
}
foreach ($entry in @(
    @{name='metadata-non-gnu';override=@{CMAKE_CXX_COMPILER_ID='Clang'}},
    @{name='metadata-non-mingw';override=@{CMAKE_CXX_PLATFORM_ID='Windows'}},
    @{name='metadata-wrapper';override=@{CMAKE_CXX_COMPILER_WRAPPER='ccache'}},
    @{name='metadata-argument';override=@{CMAKE_CXX_COMPILER_ARG1='--wrapper'}}
)) {
    $fixture = Join-Path $root $entry.name
    Write-CompilerBuildFixture $fixture 'g++' $compiler $entry.override
    Assert-Rejected { Resolve-NativePackageBuildDependencies $fixture -EnvironmentValues @{} } 'without a compiler wrapper'
}
$duplicate = Join-Path $root 'duplicate-compiler-record'
Write-CompilerBuildFixture $duplicate 'g++' $compiler
[IO.File]::AppendAllText((Join-Path $duplicate 'CMakeFiles/4.4.4/CMakeCXXCompiler.cmake'),"`nset(CMAKE_CXX_COMPILER `"g++.exe`")`n")
Assert-Rejected { Resolve-NativePackageBuildDependencies $duplicate -EnvironmentValues @{} } 'exactly one CMAKE_CXX_COMPILER'

$reference = Join-Path $root 'reference-runtime'
$matching = Join-Path $root 'matching-runtime'
$different = Join-Path $root 'different-runtime'
$differentVersion = Join-Path $root 'different-version-runtime'
foreach ($directory in @($reference,$matching,$different,$differentVersion)) {
    Write-Fixture (Join-Path $directory 'onnxruntime.dll') $(if ($directory -ceq $different) { 'different test-only SDK' } else { 'same test-only SDK' })
    Write-Fixture (Join-Path $directory 'tensorrt-runtime.version') "10.9.0.34`n$('a'*64)`n"
    Write-Fixture (Join-Path $directory 'nvinfer_10.dll') 'same test-only TensorRT SDK'
}
Write-Fixture (Join-Path $differentVersion 'tensorrt-runtime.version') "10.9.0.35`n$('b'*64)`n"
$runtimeSelection = [pscustomobject]@{cache=@{RIFELOVE_TENSORRT_RUNTIME_DIR=$reference};inferenceRuntimeDirectory=$matching}
Assert-NativeInferenceRuntimeIdentity $runtimeSelection
$runtimeSelection.inferenceRuntimeDirectory = $different
Assert-Rejected { Assert-NativeInferenceRuntimeIdentity $runtimeSelection } 'Inference runtime identity mismatch'
$runtimeSelection.inferenceRuntimeDirectory = $differentVersion
Assert-Rejected { Assert-NativeInferenceRuntimeIdentity $runtimeSelection } 'Inference runtime identity mismatch for tensorrt-runtime.version'
$incomplete = Join-Path $root 'incomplete-runtime'
Write-Fixture (Join-Path $incomplete 'onnxruntime.dll') 'same test-only SDK'
Write-Fixture (Join-Path $incomplete 'tensorrt-runtime.version') "10.9.0.34`n$('a'*64)`n"
$runtimeSelection.inferenceRuntimeDirectory = $incomplete
Assert-Rejected { Assert-NativeInferenceRuntimeIdentity $runtimeSelection } 'DLL inventory differs'

$livePassed = $false
$presetResult = $null
if ($ValidateDefaultReleasePreset -and -not $RealBuildDirectory) { throw '-ValidateDefaultReleasePreset requires -RealBuildDirectory for local SDK references.' }
if ($RealBuildDirectory) {
    $live = Resolve-NativePackageBuildDependencies $RealBuildDirectory -EnvironmentValues @{}
    $identity = Get-NativeCompilerRuntimeIdentity $live
    Assert-Equal 'GNU' $identity.compilerId 'Live compiler identity'
    Assert-Equal 'x86_64-w64-mingw32' $identity.target 'Live compiler target'
    if ($ValidateDefaultReleasePreset) {
        # Exercise the unmodified real preset twice: a repeat configure reapplies
        # its bare compiler name. Only this new retained fixture is configured.
        if (-not $DependencySourceDirectory) { $DependencySourceDirectory = Join-Path $repositoryRoot 'build/windows-dev/_deps' }
        $presetBuild = Join-Path $root 'default-release-preset-build'
        $arguments = @('--preset','windows-release','-B',$presetBuild,'-DRIFELOVE_ALLOW_DEPENDENCY_DOWNLOADS=OFF','-DFETCHCONTENT_FULLY_DISCONNECTED=ON')
        foreach ($key in @('RIFELOVE_FFMPEG_ROOT','RIFELOVE_ONNX_ROOT','RIFELOVE_TENSORRT_RUNTIME_DIR','RIFELOVE_JSON_ROOT')) {
            if (-not (Test-Path -LiteralPath $live.cache[$key] -PathType Container)) { throw "Real preset needs the selected local build's $key" }
            $arguments += "-D$key=$($live.cache[$key])"
        }
        foreach ($dependency in @('spdlog','sqlite3','googletest')) {
            $source = [IO.Path]::GetFullPath((Join-Path $DependencySourceDirectory "$dependency-src"))
            if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "Offline preset dependency source missing: $source; pass -DependencySourceDirectory." }
            $arguments += "-DFETCHCONTENT_SOURCE_DIR_$($dependency.ToUpperInvariant())=$source"
        }
        Push-Location $repositoryRoot
        try {
            foreach ($pass in 1,2) {
                & cmake @arguments *> (Join-Path $root "default-release-configure-$pass.log")
                if ($LASTEXITCODE -ne 0) { throw "Default release preset configure $pass failed; see retained log." }
            }
        } finally { Pop-Location }
        $presetSelection = Resolve-NativePackageBuildDependencies $presetBuild -EnvironmentValues @{}
        $presetDefinition = (Get-Content -LiteralPath (Join-Path $repositoryRoot 'CMakePresets.json') -Raw | ConvertFrom-Json).configurePresets | Where-Object name -eq 'windows-release'
        Assert-Equal $presetDefinition.cacheVariables.CMAKE_CXX_COMPILER $presetSelection.cache['CMAKE_CXX_COMPILER'] 'Repeated actual preset retains its declared compiler name'
        Assert-Equal 'CMakeCXXCompiler.cmake:CMAKE_CXX_COMPILER' $presetSelection.compilerSource 'Actual preset uses generated absolute compiler identity'
        $runtimeLinksCreated = 0
        foreach ($name in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
            $destination = Join-Path $presetBuild $name
            if (-not (Test-Path -LiteralPath $destination)) {
                New-Item -ItemType HardLink -Path $destination -Target (Join-Path $presetSelection.minGWBinDirectory $name) | Out-Null
                $runtimeLinksCreated++
            }
        }
        $presetIdentity = Get-NativeCompilerRuntimeIdentity $presetSelection
        Assert-Equal $identity.compilerSha256 $presetIdentity.compilerSha256 'Actual preset compiler matches validated live toolchain'
        $presetResult = [ordered]@{buildDirectory=$presetBuild;configurePasses=2;cacheCompiler=$presetSelection.cache['CMAKE_CXX_COMPILER'];
            compiler=$presetSelection.compiler;compilerSource=$presetSelection.compilerSource;compilerIdentity=$presetIdentity;
            compiledProduct=$false;matchingCompilerRuntimeHardlinksCreated=$runtimeLinksCreated;configuredReleaseBuildDirectory=$false}
    }
    $staleBuild = Join-Path $root 'stale-compiler-build'
    $cmakeVersion = @('CMAKE_CACHE_MAJOR_VERSION','CMAKE_CACHE_MINOR_VERSION','CMAKE_CACHE_PATCH_VERSION' | ForEach-Object { $live.cache[$_] }) -join '.'
    $staleMetadata = @(
        "set(CMAKE_CXX_COMPILER `"$($live.compiler.Replace('\','/'))`")",
        'set(CMAKE_CXX_COMPILER_ID "GNU")',
        'set(CMAKE_CXX_COMPILER_VERSION "0.0.0")',
        'set(CMAKE_CXX_PLATFORM_ID "MinGW")',
        'set(CMAKE_CXX_COMPILER_WRAPPER "")',
        'set(CMAKE_CXX_COMPILER_ARG1 "")'
    ) -join "`n"
    Write-Fixture (Join-Path $staleBuild "CMakeFiles/$cmakeVersion/CMakeCXXCompiler.cmake") $staleMetadata
    $staleSelection = [pscustomobject]@{cache=$live.cache;buildDirectory=$staleBuild;compiler=$live.compiler;minGWBinDirectory=$live.minGWBinDirectory}
    Assert-Rejected { Get-NativeCompilerRuntimeIdentity $staleSelection } 'Current compiler version differs from the recorded build'
    $wrongBin = Join-Path $root 'different-mingw-bin'
    New-Item -ItemType Directory -Path $wrongBin | Out-Null
    foreach ($name in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        if ($name -ceq 'libstdc++-6.dll') { Write-Fixture (Join-Path $wrongBin $name) 'different test-only compiler runtime' }
        else { New-Item -ItemType HardLink -Path (Join-Path $wrongBin $name) -Target (Join-Path $live.minGWBinDirectory $name) | Out-Null }
    }
    $live.minGWBinDirectory = $wrongBin
    Assert-Rejected { Get-NativeCompilerRuntimeIdentity $live } 'Compiler runtime identity mismatch'
    $livePassed = $true
}
[ordered]@{
    result='passed';fixtures=$root;selectionPrecedenceVerified=$true;missingConfigurationRejected=$true
    bareNamesResolvedFromGeneratedMetadata=$true;missingMetadataAndCacheIdentityMismatchesRejected=$true
    generatedRelativePathsWrappersAndKnownShimsRejected=$true;duplicateCompilerRecordsRejected=$true;differentInferenceRuntimeRejected=$true
    inferenceVersionSidecarMismatchRejected=$true;incompleteInferenceInventoryRejected=$true
    liveCompilerAndVersionRuntimeMismatchVerified=$livePassed;defaultReleasePreset=$presetResult;launchedProductExe=$false;cleanupPerformed=$false
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'test-result.json') -Encoding utf8NoBOM
Write-Output "Native dependency checks passed. Fixtures retained at $root"
