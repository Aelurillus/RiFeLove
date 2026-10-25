$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../../packaging/windows/package-helpers.ps1")

function Assert-Equal {
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual,[Parameter(Mandatory=$true)][string]$Message)
    if ($Expected -cne $Actual) { throw "${Message}: expected '$Expected', found '$Actual'" }
}

function Write-TestFile {
    param([string]$Path,[string]$Content)
    $parent = Split-Path -Path $Path -Parent
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    Set-Content -LiteralPath $Path -Value $Content -Encoding UTF8
}

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ("rifelove-package-state-{0}" -f [Guid]::NewGuid().ToString("N"))
$oldRoot = Join-Path $testRoot "old"
$newRoot = Join-Path $testRoot "new"
$linkedTarget = Join-Path $testRoot "linked-target"

try {
    New-Item -ItemType Directory -Path (Join-Path $newRoot "data") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $newRoot "models/balanced") -Force | Out-Null
    Write-TestFile (Join-Path $newRoot "data/preferences.ini") "fresh-defaults"
    Write-TestFile (Join-Path $newRoot "data/tensorrt-validation.json") "new-qualification"
    Write-TestFile (Join-Path $newRoot "models/balanced/model.json") '{"id":"balanced","version":"new"}'

    Write-TestFile (Join-Path $oldRoot "data/preferences.ini") "user-preferences"
    Write-TestFile (Join-Path $oldRoot "data/language.ini") "language=zh-TW"
    Write-TestFile (Join-Path $oldRoot "data/queue.sqlite") "queue-database"
    Write-TestFile (Join-Path $oldRoot "data/queue.sqlite-wal") "queue-wal"
    Write-TestFile (Join-Path $oldRoot "data/queue.sqlite-shm") "rebuildable-shm"
    Write-TestFile (Join-Path $oldRoot "data/tasks/task-1/segments/segment-0001.mkv") "checkpoint-segment"
    Write-TestFile (Join-Path $oldRoot "data/tensorrt-validation.json") "stale-qualification"
    Write-TestFile (Join-Path $oldRoot "data/tensorrt-qualification/old.json") "temporary-qualification"
    Write-TestFile (Join-Path $oldRoot "models/custom-model/model.json") '{"id":"custom-model","version":"1"}'
    Write-TestFile (Join-Path $oldRoot "models/custom-model/model.onnx") "custom-weights"
    Write-TestFile (Join-Path $oldRoot "models/balanced/model.json") '{"id":"balanced","version":"old"}'
    Write-TestFile (Join-Path $oldRoot "models/balanced/model.onnx") "old-bundled-weights"
    Write-TestFile (Join-Path $oldRoot "models/default-model.txt") "custom-model"

    Preserve-PackageUserState -OldRoot $oldRoot -NewRoot $newRoot

    Assert-Equal "user-preferences" (Get-Content -LiteralPath (Join-Path $newRoot "data/preferences.ini") -Raw).Trim() "Settings survive an upgrade"
    Assert-Equal "language=zh-TW" (Get-Content -LiteralPath (Join-Path $newRoot "data/language.ini") -Raw).Trim() "Language survives an upgrade"
    Assert-Equal "queue-database" (Get-Content -LiteralPath (Join-Path $newRoot "data/queue.sqlite") -Raw).Trim() "Queue database survives an upgrade"
    Assert-Equal "queue-wal" (Get-Content -LiteralPath (Join-Path $newRoot "data/queue.sqlite-wal") -Raw).Trim() "Committed SQLite WAL survives an upgrade"
    Assert-Equal "checkpoint-segment" (Get-Content -LiteralPath (Join-Path $newRoot "data/tasks/task-1/segments/segment-0001.mkv") -Raw).Trim() "Checkpoint artifacts survive an upgrade"
    Assert-Equal "new-qualification" (Get-Content -LiteralPath (Join-Path $newRoot "data/tensorrt-validation.json") -Raw).Trim() "Fresh qualification evidence replaces stale evidence"
    if (Test-Path -LiteralPath (Join-Path $newRoot "data/queue.sqlite-shm")) { throw "SQLite shared-memory state must be rebuilt, not copied." }
    if (Test-Path -LiteralPath (Join-Path $newRoot "data/tensorrt-qualification/old.json")) { throw "Temporary qualification files must not be copied." }
    Assert-Equal "custom-model" (Get-Content -LiteralPath (Join-Path $newRoot "models/default-model.txt") -Raw).Trim() "Existing valid default model survives an upgrade"
    Assert-Equal "custom-weights" (Get-Content -LiteralPath (Join-Path $newRoot "models/custom-model/model.onnx") -Raw).Trim() "User-installed model survives an upgrade"
    Assert-Equal '{"id":"balanced","version":"new"}' (Get-Content -LiteralPath (Join-Path $newRoot "models/balanced/model.json") -Raw).Trim() "A newly bundled model takes precedence over an older copy"

    Assert-PackageNotRunning -TargetPath (Join-Path $testRoot "no-running-package")

    $promotionTarget = Join-Path $testRoot "promotion-target"
    $promotionStage = Join-Path $testRoot "promotion-stage"
    Write-TestFile (Join-Path $promotionTarget "old.txt") "old-package"
    Write-TestFile (Join-Path $promotionStage "new.txt") "new-package"
    Promote-PackageDirectory -StagingPath $promotionStage -TargetPath $promotionTarget
    Assert-Equal "new-package" (Get-Content -LiteralPath (Join-Path $promotionTarget "new.txt") -Raw).Trim() "A qualified package replaces the old folder"
    if (Test-Path -LiteralPath (Join-Path $promotionTarget "old.txt")) { throw "The replaced package must not leak old files into the new package." }

    $rollbackTarget = Join-Path $testRoot "rollback-target"
    $missingStage = Join-Path $testRoot "missing-stage"
    Write-TestFile (Join-Path $rollbackTarget "old.txt") "rollback-package"
    $promotionFailed = $false
    try { Promote-PackageDirectory -StagingPath $missingStage -TargetPath $rollbackTarget }
    catch { $promotionFailed = $_.Exception.Message.Contains("staging package is missing") }
    if (-not $promotionFailed) { throw "Promotion must reject a missing qualified package." }
    Assert-Equal "rollback-package" (Get-Content -LiteralPath (Join-Path $rollbackTarget "old.txt") -Raw).Trim() "A missing stage leaves the existing package untouched"

    New-Item -ItemType Directory -Path $linkedTarget -Force | Out-Null
    Write-TestFile (Join-Path $linkedTarget "keep.txt") "external-data"
    $junction = Join-Path $oldRoot "linked"
    $junctionCreated = $false
    try {
        New-Item -ItemType Junction -Path $junction -Target $linkedTarget -ErrorAction Stop | Out-Null
        $junctionCreated = $true
    } catch {
        Write-Output "SKIP junction safety subcheck: $($_.Exception.Message)"
    }
    if ($junctionCreated) {
        $linkedStateRoot = Join-Path $testRoot "linked-state"
        New-Item -ItemType Directory -Path (Join-Path $linkedStateRoot "data") -Force | Out-Null
        $stateJunction = Join-Path $linkedStateRoot "data/external"
        New-Item -ItemType Junction -Path $stateJunction -Target $linkedTarget -ErrorAction Stop | Out-Null
        $refusedLinkedState = $false
        try { Preserve-PackageUserState -OldRoot $linkedStateRoot -NewRoot $newRoot }
        catch { $refusedLinkedState = $_.Exception.Message.Contains("linked user data") }
        if (-not $refusedLinkedState) { throw "Package-state migration must refuse linked user data." }

        Remove-PackagePath -Path $oldRoot
        Assert-Equal "external-data" (Get-Content -LiteralPath (Join-Path $linkedTarget "keep.txt") -Raw).Trim() "Removing a package must not follow junctions"
    }

    Write-Output "Package helper checks passed."
} finally {
    Remove-PackagePath -Path $testRoot
}
