# Resolve dependencies from explicit inputs or the selected build, never from a
# machine-specific install path or an unrelated compiler found on PATH.
function Read-NativePackageBuildCache {
    param([Parameter(Mandatory=$true)][string]$BuildDirectory)
    $path = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Native build cache missing: $path" }
    $cache = @{}
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        if ($line -match '^([^/#:][^:]*):[^=]+=(.*)$') { $cache[$Matches[1]] = $Matches[2] }
    }
    return $cache
}

function Resolve-NativePackageCompilerRecord {
    param([Parameter(Mandatory=$true)][string]$BuildDirectory,
          [Parameter(Mandatory=$true)][System.Collections.IDictionary]$Cache)
    $components = @('CMAKE_CACHE_MAJOR_VERSION','CMAKE_CACHE_MINOR_VERSION','CMAKE_CACHE_PATCH_VERSION' | ForEach-Object { [string]$Cache[$_] })
    if (@($components | Where-Object { $_ -notmatch '^\d+$' }).Count) {
        throw 'Generated CMake compiler identity requires the selected build cache to record its CMake version. Reconfigure before packaging.'
    }
    $metadataPath = Join-Path $BuildDirectory ("CMakeFiles/{0}/CMakeCXXCompiler.cmake" -f ($components -join '.'))
    if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) { throw "Generated CMake compiler identity missing: $metadataPath" }
    $metadata = [IO.File]::ReadAllText($metadataPath)
    $recorded = @{}
    foreach ($key in @('CMAKE_CXX_COMPILER','CMAKE_CXX_COMPILER_ID','CMAKE_CXX_COMPILER_VERSION','CMAKE_CXX_PLATFORM_ID','CMAKE_CXX_COMPILER_WRAPPER','CMAKE_CXX_COMPILER_ARG1')) {
        $entries = [regex]::Matches($metadata,'(?m)^set\(' + [regex]::Escape($key) + ' "([^"\r\n]*)"\)\r?$')
        if ($entries.Count -ne 1) { throw "Generated CMake compiler identity must record exactly one $key" }
        $recorded[$key] = $entries[0].Groups[1].Value
    }
    if ($recorded['CMAKE_CXX_COMPILER_ID'] -cne 'GNU' -or $recorded['CMAKE_CXX_PLATFORM_ID'] -cne 'MinGW' -or
        $recorded['CMAKE_CXX_COMPILER_WRAPPER'] -or $recorded['CMAKE_CXX_COMPILER_ARG1']) {
        throw 'This native package requires the recorded GNU MinGW x64 toolchain without a compiler wrapper.'
    }
    $compiler = [string]$recorded['CMAKE_CXX_COMPILER']
    if (-not [IO.Path]::IsPathFullyQualified($compiler)) { throw 'Generated CMake compiler identity must record an absolute compiler path.' }
    $compiler = [IO.Path]::GetFullPath($compiler)
    if ([IO.Path]::GetExtension($compiler) -ine '.exe' -or $compiler -match '(?i)[\\/]shims[\\/]' -or
        (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($compiler,'.shim')))) {
        throw 'CMAKE_CXX_COMPILER points to a shim or wrapper. Reconfigure with the actual MinGW g++.exe.'
    }
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "Recorded MinGW compiler missing: $compiler" }
    if ((Get-Item -LiteralPath $compiler -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Recorded MinGW compiler must be the real executable, not a linked wrapper.'
    }
    $cachedCompiler = [string]$Cache['CMAKE_CXX_COMPILER']
    $source = 'CMakeCache:CMAKE_CXX_COMPILER'
    if ([IO.Path]::IsPathFullyQualified($cachedCompiler)) {
        if ([IO.Path]::GetFullPath($cachedCompiler) -ine $compiler) {
            throw 'CMake cache and generated compiler identity disagree. Reconfigure and rebuild before packaging.'
        }
    } else {
        # A preset can reapply its bare name after CMake has identified the real
        # compiler. Resolve only through that generated record, never PATH.
        if ($cachedCompiler -notmatch '^[A-Za-z0-9_+.-]+$' -or
            ($cachedCompiler -ine [IO.Path]::GetFileName($compiler) -and
             $cachedCompiler -ine [IO.Path]::GetFileNameWithoutExtension($compiler))) {
            throw 'CMake cache compiler name and generated compiler identity disagree; relative paths and PATH wrappers are not accepted.'
        }
        $source = 'CMakeCXXCompiler.cmake:CMAKE_CXX_COMPILER'
    }
    return [pscustomobject]@{ compiler=$compiler; recorded=$recorded; metadataPath=$metadataPath; source=$source }
}

