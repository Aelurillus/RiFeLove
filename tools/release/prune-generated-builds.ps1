[CmdletBinding()]
param(
    [string]$OutputRoot = "",
    [switch]$Apply,
    [string[]]$KeepPackage = @(
        "RifeLove-local-2026-10-26-restoration-catalog",
        "RifeLove-portable-0.1.1",
        "RifeLove-portable-0.1.1-verified"
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
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { return $true }
            if ($attributes -band [IO.FileAttributes]::Directory) { $pending.Push($entry) }
        }
    }
    return $false
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

[long]$plannedBytes = ($plan | Measure-Object -Property Bytes -Sum).Sum
if (-not $plannedBytes) { $plannedBytes = 0 }
Write-Output ("Found {0} inactive generated item(s); {1:N2} GiB of file contents can be removed while preserving local data." -f $plan.Count, ($plannedBytes / 1GB))
foreach ($item in $plan) {
    $action = if ($item.Kind -eq "archive") { "remove temporary dated archive; keep versioned archives" }
        elseif ($item.PreserveData) { "remove generated files; keep data/" }
        else { "remove generated folder" }
    Write-Output ("{0} — {1:N2} GiB — {2}" -f $item.Name, ($item.Bytes / 1GB), $action)
}
foreach ($item in $skipped) { Write-Output ("Skipped {0}" -f $item) }
if (-not $Apply) {
    Write-Output "Dry run only. Re-run with -Apply to prune these inactive generated outputs. User data, version archives, the verified portable package, configured keeps, and running packages are excluded."
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
    } elseif ($item.PreserveData) {
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($item.Path)) {
            if ([IO.Path]::GetFileName($entry).Equals("data", [StringComparison]::OrdinalIgnoreCase)) { continue }
            if (-not (Test-ContainedPath $item.Path $entry)) { throw "Unsafe generated child path: $entry" }
            Remove-Item -LiteralPath $entry -Recurse -Force
        }
    } else {
        if (-not (Test-ContainedPath $root $item.Path) -or -not (Split-Path -Parent $item.Path).Equals($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unsafe generated package path: $($item.Path)"
        }
        Remove-Item -LiteralPath $item.Path -Recurse -Force
    }
    Write-Output ("Pruned {0}" -f $item.Name)
}
Write-Output "Pruning complete. Version archives and local data were preserved."
