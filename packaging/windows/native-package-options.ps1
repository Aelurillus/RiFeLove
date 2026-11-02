# Explicit data-mode initialization for a newly staged native package. Never
# copies user state, removes a marker, or changes an existing package's mode.
function Assert-NativePackageDirectoriesDisjoint {
    param([Parameter(Mandatory=$true)][string]$SourceDirectory,
          [Parameter(Mandatory=$true)][string]$OutputDirectory)
    $source = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
    $target = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
    $separator = [IO.Path]::DirectorySeparatorChar
    if ([string]::Equals($source,$target,[StringComparison]::OrdinalIgnoreCase) -or
        $source.StartsWith($target + $separator,[StringComparison]::OrdinalIgnoreCase) -or
        $target.StartsWith($source + $separator,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Source and output package directories must be disjoint; neither may contain the other.'
    }
    foreach ($path in @($source,$target)) {
        $cursor = $path
        while ($cursor) {
            if ((Test-Path -LiteralPath $cursor) -and
                ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Package directory ancestry cannot contain a link: $cursor"
            }
            $parent = [IO.Path]::GetDirectoryName($cursor)
            if ($parent -eq $cursor) { break }
            $cursor = $parent
        }
    }
}

function Initialize-NativePackageDataMode {
    param([Parameter(Mandatory=$true)][string]$PackageRoot,
          [switch]$PortableMode)
    foreach ($directory in @($PackageRoot, (Join-Path $PackageRoot 'data'))) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) { throw "New package data directory is missing: $directory" }
        if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "New package data directory cannot be a link: $directory"
        }
    }
    $dataRoot = Join-Path $PackageRoot 'data'
    if (@(Get-ChildItem -LiteralPath $dataRoot -Force).Count -ne 0) {
        throw "Data mode can only be initialized for a new empty package data directory."
    }
    $marker = Join-Path $PackageRoot 'portable-mode.txt'
    if (Test-Path -LiteralPath $marker) {
        throw "New package must not inherit or overwrite an existing portable marker."
    }
    $markerHash = $null
    if ($PortableMode) {
        $contents = [Text.Encoding]::UTF8.GetBytes("RifeLovePortable 1`n")
        $file = [IO.File]::Open($marker,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        try { $file.Write($contents,0,$contents.Length); $file.Flush($true) }
        finally { $file.Dispose() }
        $markerHash = (Get-FileHash -LiteralPath $marker -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    [pscustomobject]@{
        portableModeEnabled = [bool]$PortableMode
        dataDirectory = if ($PortableMode) { 'data' } else { '%LOCALAPPDATA%\RifeLove' }
        defaultDataDirectory = '%LOCALAPPDATA%\RifeLove'
        portableDataDirectory = 'data'
        markerName = 'portable-mode.txt'
        markerValue = 'RifeLovePortable 1'
        markerIncluded = [bool]$PortableMode
        markerSha256 = $markerHash
        dataState = 'new-empty-directory; no user queue, preferences or logs copied'
    }
}

# Both full and hard-link packaging use the same serialized Normal/Portable
# contract. This only describes the selected mode; it never changes user data.
function Set-NativePackageDataModeMetadata {
    param([Parameter(Mandatory=$true)][System.Collections.IDictionary]$Metadata,
          [Parameter(Mandatory=$true)][object]$DataMode,
          [switch]$Manifest)
    $Metadata['dataDirectory'] = $DataMode.dataDirectory
    $Metadata['defaultDataDirectory'] = $DataMode.defaultDataDirectory
    $Metadata['portableModeEnabled'] = [bool]$DataMode.portableModeEnabled
    $Metadata['portableDataDirectory'] = $DataMode.portableDataDirectory
    $Metadata['portableModeMarker'] = $DataMode.markerName
    $Metadata['portableModeMarkerValue'] = $DataMode.markerValue
    if ($Manifest) {
        $Metadata['portableModeMarkerIncluded'] = [bool]$DataMode.markerIncluded
        $Metadata['packagedDataState'] = $DataMode.dataState
    }
}

# Model selection is intentionally separate from data-mode handling so both
# full packaging and hard-link reuse resolve the bundle before touching any
# optional model directory.  Existing callers that do not pass BaseModelsOnly
# retain the historical four-model behavior.
function Resolve-NativeModelBundleSelection {
    param(
        [switch]$BaseModelsOnly,
        [switch]$IncludeTemporalRestoration,
        [switch]$IncludeCompressionRestoration,
        [switch]$IncludeDeblurRestoration
    )
    $includeTemporal = if ($BaseModelsOnly) { [bool]$IncludeTemporalRestoration } else { $true }
    $includeCompression = if ($BaseModelsOnly) { [bool]$IncludeCompressionRestoration } else { $true }
    $includeDeblur = [bool]$IncludeDeblurRestoration
    $modelIds = @('rife-4.25-fp32-sdr8-v3', 'realesrgan-x2plus-sdr8-v2')
    if ($includeTemporal) { $modelIds += 'fastdvdnet-clipped-five-frame-sdr8-eval' }
    if ($includeCompression) { $modelIds += 'stdf-r3-y7-sdr8-local-eval' }
    if ($includeDeblur) { $modelIds += 'nafnet-gopro-deblur-sdr8-fp32-eval' }
    [pscustomobject]@{
        mode = if ($BaseModelsOnly) { 'base' } else { 'full' }
        baseModels = @('rife-4.25-fp32-sdr8-v3', 'realesrgan-x2plus-sdr8-v2')
        includeTemporalRestoration = $includeTemporal
        includeCompressionRestoration = $includeCompression
        requireCompressionRestoration = [bool]$IncludeCompressionRestoration
        includeDeblurRestoration = $includeDeblur
        modelIds = $modelIds
    }
}

function Get-NativeExcludedModelRoots {
    param([Parameter(Mandatory=$true)][object]$Selection)
    $roots = @()
    if (-not $Selection.includeTemporalRestoration) {
        $roots += @(
            'models/fastdvdnet-clipped-five-frame-sdr8-eval',
            'licenses/Model-fastdvdnet-clipped-five-frame-sdr8-eval'
        )
    }
    if (-not $Selection.includeCompressionRestoration) {
        $roots += @(
            'models/stdf-r3-y7-sdr8-local-eval',
            'licenses/Model-stdf-r3-y7-sdr8-local-eval'
        )
    }
    if (-not $Selection.includeDeblurRestoration) {
        $roots += @(
            'models/nafnet-gopro-deblur-sdr8-fp32-eval',
            'licenses/Model-nafnet-gopro-deblur-sdr8-fp32-eval'
        )
    }
    if ($Selection.mode -ceq 'base') {
        # A base bundle has no recovery data to migrate, so a hidden legacy
        # RIFE checkpoint is not copied as a second interpolation model.
        $roots += @(
            'models/rife-4.25-fp32-sdr8-v2',
            'licenses/Model-rife-4.25-fp32-sdr8-v2'
        )
    }
    return @($roots | Sort-Object -Unique)
}

# This local-only evaluation pack has a directory name distinct from its model
# ID. Pin the graph and adapter, then verify the license and source claims made
# by its checksummed descriptor/sidecars. An arbitrary deblur model must not be
# relabelled as the validated NAFNet candidate.
function Assert-NativeDeblurModelIdentity {
    param([Parameter(Mandatory=$true)][object]$Descriptor,
          [Parameter(Mandatory=$true)][string]$GraphHash,
          [Parameter(Mandatory=$true)][string]$Directory)
    $expectedHash = 'b99b39f0bdf29f15205aa50dd23b7a14b571b0c7ae9576ba2f9417fccad4f056'
    if ($GraphHash -cne $expectedHash -or [string]$Descriptor.sha256 -cne $expectedHash -or
        [string]$Descriptor.id -cne 'nafnet-zoo-gopro-sdr8-local' -or
        [string]$Descriptor.family -cne 'video-restoration' -or
        [string]$Descriptor.adapter.id -cne 'nafnet-zoo-deblur-rgb-f32-nchw-v1' -or
        [int]$Descriptor.adapter.version -ne 1 -or
        [string]$Descriptor.license.id -cne 'MIT' -or
        [string]$Descriptor.license.file -cne 'LICENSE' -or
        [string]$Descriptor.provenance.manifestFile -cne 'source-manifest.json' -or
        [string]$Descriptor.validation.reportFile -cne 'validation.json' -or
        (@($Descriptor.capabilities.tasks).Count -ne 1) -or
        [string]@($Descriptor.capabilities.tasks)[0] -cne 'deblur' -or
        (@($Descriptor.capabilities.placements).Count -ne 1) -or
        [string]@($Descriptor.capabilities.placements)[0] -cne 'output-rate-after-interpolation' -or
        (@($Descriptor.capabilities.supportedInputProfiles).Count -ne 1) -or
        [string]@($Descriptor.capabilities.supportedInputProfiles)[0] -cne 'bt709-sdr8' -or
        [int]$Descriptor.capabilities.temporalFramesBefore -ne 0 -or
        [int]$Descriptor.capabilities.temporalFramesAfter -ne 0 -or
        [int]$Descriptor.capabilities.scaleNumerator -ne 1 -or
        [int]$Descriptor.capabilities.scaleDenominator -ne 1) {
        throw "NAFNet deblur model identity/capabilities mismatch: $Directory"
    }
    $source = Get-Content -LiteralPath (Join-Path $Directory 'source-manifest.json') -Raw | ConvertFrom-Json
    $validation = Get-Content -LiteralPath (Join-Path $Directory 'validation.json') -Raw | ConvertFrom-Json
    $conversionPath = Join-Path $Directory 'conversion.json'
    if (-not (Test-Path -LiteralPath $conversionPath -PathType Leaf) -or
        [string]$source.modelId -cne [string]$Descriptor.id -or
        [string]$source.upstreamRepository -cne 'https://github.com/opencv/opencv_zoo' -or
        [string]$source.license -cne 'MIT' -or
        [string]$source.derivedGraphSha256 -cne $expectedHash -or
        [string]$source.weightLicenseStatus -notmatch '(?i)unconfirmed.*local evaluation only' -or
        [string]$source.conversionReportSha256 -cne (Get-FileHash -LiteralPath $conversionPath -Algorithm SHA256).Hash.ToLowerInvariant() -or
        [string]$validation.model_sha256 -cne $expectedHash -or
        [string]$validation.model_adapter.id -cne [string]$Descriptor.adapter.id -or
        [string]$validation.status -notmatch '^passed_' -or
        (@($validation.capabilities.tasks).Count -ne 1) -or
        [string]@($validation.capabilities.tasks)[0] -cne 'deblur') {
        throw "NAFNet deblur provenance/validation mismatch: $Directory"
    }
}

function Test-NativeRelativePathExcluded {
    param([Parameter(Mandatory=$true)][string]$RelativePath,
          [Parameter(Mandatory=$true)][AllowEmptyCollection()][string[]]$ExcludedRoots,
          [object]$Selection = $null)
    $relative = $RelativePath.Replace('\','/').TrimStart('/')
    if ($null -ne $Selection -and $Selection.mode -ceq 'base' -and $relative.StartsWith('models/',[StringComparison]::OrdinalIgnoreCase)) {
        $modelName = ($relative -split '/')[1]
        if ($modelName -cne 'default-model.txt' -and $Selection.modelIds -notcontains $modelName) { return $true }
    }
    foreach ($root in $ExcludedRoots) {
        $normalizedRoot = ([string]$root).Replace('\','/').TrimEnd('/')
        if ([string]::Equals($relative,$normalizedRoot,[StringComparison]::OrdinalIgnoreCase) -or
            $relative.StartsWith($normalizedRoot + '/',[StringComparison]::OrdinalIgnoreCase) -or
            ($normalizedRoot.StartsWith('licenses/Model-',[StringComparison]::OrdinalIgnoreCase) -and
             $relative.StartsWith($normalizedRoot + '-',[StringComparison]::OrdinalIgnoreCase))) {
            return $true
        }
    }
    return $false
}
