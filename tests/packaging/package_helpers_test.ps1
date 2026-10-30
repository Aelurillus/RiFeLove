$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "../../packaging/windows/package-helpers.ps1")
. (Join-Path $PSScriptRoot "../../packaging/windows/ffmpeg-sdk-helpers.ps1")

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
$runtimeRoot = Join-Path $testRoot "runtime"

try {
    $enabledConfiguration = ConvertFrom-FfmpegConfiguration "ffmpeg version 8.1.3 test`nconfiguration: --enable-shared --enable-x86asm --enable-nvenc"
    Assert-Equal $true $enabledConfiguration.x86asmEnabled "Report actual x86asm build flag"
    $disabledConfiguration = ConvertFrom-FfmpegConfiguration "ffmpeg version 8.1.3 test`nconfiguration: --enable-shared --disable-x86asm --enable-nvenc"
    Assert-Equal $false $disabledConfiguration.x86asmEnabled "Report baseline build flag"
    if ($enabledConfiguration.configurationSha256 -ceq $disabledConfiguration.configurationSha256) { throw "Different SDK configurations need different identities." }
    foreach ($flags in @('--enable-shared', '--enable-x86asm --disable-x86asm')) {
        $rejected = $false
        try { ConvertFrom-FfmpegConfiguration "ffmpeg version test`nconfiguration: $flags" | Out-Null } catch { $rejected = $true }
        if (-not $rejected) { throw "Ambiguous x86asm configuration must be rejected: $flags" }
    }
    $sdkRoot = Join-Path $testRoot "sdk"
    $sdkRuntime = Join-Path $testRoot "sdk-runtime"
    Write-TestFile (Join-Path $sdkRoot "bin/swscale-9.dll") "selected SDK bytes"
    Write-TestFile (Join-Path $sdkRuntime "swscale-9.dll") "selected SDK bytes"
    $hashes = Assert-FfmpegRuntimeIdentity $sdkRoot $sdkRuntime @('swscale-9.dll')
    Assert-Equal 64 $hashes['swscale-9.dll'].Length "SDK DLL hash recorded"
    $unchangedPlan = Get-FfmpegRuntimeRefreshPlan $sdkRoot $sdkRuntime @('swscale-9.dll')
    Assert-Equal $false $unchangedPlan.files[0].replace "Identical SDK DLL remains reusable"
    Assert-Equal 0 $unchangedPlan.replacementBytes "Identical SDK DLL needs no additional bytes"
    Write-TestFile (Join-Path $sdkRuntime "swscale-9.dll") "old baseline bytes"
    $staleRuntimeRejected = $false
    try { Assert-FfmpegRuntimeIdentity $sdkRoot $sdkRuntime @('swscale-9.dll') | Out-Null }
    catch { $staleRuntimeRejected = $_.Exception.Message.Contains('differs from the selected SDK') }
    if (-not $staleRuntimeRejected) { throw "Old SDK DLLs must never be relabeled as a new build with the same source lock." }
    $beforeRefreshHash = (Get-FileHash -LiteralPath (Join-Path $sdkRuntime 'swscale-9.dll') -Algorithm SHA256).Hash
    $refreshPlan = Get-FfmpegRuntimeRefreshPlan $sdkRoot $sdkRuntime @('swscale-9.dll')
    Assert-Equal $true $refreshPlan.files[0].replace "Configuration refresh identifies a changed DLL"
    Assert-Equal (Get-Item (Join-Path $sdkRoot 'bin/swscale-9.dll')).Length $refreshPlan.replacementBytes "Configuration refresh reports exact added bytes"
    Assert-Equal $beforeRefreshHash (Get-FileHash -LiteralPath (Join-Path $sdkRuntime 'swscale-9.dll') -Algorithm SHA256).Hash "Planning never overwrites the source bundle"
    $modelHash = "a" * 64
    $qualification = [pscustomobject]@{
        schemaVersion = 1
        generatedAtUtc = [DateTimeOffset]::UtcNow.ToString("o")
        runtime = [pscustomobject]@{
            onnxRuntimeVersion = "ort-test"
            tensorRtVersion = "trt-test"
            tensorRtManifestSha256 = "b" * 64
        }
        models = @([pscustomobject]@{
            modelId = "test-model"
            modelVersion = "1.0"
            modelSha256 = $modelHash
            device = "NVIDIA Test GPU"
            computeCapability = "7.5"
            precision = "fp32"
            backend = "onnxruntime_tensorrt_cuda"
            referenceCasesPassed = 6
            maxAbsError = 0.001
            allowedMaxAbsError = 0.002
            minPsnrDb = 81.0
            requiredMinPsnrDb = 80.0
            tensorrtImageOperators = @("TRTConvolution")
        })
    }
    $modelPacks = @([pscustomobject]@{descriptor=[pscustomobject]@{id="test-model";version="1.0";sha256=$modelHash}})
    $testGpus = @([pscustomobject]@{name="NVIDIA Test GPU";computeCapability="7.5"})
    $qualificationArguments = @{
        Evidence = $qualification
        ModelPacks = $modelPacks
        OnnxRuntimeVersion = "ort-test"
        TensorRtVersion = "trt-test"
        TensorRtManifestSha256 = "b" * 64
        GpuIdentities = $testGpus
    }
    Assert-TensorRtQualificationEvidence @qualificationArguments

    $differentGpuArguments = $qualificationArguments.Clone()
    $differentGpuArguments.GpuIdentities = @([pscustomobject]@{name="Different GPU";computeCapability="7.5"})
    $differentGpuRejected = $false
    try { Assert-TensorRtQualificationEvidence @differentGpuArguments }
    catch { $differentGpuRejected = $_.Exception.Message.Contains("different GPU") }
    if (-not $differentGpuRejected) { throw "Cached qualification from another GPU must be rejected." }

    $poorEvidence = $qualification | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $poorEvidence.models[0].maxAbsError = 0.01
    $poorArguments = $qualificationArguments.Clone()
    $poorArguments.Evidence = $poorEvidence
    $poorNumericsRejected = $false
    try { Assert-TensorRtQualificationEvidence @poorArguments }
    catch { $poorNumericsRejected = $_.Exception.Message.Contains("numerical gates") }
    if (-not $poorNumericsRejected) { throw "Cached qualification that misses numerical gates must be rejected." }

    $missingEvidence = $qualification | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $missingEvidence.models[0].minPsnrDb = $null
    $missingArguments = $qualificationArguments.Clone()
    $missingArguments.Evidence = $missingEvidence
    $missingNumericsRejected = $false
    try { Assert-TensorRtQualificationEvidence @missingArguments }
    catch { $missingNumericsRejected = $_.Exception.Message.Contains("missing numerical evidence") }
    if (-not $missingNumericsRejected) { throw "Cached qualification without numeric measurements must be rejected." }

    $staleEvidence = $qualification | ConvertTo-Json -Depth 8 | ConvertFrom-Json
    $staleEvidence.generatedAtUtc = [DateTimeOffset]::UtcNow.AddDays(-31).ToString("o")
    $staleArguments = $qualificationArguments.Clone()
    $staleArguments.Evidence = $staleEvidence
    $staleEvidenceRejected = $false
    try { Assert-TensorRtQualificationEvidence @staleArguments }
    catch { $staleEvidenceRejected = $_.Exception.Message.Contains("validity window") }
    if (-not $staleEvidenceRejected) { throw "Stale cached qualification must be rejected." }

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

    Write-TestFile (Join-Path $newRoot "RifeLove.exe") "desktop-binary"
    Write-TestFile (Join-Path $newRoot "licenses/RifeLove-MIT.txt") "license-notice"
    Write-TestFile (Join-Path $newRoot "SHA256SUMS.txt") "previous-manifest"
    Write-TestFile (Join-Path $runtimeRoot "onnxruntime.dll") "runtime-binary"
    Write-TestFile (Join-Path $runtimeRoot "providers/cuda.dll") "provider-binary"
    $manifest = @(New-PackageChecksumManifest -PackageRoot $newRoot -RuntimeRoot $runtimeRoot -QualificationEvidencePath (Join-Path $newRoot "data/tensorrt-validation.json"))
    foreach ($path in @("RifeLove.exe","licenses/RifeLove-MIT.txt","models/custom-model/model.onnx","data/tensorrt-validation.json","runtime/onnxruntime.dll","runtime/providers/cuda.dll")) {
        if (-not ($manifest | Where-Object { $_ -match ("  " + [regex]::Escape($path) + '$') })) { throw "Package checksum manifest is missing $path." }
    }
    if ($manifest | Where-Object { $_ -match '  (data/)?preferences\.ini$|  data/queue\.sqlite$|  SHA256SUMS\.txt$' }) {
        throw "Mutable user data and the manifest itself must be excluded from the package checksum list."
    }

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