function Resolve-NativePackageBuildDependencies {
    param([Parameter(Mandatory=$true)][string]$BuildDirectory,
          [string]$InferenceRuntimeDirectory = '', [string]$MinGWBinDirectory = '',
          [System.Collections.IDictionary]$EnvironmentValues = $null)
    $build = [IO.Path]::GetFullPath($BuildDirectory)
    $cache = Read-NativePackageBuildCache $build
    if ($null -eq $EnvironmentValues) {
        $EnvironmentValues = @{
            RIFELOVE_TENSORRT_RUNTIME_DIR = [Environment]::GetEnvironmentVariable('RIFELOVE_TENSORRT_RUNTIME_DIR')
            RIFELOVE_MINGW_BIN = [Environment]::GetEnvironmentVariable('RIFELOVE_MINGW_BIN')
        }
    }
    $runtimeSource = 'parameter:InferenceRuntimeDirectory'
    if ([string]::IsNullOrWhiteSpace($InferenceRuntimeDirectory)) {
        $InferenceRuntimeDirectory = [string]$EnvironmentValues['RIFELOVE_TENSORRT_RUNTIME_DIR']
        $runtimeSource = 'environment:RIFELOVE_TENSORRT_RUNTIME_DIR'
    }
    if ([string]::IsNullOrWhiteSpace($InferenceRuntimeDirectory)) {
        $InferenceRuntimeDirectory = [string]$cache['RIFELOVE_TENSORRT_RUNTIME_DIR']
        $runtimeSource = 'CMakeCache:RIFELOVE_TENSORRT_RUNTIME_DIR'
    }
    if ([string]::IsNullOrWhiteSpace($InferenceRuntimeDirectory)) {
        throw 'Inference runtime is not configured. Pass -InferenceRuntimeDirectory, set RIFELOVE_TENSORRT_RUNTIME_DIR, or configure it in the selected build.'
    }
    $compilerRecord = Resolve-NativePackageCompilerRecord -BuildDirectory $build -Cache $cache
    $compiler = $compilerRecord.compiler
    $mingwSource = 'parameter:MinGWBinDirectory'
    if ([string]::IsNullOrWhiteSpace($MinGWBinDirectory)) {
        $MinGWBinDirectory = [string]$EnvironmentValues['RIFELOVE_MINGW_BIN']
        $mingwSource = 'environment:RIFELOVE_MINGW_BIN'
    }
    if ([string]::IsNullOrWhiteSpace($MinGWBinDirectory)) {
        $MinGWBinDirectory = Split-Path -Parent $compiler
        $mingwSource = $compilerRecord.source + '-directory'
    }
    return [pscustomobject]@{
        buildDirectory = $build; cache = $cache; compiler = $compiler; compilerSource = $compilerRecord.source
        inferenceRuntimeDirectory = [IO.Path]::GetFullPath($InferenceRuntimeDirectory)
        inferenceRuntimeSource = $runtimeSource
        minGWBinDirectory = [IO.Path]::GetFullPath($MinGWBinDirectory)
        minGWSource = $mingwSource
    }
}

