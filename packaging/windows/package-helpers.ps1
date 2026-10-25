function Get-RunningPackageProcesses {
    param([Parameter(Mandatory=$true)][string]$TargetPath)
    $targetPrefix = [IO.Path]::GetFullPath($TargetPath).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    foreach ($process in (Get-Process -Name "RifeLove", "rifelove-cli" -ErrorAction SilentlyContinue)) {
        $processPath = $null
        try { $processPath = $process.Path } catch { }
        if (-not [string]::IsNullOrWhiteSpace($processPath)) {
            $fullProcessPath = [IO.Path]::GetFullPath($processPath)
            if ($fullProcessPath.StartsWith($targetPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                [pscustomobject]@{Name=$process.ProcessName;Id=$process.Id;Path=$fullProcessPath}
            }
        }
    }
}

function Assert-PackageNotRunning {
    param([Parameter(Mandatory=$true)][string]$TargetPath)
    $running = @(Get-RunningPackageProcesses -TargetPath $TargetPath)
    if ($running.Count -gt 0) {
        $details = ($running | ForEach-Object { "{0} (PID {1})" -f $_.Name, $_.Id }) -join ", "
        throw "Cannot replace the local package while it is running: $details. Close it first; user data is preserved."
    }
}

function Remove-PackagePath {
    param([Parameter(Mandatory=$true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        if ($item.PSIsContainer) { [IO.Directory]::Delete($Path, $false) }
        else { Remove-Item -LiteralPath $Path -Force }
        return
    }
    if ($item.PSIsContainer) {
        foreach ($child in (Get-ChildItem -LiteralPath $Path -Force)) {
            Remove-PackagePath -Path $child.FullName
        }
    }
    Remove-Item -LiteralPath $Path -Force
}

function Get-PackageTreeItemsWithoutFollowingLinks {
    param([Parameter(Mandatory=$true)][string]$Path)
    foreach ($child in (Get-ChildItem -LiteralPath $Path -Force)) {
        $child
        if ($child.PSIsContainer -and -not ($child.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Get-PackageTreeItemsWithoutFollowingLinks -Path $child.FullName
        }
    }
}

function Preserve-PackageUserState {
    param([Parameter(Mandatory=$true)][string]$OldRoot,[Parameter(Mandatory=$true)][string]$NewRoot)

    $oldData = Join-Path $OldRoot "data"
    $newData = Join-Path $NewRoot "data"
    if (Test-Path -LiteralPath $oldData -PathType Container) {
        $oldDataItem = Get-Item -LiteralPath $oldData -Force
        if ($oldDataItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to follow linked user data outside the package: $oldData"
        }
        $oldDataItems = @(Get-PackageTreeItemsWithoutFollowingLinks -Path $oldData)
        $linkedDataItems = @($oldDataItems | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
        if ($linkedDataItems.Count -gt 0) {
            throw "Refusing to replace a package containing linked user data: $($linkedDataItems[0].FullName)"
        }
        foreach ($file in @($oldDataItems | Where-Object { -not $_.PSIsContainer })) {
            $relative = $file.FullName.Substring($oldData.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
            if ($relative -ieq "tensorrt-validation.json" -or $relative -match '^tensorrt-qualification([\\/]|$)' -or $relative -ieq "queue.sqlite-shm") { continue }
            $destination = Join-Path $newData $relative
            $destinationDirectory = Split-Path -Path $destination -Parent
            New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
        }
    }

    $oldModels = Join-Path $OldRoot "models"
    $newModels = Join-Path $NewRoot "models"
    if (Test-Path -LiteralPath $oldModels -PathType Container) {
        $oldModelsItem = Get-Item -LiteralPath $oldModels -Force
        if ($oldModelsItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to follow a linked model directory outside the package: $oldModels"
        }
        foreach ($directory in (Get-ChildItem -LiteralPath $oldModels -Directory -Force)) {
            if ($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing to replace a package containing a linked model directory: $($directory.FullName)"
            }
            $descriptorPath = Join-Path $directory.FullName "model.json"
            if (-not (Test-Path -LiteralPath $descriptorPath -PathType Leaf)) { continue }
            try { $descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json } catch { continue }
            if ($descriptor.id -notmatch '^[A-Za-z0-9._-]{1,128}$') { continue }
            $destination = Join-Path $newModels ([string]$descriptor.id)
            if (-not (Test-Path -LiteralPath $destination)) {
                $modelItems = @(Get-PackageTreeItemsWithoutFollowingLinks -Path $directory.FullName)
                $linkedModelItems = @($modelItems | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
                if ($linkedModelItems.Count -gt 0) {
                    throw "Refusing to replace a package containing linked files in a user model: $($linkedModelItems[0].FullName)"
                }
                Copy-Item -LiteralPath $directory.FullName -Destination $destination -Recurse
            }
        }
    }

    $oldDefaultPath = Join-Path $oldModels "default-model.txt"
    if (Test-Path -LiteralPath $oldDefaultPath -PathType Leaf) {
        $oldDefault = (Get-Content -LiteralPath $oldDefaultPath -Raw).Trim()
        if ($oldDefault -match '^[A-Za-z0-9._-]{1,128}$' -and (Test-Path -LiteralPath (Join-Path $newModels $oldDefault) -PathType Container)) {
            Set-Content -LiteralPath (Join-Path $newModels "default-model.txt") -Value $oldDefault -Encoding UTF8
        }
    }
}

function Promote-PackageDirectory {
    param([Parameter(Mandatory=$true)][string]$StagingPath,[Parameter(Mandatory=$true)][string]$TargetPath)

    Assert-PackageNotRunning -TargetPath $TargetPath
    if (Test-Path -LiteralPath $TargetPath) {
        $existingTarget = Get-Item -LiteralPath $TargetPath -Force
        if ($existingTarget.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to replace a linked package path: $TargetPath"
        }
    }
    if (-not (Test-Path -LiteralPath $StagingPath -PathType Container)) {
        throw "The qualified staging package is missing: $StagingPath"
    }

    $target = [IO.Path]::GetFullPath($TargetPath)
    $packageLeaf = Split-Path -Path $target -Leaf
    $packageParent = Split-Path -Path $target -Parent
    $backupRoot = Join-Path $packageParent (".{0}.previous-{1}" -f $packageLeaf, [Guid]::NewGuid().ToString("N"))
    $movedOldPackage = $false
    try {
        if (Test-Path -LiteralPath $target) {
            Move-Item -LiteralPath $target -Destination $backupRoot
            $movedOldPackage = $true
        }
        Move-Item -LiteralPath $StagingPath -Destination $target
    } catch {
        if ($movedOldPackage -and -not (Test-Path -LiteralPath $target) -and (Test-Path -LiteralPath $backupRoot)) {
            Move-Item -LiteralPath $backupRoot -Destination $target
        }
        throw
    }
    if ($movedOldPackage -and (Test-Path -LiteralPath $backupRoot)) {
        try { Remove-PackagePath -Path $backupRoot }
        catch { Write-Warning "The new package is active, but the previous package could not be cleaned up: $backupRoot" }
    }
}

function New-PackageChecksumManifest {
    param(
        [Parameter(Mandatory=$true)][string]$PackageRoot,
        [Parameter(Mandatory=$true)][string]$RuntimeRoot,
        [Parameter(Mandatory=$true)][string]$QualificationEvidencePath)

    $packageRootFull = [IO.Path]::GetFullPath($PackageRoot)
    $runtimeRootFull = [IO.Path]::GetFullPath($RuntimeRoot)
    $runtimeItem = Get-Item -LiteralPath $runtimeRootFull -Force
    if (-not $runtimeItem.PSIsContainer -or ($runtimeItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "The runtime checksum root must be a local, non-linked directory: $runtimeRootFull"
    }

    $checksums = @{}
    foreach ($file in @(Get-PackageTreeItemsWithoutFollowingLinks -Path $packageRootFull | Where-Object { -not $_.PSIsContainer -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) })) {
        $relative = $file.FullName.Substring($packageRootFull.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
        if ($relative -ieq "SHA256SUMS.txt" -or $relative -match '^(data[\\/])') { continue }
        $manifestPath = $relative.Replace([IO.Path]::DirectorySeparatorChar, '/')
        $checksums[$manifestPath] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }

    $evidenceFull = [IO.Path]::GetFullPath($QualificationEvidencePath)
    if (-not $evidenceFull.StartsWith($packageRootFull.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Qualification evidence must be inside the package: $evidenceFull"
    }
    if (-not (Test-Path -LiteralPath $evidenceFull -PathType Leaf)) { throw "Qualification evidence is missing: $evidenceFull" }
    $evidenceRelative = $evidenceFull.Substring($packageRootFull.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar).Replace([IO.Path]::DirectorySeparatorChar, '/')
    $checksums[$evidenceRelative] = (Get-FileHash -LiteralPath $evidenceFull -Algorithm SHA256).Hash.ToLowerInvariant()

    foreach ($file in @(Get-PackageTreeItemsWithoutFollowingLinks -Path $runtimeRootFull | Where-Object { -not $_.PSIsContainer -and -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) })) {
        $relative = $file.FullName.Substring($runtimeRootFull.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
        $manifestPath = (Join-Path "runtime" $relative).Replace([IO.Path]::DirectorySeparatorChar, '/')
        $checksums[$manifestPath] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }

    $manifest = @("RifeLove local bundle SHA-256")
    foreach ($path in ($checksums.Keys | Sort-Object)) { $manifest += "$($checksums[$path])  $path" }
    return $manifest
}
