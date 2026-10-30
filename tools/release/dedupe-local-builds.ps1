[CmdletBinding()]
param(
    [string]$OutputRoot = "",
    [long]$MinimumFileSizeBytes = 1048576,
    [switch]$Apply
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptRoot "../.."))
$expectedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot "out"))
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = $expectedRoot }
$root = [IO.Path]::GetFullPath($OutputRoot)
if (-not $root.Equals($expectedRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "For safety, this tool only deduplicates the repository's generated out directory."
}
if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Generated output directory was not found: $root" }
if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw "Refusing to scan a linked output directory."
}
if ($MinimumFileSizeBytes -lt 1048576) { throw "MinimumFileSizeBytes must be at least 1 MiB." }

if (-not ("RifeLoveBuildFileLinks" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class RifeLoveBuildFileLinks
{
    [StructLayout(LayoutKind.Sequential)]
    private struct ByHandleFileInformation
    {
        public uint Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    public sealed class Identity
    {
        public string Value { get; set; }
        public uint LinkCount { get; set; }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out ByHandleFileInformation info);

    [DllImport("kernel32.dll", EntryPoint = "CreateHardLinkW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CreateHardLink(string newPath, string existingPath, IntPtr securityAttributes);

    public static Identity GetIdentity(string path)
    {
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        {
            ByHandleFileInformation info;
            if (!GetFileInformationByHandle(stream.SafeFileHandle, out info))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return new Identity {
                Value = info.VolumeSerialNumber.ToString("X8") + ":" + info.FileIndexHigh.ToString("X8") + info.FileIndexLow.ToString("X8"),
                LinkCount = info.NumberOfLinks
            };
        }
    }
}
"@
}

$activePackageRoots = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($process in (Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -in @('RifeLove.exe', 'RifeLoveNative.exe') })) {
    if ([string]::IsNullOrWhiteSpace($process.ExecutablePath)) { continue }
    $exe = [IO.Path]::GetFullPath([string]$process.ExecutablePath)
    if (-not $exe.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { continue }
    $relative = $exe.Substring($root.Length + 1)
    $packageName = $relative.Split([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)[0]
    if ($packageName) { [void]$activePackageRoots.Add((Join-Path $root $packageName)) }
}

$packages = @(Get-ChildItem -LiteralPath $root -Directory -Force | Where-Object {
    $_.Name -match '^RifeLove(?:Native)?-(local|offline|portable)(-|$)' -and -not $activePackageRoots.Contains($_.FullName)
})
if ($activePackageRoots.Count) {
    Write-Output ("Protected running package(s): " + (($activePackageRoots | ForEach-Object { Split-Path -Leaf $_ }) -join ", "))
}

$eligibleFiles = [System.Collections.Generic.List[object]]::new()
foreach ($package in $packages) {
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($package.FullName)
    while ($pending.Count -gt 0) {
        $directory = $pending.Pop()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($directory)) {
            $attributes = [IO.File]::GetAttributes($entry)
            if ($attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
            if ($attributes -band [IO.FileAttributes]::Directory) {
                $relative = $entry.Substring($package.FullName.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
                $first = $relative.Split([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)[0]
                if ($first -ieq "data") { continue }
                $pending.Push($entry)
                continue
            }
            $file = [IO.FileInfo]::new($entry)
            if ($file.Length -lt $MinimumFileSizeBytes) { continue }
            if ($file.Extension -notin @(".dll", ".exe", ".onnx")) { continue }
            $eligibleFiles.Add([pscustomobject]@{ Path=$file.FullName; Bytes=$file.Length; Package=$package.FullName })
        }
    }
}

$linkPlans = [System.Collections.Generic.List[object]]::new()
$reclaimGroups = [System.Collections.Generic.List[object]]::new()
$hashBuckets = $eligibleFiles | Group-Object Bytes | Where-Object Count -gt 1
foreach ($bucket in $hashBuckets) {
    $withHashes = foreach ($file in $bucket.Group) {
        [pscustomobject]@{
            Path = $file.Path
            Bytes = $file.Bytes
            Package = $file.Package
            Hash = (Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash
            Identity = [RifeLoveBuildFileLinks]::GetIdentity($file.Path)
        }
    }
    foreach ($sameContent in ($withHashes | Group-Object Hash | Where-Object Count -gt 1)) {
        $identities = @($sameContent.Group | Group-Object { $_.Identity.Value } | Sort-Object Count -Descending)
        if ($identities.Count -lt 2) { continue }
        $canonicalIdentity = $identities[0].Name
        $canonical = $sameContent.Group | Where-Object { $_.Identity.Value -eq $canonicalIdentity } | Sort-Object Path | Select-Object -First 1
        foreach ($duplicateIdentity in ($identities | Where-Object Name -ne $canonicalIdentity)) {
            $duplicates = @($sameContent.Group | Where-Object { $_.Identity.Value -eq $duplicateIdentity.Name })
            $identityInfo = $duplicates[0].Identity
            $reclaimId = [Guid]::NewGuid().ToString("N")
            $canReclaim = $identityInfo.LinkCount -eq $duplicates.Count
            if ($canReclaim) {
                $reclaimGroups.Add([pscustomobject]@{ Id=$reclaimId; Bytes=$duplicates[0].Bytes })
            }
            foreach ($duplicate in $duplicates) {
                $sourceVolume = [IO.Path]::GetPathRoot($canonical.Path)
                $targetVolume = [IO.Path]::GetPathRoot($duplicate.Path)
                if (-not $sourceVolume.Equals($targetVolume, [StringComparison]::OrdinalIgnoreCase)) { continue }
                $linkPlans.Add([pscustomobject]@{ Source=$canonical.Path; Target=$duplicate.Path; Bytes=$duplicate.Bytes; Hash=$duplicate.Hash; ReclaimId=$reclaimId })
            }
        }
    }
}

$estimatedBytes = ($reclaimGroups | Measure-Object Bytes -Sum).Sum
if (-not $estimatedBytes) { $estimatedBytes = 0 }
Write-Output ("Scanned {0} inactive package(s) and {1} eligible binaries." -f $packages.Count, $eligibleFiles.Count)
Write-Output ("Found {0} duplicate paths across {1} fully removable copies; estimated space recovery: {2:N2} GiB." -f $linkPlans.Count, $reclaimGroups.Count, ($estimatedBytes / 1GB))
if (-not $Apply) {
    Write-Output "Dry run only. Re-run with -Apply to replace byte-identical generated binaries with NTFS hard links. User data under data/ and running packages are excluded."
    exit 0
}

$linked = 0
$failed = 0
$groupResults = @{}
foreach ($plan in $linkPlans) {
    $backup = $plan.Target + ".rifelove-dedupe-" + [Guid]::NewGuid().ToString("N") + ".tmp"
    try {
        if ((Get-FileHash -LiteralPath $plan.Source -Algorithm SHA256).Hash -ne $plan.Hash -or
            (Get-FileHash -LiteralPath $plan.Target -Algorithm SHA256).Hash -ne $plan.Hash) {
            throw "A candidate changed after the dry-run scan."
        }
        [IO.File]::Move($plan.Target, $backup)
        if (-not [RifeLoveBuildFileLinks]::CreateHardLink($plan.Target, $plan.Source, [IntPtr]::Zero)) {
            throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())
        }
        [IO.File]::Delete($backup)
        $linked++
        if (-not $groupResults.ContainsKey($plan.ReclaimId)) { $groupResults[$plan.ReclaimId] = @{ Linked=0; Failed=0 } }
        $groupResults[$plan.ReclaimId].Linked++
    } catch {
        if (Test-Path -LiteralPath $backup -PathType Leaf) {
            if (Test-Path -LiteralPath $plan.Target) { Remove-Item -LiteralPath $plan.Target -Force }
            [IO.File]::Move($backup, $plan.Target)
        }
        $failed++
        if (-not $groupResults.ContainsKey($plan.ReclaimId)) { $groupResults[$plan.ReclaimId] = @{ Linked=0; Failed=0 } }
        $groupResults[$plan.ReclaimId].Failed++
        Write-Warning ("Skipped {0}: {1}" -f [IO.Path]::GetFileName($plan.Target), $_.Exception.Message)
    }
}
$recoveredBytes = 0L
foreach ($group in $reclaimGroups) {
    $result = $groupResults[$group.Id]
    $targetCount = @($linkPlans | Where-Object ReclaimId -eq $group.Id).Count
    if ($result -and $result.Failed -eq 0 -and $result.Linked -eq $targetCount) { $recoveredBytes += [long]$group.Bytes }
}
Write-Output ("Created {0} hard links; skipped {1}; estimated recovered space {2:N2} GiB." -f $linked, $failed, ($recoveredBytes / 1GB))