function Get-NativeCompilerRuntimeIdentity {
    param([Parameter(Mandatory=$true)]$Selection)
    $compilerRecord = Resolve-NativePackageCompilerRecord -BuildDirectory $Selection.buildDirectory -Cache $Selection.cache
    $recorded = $compilerRecord.recorded
    if ($compilerRecord.compiler -ine $Selection.compiler) {
        throw 'CMake cache and generated compiler identity disagree. Reconfigure and rebuild before packaging.'
    }
    $compiler = $Selection.compiler
    $file = [IO.File]::OpenRead($compiler)
    try { if ($file.ReadByte() -ne 77 -or $file.ReadByte() -ne 90) { throw 'Cached compiler is not a native Windows executable; refusing a wrapper.' } }
    finally { $file.Dispose() }
    $version = @(& $compiler -dumpfullversion -dumpversion 2>&1)
    if ($LASTEXITCODE -ne 0 -or $version.Count -ne 1 -or [string]$version[0] -cne $recorded['CMAKE_CXX_COMPILER_VERSION']) {
        throw 'Current compiler version differs from the recorded build. Reconfigure and rebuild; do not mix current toolchain DLLs with an older EXE.'
    }
    $target = @(& $compiler -dumpmachine 2>&1)
    if ($LASTEXITCODE -ne 0 -or $target.Count -ne 1 -or [string]$target[0] -cne 'x86_64-w64-mingw32') {
        throw 'The recorded compiler is not the required x86_64-w64-mingw32 toolchain.'
    }
    $hashes = [ordered]@{}
    foreach ($name in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
        $reported = @(& $compiler "-print-file-name=$name" 2>&1)
        if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1 -or -not [IO.Path]::IsPathRooted([string]$reported[0])) {
            throw "Compiler cannot resolve its own $name. Supply an intact MinGW installation and rebuild."
        }
        $paths = @([string]$reported[0], (Join-Path $Selection.minGWBinDirectory $name), (Join-Path $Selection.buildDirectory $name))
        $expected = $null
        foreach ($path in $paths) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Compiler runtime missing: $path" }
            if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Compiler runtime cannot be a link: $path" }
            $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($null -ne $expected -and $hash -cne $expected) {
                throw "Compiler runtime identity mismatch for $name between the cached compiler, selected directory and EXE build directory. Rebuild or supply matching DLLs."
            }
            $expected = $hash
        }
        $hashes[$name] = $expected
    }
    return [pscustomobject]@{
        compilerId = 'GNU'; version = [string]$version[0]; target = [string]$target[0]
        compilerSha256 = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant()
        runtimeSha256 = $hashes
    }
}

function Assert-NativeInferenceRuntimeIdentity {
    param([Parameter(Mandatory=$true)]$Selection)
    $reference = [string]$Selection.cache['RIFELOVE_TENSORRT_RUNTIME_DIR']
    if ([string]::IsNullOrWhiteSpace($reference) -or -not (Test-Path -LiteralPath $reference -PathType Container)) {
        throw 'The selected build has no available RIFELOVE_TENSORRT_RUNTIME_DIR reference. Reconfigure with the reviewed runtime before packaging.'
    }
    $reference = [IO.Path]::GetFullPath($reference)
    $candidate = $Selection.inferenceRuntimeDirectory
    if (-not (Test-Path -LiteralPath $candidate -PathType Container)) { throw "Selected inference runtime missing: $candidate" }
    foreach ($directory in @($reference,$candidate)) {
        if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Inference runtime directory cannot be a link: $directory"
        }
    }
    $referenceNames = @(Get-ChildItem -LiteralPath $reference -File -Filter '*.dll' | ForEach-Object Name | Sort-Object)
    $candidateNames = @(Get-ChildItem -LiteralPath $candidate -File -Filter '*.dll' | ForEach-Object Name | Sort-Object)
    if ($referenceNames.Count -eq 0 -or $candidateNames.Count -ne $referenceNames.Count -or
        (Compare-Object -ReferenceObject $referenceNames -DifferenceObject $candidateNames)) {
        throw 'Selected inference runtime DLL inventory differs from the runtime recorded in the build.'
    }
    foreach ($name in @($referenceNames) + @('tensorrt-runtime.version')) {
        $selected = Join-Path $candidate $name
        $recorded = Join-Path $reference $name
        foreach ($path in @($selected,$recorded)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Inference runtime identity file missing: $path" }
            if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Inference runtime identity file cannot be a link: $path" }
        }
        if ($reference -ine $candidate -and
            (Get-FileHash -LiteralPath $selected -Algorithm SHA256).Hash -cne (Get-FileHash -LiteralPath $recorded -Algorithm SHA256).Hash) {
            throw "Inference runtime identity mismatch for $name; overrides must match the runtime recorded in the build."
        }
    }
}
