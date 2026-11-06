# Windows releases and distribution

RifeLove is a local-first Windows desktop application. Video input, output, model inference, settings, task history and logs remain on the user's computer. The release and model download channels distribute application and model files only; they do not receive user videos or task data.

## Public distribution channels

| Content | Channel | Purpose |
| --- | --- | --- |
| Windows application | GitHub Releases | Versioned official Release with the portable ZIP and SHA-256 sidecar |
| RIFE models | GitHub Releases | Primary model archive and distribution manifest |
| RIFE models | Cloudflare Pages | Independent HTTPS mirror and signed model catalog |
| Release history | Cloudflare Pages | Public update log pointing to verified GitHub Release assets |

The current model channel contract requires two to five distinct HTTPS hosts, including the configured Cloudflare Pages host. The signed catalog, distribution manifest, archive size and SHA-256 must agree before the application can offer a model download. GitHub Release links use versioned release tags, while the release object carries the user-facing notes and downloadable assets.

## Application release procedure

The repository release metadata is authoritative for readiness. A package cannot be published unless its metadata sets `releaseReady` to `true` after all Windows distribution checks pass. Current historical preview entries remain non-downloadable until a real package qualifies.

Build and inspect a candidate first:

```powershell
npm --prefix release-site run publish:release -- --archive <path-to-RifeLove-version-win-x64.zip> --release <path-to-reviewed-release.json> --dry-run
```

The dry run validates package privacy, release metadata, the exact archive name, its SHA-256, the release manifest and GitHub's per-asset size limit. It does not contact GitHub or change the local release log.

After qualification, run the same command without `--dry-run` with `GH_TOKEN` or `GITHUB_TOKEN` set to a credential that can write repository contents. The publisher:

1. Creates or resumes a draft GitHub Release for `v<version>`.
2. Uploads the Windows ZIP and a SHA-256 sidecar without replacing existing assets.
3. Confirms the GitHub API asset digests and sizes, publishes the Release, then checks anonymous HTTPS availability and size.
4. Updates `release-site/releases.json` only after those checks pass. A failed local manifest write can be safely retried; a published asset with different contents is rejected.

Each GitHub release asset must be smaller than 2 GiB. Older published releases remain available; the publisher does not prune or overwrite their assets. The user-facing Release is a published GitHub Release object with notes and assets, not only a Git tag.

## Update log deployment

`npm --prefix release-site run deploy` builds the current update log before deploying the generated `release-site/dist` directory to the configured Cloudflare Pages project. This action is separate from publishing a GitHub Release. Review the manifest and rendered page before deploying. Wrangler must already be authenticated for the intended Cloudflare account.

## Release gates

Do not set `releaseReady` to `true` or publish a public application package until the exact candidate passes these checks:

- Complete package inventory with source, version and redistribution terms for every DLL and bundled runtime.
- Review of NVIDIA/CUDA/cuDNN/TensorRT terms, FFmpeg codec patent exposure, and Microsoft runtime redistribution requirements.
- Signed installer and portable packages, with clean-machine installation, model acquisition, GPU inference and export, update/rollback, uninstall, security and supported-device checks.
- Native Win32 user-interface acceptance at supported DPI and window sizes. Qt and a bundled CLI are outside the shipped product boundary.
- Verified model provenance and redistribution terms, signed catalog, and exact anonymous size/SHA-256 readbacks from both model hosts.
- A realistic real-footage quality review across supported models and representative motion, scene cuts, animation, film grain and low-light footage.

The current RIFE-only and CUDA-only outputs are qualification candidates. Synthetic throughput or a successful local inference run does not establish clean-machine installation, legal redistribution, broad hardware compatibility or real-footage quality.

## Local cleanup

Generated package cleanup scripts remain available for local work. They are separate from release publishing and do not remove model datasets, user task state or source files:

```powershell
.\tools\release\dedupe-local-builds.ps1
.\tools\release\dedupe-local-builds.ps1 -Apply
.\tools\release\prune-generated-builds.ps1
.\tools\release\prune-generated-builds.ps1 -Apply
```
