[CmdletBinding()]
param(
    [string]$OutputRoot = "",
    [switch]$Apply,
    # Explicitly list versions whose canonical private OSS archive was already verified.
    # Only the exact root-level generated ZIP for that version is eligible.
    [string[]]$RemoveArchivedVersionZip = @(),
    [string[]]$KeepPackage = @(
        "RifeLove-local-2026-10-26-user-delivery",
        "RifeLove-local-2026-10-26-tensorrt-sr",
        "RifeLove-offline-2026-10-26-v0.1.3"
    )
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot "../.."))
$expectedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "out"))
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = $expectedRoot }
$root = [IO.Path]::GetFullPath($OutputRoot)
if (-not $root.Equals($expectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "For safety, this tool only prunes generated folders inside this repository's out directory."
}
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Generated output directory was not found: $root" }
if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw "Refusing to prune a linked output directory."
}

function Test-ContainedPath([string]$Parent, [string]$Candidate) {
    $relative = [IO.Path]::GetRelativePath($Parent, $Candidate)
    return -not [IO.Path]::IsPathRooted($relative) -and $relative -ne "." -and
        -not $relative.Equals("..", [StringComparison]::Ordinal) -and
        -not $relative.StartsWith(".." + [IO.Path]::DirectorySeparatorChar, [StringComparison]::Ordinal)
}

function Test-TreeHasReparsePoint([string]$Directory, [switch]$PreserveData) {
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($Directory)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($current)) {
            if ($PreserveData -and $current.Equals($Directory, [StringComparison]::OrdinalIgnoreCase) -and
                [IO.Path]::GetFileName($entry).Equals("data", [StringComparison]::OrdinalIgnoreCase)) { continue }
            $attributes = [IO.File]::GetAttributes($entry)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) {
                $isPackageRuntimeJunction = $current.Equals($Directory, [StringComparison]::OrdinalIgnoreCase) -and
                    [IO.Path]::GetFileName($entry).Equals("runtime", [StringComparison]::OrdinalIgnoreCase)
                if ($isPackageRuntimeJunction) {
                    $link = Get-Item -LiteralPath $entry -Force
                    $target = [string]$link.Target
                    if ($link.LinkType -eq "Junction" -and -not [string]::IsNullOrWhiteSpace($target) -and
                        (Test-Path -LiteralPath $target -PathType Container) -and
                        -not (Test-ContainedPath $Directory ([IO.Path]::GetFullPath($target)))) {
                        continue
                    }
                }
                return $true
            }
            if ($attributes -band [IO.FileAttributes]::Directory) { $pending.Push($entry) }
        }
    }
    return $false
}

function Remove-GeneratedEntry([string]$PackageRoot, [string]$Entry) {
    $absolute = [IO.Path]::GetFullPath($Entry)
    if (-not (Test-ContainedPath $PackageRoot $absolute)) {
        throw "Unsafe generated child path: $absolute"
    }
    $attributes = [IO.File]::GetAttributes($absolute)
    if ($attributes -band [IO.FileAttributes]::ReparsePoint) {
        # Delete links as directory entries; never enumerate or recurse through
        # their targets. Runtime junctions point into the shared SDK tree.
        if ($attributes -band [IO.FileAttributes]::Directory) { [IO.Directory]::Delete($absolute, $false) }
        else { [IO.File]::Delete($absolute) }
        return
    }
    if ($attributes -band [IO.FileAttributes]::Directory) {
        foreach ($child in [IO.Directory]::EnumerateFileSystemEntries($absolute)) {
            Remove-GeneratedEntry $PackageRoot $child
        }
        [IO.Directory]::Delete($absolute, $false)
    } else {
        [IO.File]::Delete($absolute)
    }
}

function Get-GeneratedBytes([string]$Directory, [switch]$PreserveData) {
    [long]$total = 0
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($Directory)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($current)) {
            if ($PreserveData -and $current.Equals($Directory, [StringComparison]::OrdinalIgnoreCase) -and
                [IO.Path]::GetFileName($entry).Equals("data", [StringComparison]::OrdinalIgnoreCase)) { continue }
            $attributes = [IO.File]::GetAttributes($entry)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($attributes -band [IO.FileAttributes]::Directory) { $pending.Push($entry) }
            else { $total += ([IO.FileInfo]::new($entry)).Length }
        }
    }
    return $total
}

