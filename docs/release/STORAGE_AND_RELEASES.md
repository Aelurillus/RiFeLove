# Release storage and update log

RifeLove is currently operated in **local-only mode**. Video processing, models, settings, task databases, logs, checkpoints, release archives and update metadata stay on the local filesystem. The application does not upload source videos or task data.

## Current storage policy

- Aliyun OSS is disabled for this checkout. There is no OSS bucket, region or endpoint default in `release-site/storage.json`.
- The release scripts fail closed before loading credentials or constructing a network client. Environment variables cannot re-enable Aliyun publishing locally.
- Storj is retained only as a future, separately reviewed major-release backup option. It is disabled in `release-site/storage.json`, has no configured bucket, and is not contacted by the local workflow.
- `release-site/releases.json` remains a local metadata/history file. `release-site/dist` and the worker bundle are generated local artifacts; building them does not publish anything.
- No cloud credentials are stored in the repository. A workspace scan found no configured access-key or secret-key values.

The authoritative local setting is:

```json
{
  "mode": "local-only",
  "primary": null,
  "backup": {
    "provider": "storj",
    "enabled": false,
    "endpoint": "",
    "storjBucket": ""
  }
}
```

## Local update-log workflow

From `release-site`, the following commands only build and test local files:

```powershell
npm ci
npm test
npm run build
npm run build:worker
```

The static site reads the local `releases.json`. A generated worker source is written under `out/release-worker/worker.js`; deployment is a separate action and is outside the local-only workflow.

The following legacy commands are deliberately disabled and fail before any storage client or credential is read:

```powershell
npm run publish
npm run upload:preview:oss
npm run configure:oss-updatelog
npm run deploy:oss
```

They report that Aliyun OSS publishing is disabled in local-only mode. The direct entry points under `tools/release/` contain the same guard for stale scripts or shells.

## Local release archive procedure

1. Build a Windows package with real local copies of the runtime DLLs and selected model files.
2. Keep the archive under `out/release-candidates/<version>/` and retain its SHA-256, byte count and package manifest locally.
3. Run the release archive safety checks, CTest and the packaged smoke tests before marking metadata as ready.
4. Update `release-site/releases.json` only with reviewed local metadata. Keep `downloadAvailable` false until a future distribution workflow is explicitly enabled and reviewed.
5. Do not place user settings, task history, logs or videos in a release archive.

Local cleanup tools remain available for generated packages:

```powershell
.\tools\release\dedupe-local-builds.ps1
.\tools\release\dedupe-local-builds.ps1 -Apply
.\tools\release\prune-generated-builds.ps1
.\tools\release\prune-generated-builds.ps1 -Apply
```

They operate on generated `out` packages and preserve package `data/` directories, user files and local task state. No cloud verification is a prerequisite for local cleanup.

## Public repository boundary

The public source contains the current local-only policy and fail-closed upload guards. It excludes private storage identifiers, object keys, archive hashes and private upload records. Storj stays disabled in the local configuration until a separate release workflow is explicitly reviewed.

## Verification performed for this change

- `release-site/storage.json` has no Aliyun provider, bucket, region or endpoint.
- The local release policy no longer describes an OSS retention budget or automatic remote backup.
- All four legacy Aliyun package commands fail closed through `tools/release/oss-disabled.mjs`.
- Repository scans found no literal cloud access keys or secret values in tracked project files.
- No Aliyun OSS, Storj or other cloud endpoint was contacted.