$activePackageRoots = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($process in (Get-CimInstance Win32_Process -Filter "Name='RifeLove.exe'" -ErrorAction SilentlyContinue)) {
    if ([string]::IsNullOrWhiteSpace($process.ExecutablePath)) { continue }
    $exe = [IO.Path]::GetFullPath([string]$process.ExecutablePath)
    if (-not (Test-ContainedPath $root $exe)) { continue }
    $relative = [IO.Path]::GetRelativePath($root, $exe)
    $packageName = $relative.Split([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)[0]
    if ($packageName) { [void]$activePackageRoots.Add((Join-Path $root $packageName)) }
    [void]$activePackageRoots.Add((Split-Path -Parent $exe))
}

$plan = [System.Collections.Generic.List[object]]::new()
$skipped = [System.Collections.Generic.List[string]]::new()
$keep = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in $KeepPackage) { [void]$keep.Add($name) }
foreach ($package in (Get-ChildItem -LiteralPath $root -Directory -Force | Where-Object { $_.Name -match '^RifeLove-(local|offline|portable)(-|$)' })) {
    $absolute = [IO.Path]::GetFullPath($package.FullName)
    if (-not (Test-ContainedPath $root $absolute) -or -not (Split-Path -Parent $absolute).Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe generated package path: $absolute"
    }
    if ($keep.Contains($package.Name) -or $activePackageRoots.Contains($absolute)) { continue }
    if ($package.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $skipped.Add($package.Name + " (linked directory)")
        continue
    }
    $hasData = Test-Path -LiteralPath (Join-Path $absolute "data") -PathType Container
    if (Test-TreeHasReparsePoint $absolute -PreserveData:$hasData) {
        $skipped.Add($package.Name + " (contains a linked path)")
        continue
    }
    $bytes = Get-GeneratedBytes $absolute -PreserveData:$hasData
    if ($bytes -eq 0 -and $hasData) { continue }
    $plan.Add([pscustomobject]@{ Name=$package.Name; Path=$absolute; Kind="folder"; PreserveData=$hasData; Bytes=$bytes })
}

$candidateRoot = Join-Path $root "release-candidates"
if (Test-Path -LiteralPath $candidateRoot -PathType Container) {
    $candidateRootInfo = Get-Item -LiteralPath $candidateRoot -Force
    if ($candidateRootInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $skipped.Add("release-candidates (linked directory)")
    } else {
        foreach ($versionDirectory in (Get-ChildItem -LiteralPath $candidateRoot -Directory -Force)) {
            if ($versionDirectory.Name -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { continue }
            $versionPath = [IO.Path]::GetFullPath($versionDirectory.FullName)
            if (-not (Test-ContainedPath $candidateRoot $versionPath) -or -not (Split-Path -Parent $versionPath).Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Unsafe release-candidate version directory: $versionPath"
            }
            $portablePath = [IO.Path]::GetFullPath((Join-Path $versionPath "portable"))
            if (-not (Test-Path -LiteralPath $portablePath -PathType Container)) { continue }
            if (-not (Test-ContainedPath $versionPath $portablePath) -or -not (Split-Path -Parent $portablePath).Equals($versionPath, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Unsafe release-candidate package directory: $portablePath"
            }
            if ($activePackageRoots.Contains($portablePath)) { $skipped.Add("release-candidates/$($versionDirectory.Name)/portable (running)"); continue }
            $portableInfo = Get-Item -LiteralPath $portablePath -Force
            if ($portableInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                $skipped.Add("release-candidates/$($versionDirectory.Name)/portable (linked directory)")
                continue
            }
            $hasData = Test-Path -LiteralPath (Join-Path $portablePath "data") -PathType Container
            if (Test-TreeHasReparsePoint $portablePath -PreserveData:$hasData) {
                $skipped.Add("release-candidates/$($versionDirectory.Name)/portable (contains a linked path)")
                continue
            }
            $bytes = Get-GeneratedBytes $portablePath -PreserveData:$hasData
            if ($bytes -eq 0 -and $hasData) { continue }
            $plan.Add([pscustomobject]@{
                Name="release-candidates/$($versionDirectory.Name)/portable"
                Path=$portablePath
                Kind="candidate"
                PreserveData=$hasData
                Bytes=$bytes
            })
        }
    }
}

foreach ($archive in (Get-ChildItem -LiteralPath $root -File -Force | Where-Object {
    $_.Name -match '^RifeLove-offline(?:-[a-z0-9-]+)?-\d{4}-\d{2}-\d{2}-win-x64\.zip$'
})) {
    $absolute = [IO.Path]::GetFullPath($archive.FullName)
    if (-not (Test-ContainedPath $root $absolute) -or -not (Split-Path -Parent $absolute).Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe temporary archive path: $absolute"
    }
    if ($archive.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        $skipped.Add($archive.Name + " (linked file)")
        continue
    }
    $plan.Add([pscustomobject]@{ Name=$archive.Name; Path=$absolute; Kind="archive"; PreserveData=$false; Bytes=$archive.Length })
}

foreach ($version in $RemoveArchivedVersionZip) {
    if ($version -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { throw "Invalid archived release version: $version" }
    $name = "RifeLove-$version-win-x64.zip"
    $absolute = [IO.Path]::GetFullPath((Join-Path $root $name))
    if (-not (Test-ContainedPath $root $absolute) -or -not (Split-Path -Parent $absolute).Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe archived release path: $absolute"
    }
    if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) { throw "Archived release ZIP was not found: $name" }
    $file = Get-Item -LiteralPath $absolute -Force
    if ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing to remove a linked archived release ZIP: $name" }
    $plan.Add([pscustomobject]@{ Name=$name; Path=$absolute; Kind="archived-version-zip"; PreserveData=$false; Bytes=$file.Length })
}

[long]$plannedBytes = ($plan | Measure-Object -Property Bytes -Sum).Sum
if (-not $plannedBytes) { $plannedBytes = 0 }
Write-Output ("Found {0} inactive generated item(s); {1:N2} GiB of file contents can be removed while preserving local data." -f $plan.Count, ($plannedBytes / 1GB))
foreach ($item in $plan) {
    $action = if ($item.Kind -eq "archive") { "remove temporary dated archive; keep versioned archives" }
        elseif ($item.Kind -eq "archived-version-zip") { "remove exact local copy after private OSS archive verification" }
        elseif ($item.PreserveData) { "remove generated files; keep data/" }
        else { "remove generated folder" }
    Write-Output ("{0} — {1:N2} GiB — {2}" -f $item.Name, ($item.Bytes / 1GB), $action)
}
foreach ($item in $skipped) { Write-Output ("Skipped {0}" -f $item) }
if (-not $Apply) {
    Write-Output "Dry run only. Re-run with -Apply to prune these inactive generated outputs. User data, unlisted version archives, configured keeps, and running packages are excluded."
    exit 0
}

foreach ($item in $plan) {
    if ($item.Kind -eq "archive") {
        if (-not (Test-ContainedPath $root $item.Path) -or -not (Split-Path -Parent $item.Path).Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unsafe temporary archive path: $($item.Path)"
        }
        $current = Get-Item -LiteralPath $item.Path -Force
        if ($current.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Temporary archive became a linked file: $($item.Name)" }
        Remove-Item -LiteralPath $item.Path -Force
    } elseif ($item.Kind -eq "archived-version-zip") {
        if (-not (Test-ContainedPath $root $item.Path) -or -not (Split-Path -Parent $item.Path).Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($item.Path) -ne $item.Name) { throw "Unsafe archived release path: $($item.Path)" }
        $current = Get-Item -LiteralPath $item.Path -Force
        if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $current.Length -ne $item.Bytes) {
            throw "Archived release ZIP changed after preview: $($item.Name)"
        }
        Remove-Item -LiteralPath $item.Path -Force
    } elseif ($item.PreserveData) {
        if ($item.Kind -eq "candidate") {
            $candidateVersionPath = Split-Path -Parent $item.Path
            if (-not (Test-ContainedPath $root $item.Path) -or (Split-Path -Leaf $item.Path) -ne "portable" -or
                -not (Split-Path -Parent $candidateVersionPath).Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase) -or
                (Split-Path -Leaf $candidateVersionPath) -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') {
                throw "Unsafe release-candidate cleanup target: $($item.Path)"
            }
            $current = Get-Item -LiteralPath $item.Path -Force
            if (($current.Attributes -band [IO.FileAttributes]::ReparsePoint) -or (Test-TreeHasReparsePoint $item.Path -PreserveData)) {
                throw "Release-candidate package changed or gained a linked path: $($item.Name)"
            }
        }
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($item.Path)) {
            if ([IO.Path]::GetFileName($entry).Equals("data", [StringComparison]::OrdinalIgnoreCase)) { continue }
            Remove-GeneratedEntry $item.Path $entry
        }
    } else {
        $validTopLevelPackage = $item.Kind -eq "folder" -and (Split-Path -Parent $item.Path).Equals($root, [StringComparison]::OrdinalIgnoreCase)
        $candidateVersionPath = Split-Path -Parent $item.Path
        $validCandidate = $item.Kind -eq "candidate" -and (Test-ContainedPath $root $item.Path) -and
            (Split-Path -Leaf $item.Path) -eq "portable" -and
            (Split-Path -Parent $candidateVersionPath).Equals($candidateRoot, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $candidateVersionPath) -match '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$'
        if (-not (Test-ContainedPath $root $item.Path) -or (-not $validTopLevelPackage -and -not $validCandidate)) {
            throw "Unsafe generated package path: $($item.Path)"
        }
        $current = Get-Item -LiteralPath $item.Path -Force
        if ($current.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Generated package became a linked directory: $($item.Name)" }
        Remove-GeneratedEntry $root $item.Path
    }
    Write-Output ("Pruned {0}" -f $item.Name)
}
Write-Output "Pruning complete. Unlisted version archives and local data were preserved."
