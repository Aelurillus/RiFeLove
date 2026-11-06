# Model package distribution

## Current state

There is not yet a public RifeLove application release or a public RifeLove
model download. The native Model Center now has a signed-catalog install path,
but no signed catalog or live public model endpoint has been deployed. Until
those endpoints pass anonymous read-back checks, the official-model action
fails closed. The manual HTTPS workflow remains available; its adjacent
distribution manifest checks file integrity but does not authenticate the
publisher. Do not treat an old development-preview entry or a source-checkpoint
URL as a published, ready-to-use RifeLove model.

The first distributable candidate is the verified RIFE 4.25 FP32 SDR8 model
pack. Build its local review artifact with:

```powershell
.\packaging\windows\package-model-local.ps1 `
  -ModelDirectory .\models\cache\rife-4.25-fp32-sdr8-v3
```

The script accepts only the exact RIFE 4.25 schema-v2 pack and requires its
model, license, source-manifest, and validation-report hashes to match
`model.json`. It rejects additional files and reparse points. It writes a ZIP
and a sidecar distribution manifest under `out/model-distribution`; it does
not upload or publish either file.

## Provenance and scope

The upstream author lists Google Drive and Baidu download links for RIFE 4.25,
states that linked trained-model contents are covered by the project's MIT
license, and recommends 4.25 as the default for most scenes. The source links
provide the original checkpoint and Python model source, not a RifeLove-ready
ONNX pack. See the [upstream model list](https://github.com/hzwer/Practical-RIFE#trained-model)
and its [pinned MIT license](https://github.com/hzwer/Practical-RIFE/blob/bbfd2ea90910789a860ea3e2b32a240cd577b75e/LICENSE).

The local ONNX SHA-256 is `8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3`.
It is the RifeLove integrity pin for the model graph, not an upstream author
signature; the author did not publish an authoritative checkpoint checksum.
The pack includes the license, pinned source manifest, and SDR8 numerical
validation report. That report records six small-profile PyTorch CPU and ONNX
Runtime CPU cases. Separate repository records cover local CUDA/TensorRT FP32
checks and a tagged SDR video pipeline run; they are not included in the ZIP.
These checks do not claim HDR support, a universal quality ranking, or author
authentication, and they are not broad perceptual acceptance. See the [local
model selection record](../development/LOCAL_MODEL_SELECTION.md) for the
additional hardware and video evidence.

## Download-channel acceptance

Before advertising a channel, publish this exact package to the host and then
download it back anonymously. Verify HTTP status, byte count, and the complete
archive SHA-256 against the sidecar manifest. For individual-file hosting,
verify every path and SHA-256, then reconstruct and validate the model pack
with the native pack verifier. Do not silently substitute a model with the
same display name or upstream version but a different graph hash.

The candidate Hugging Face RIFE 4.25 ONNX file checked during this review has a
different SHA-256 and byte count from the local qualified graph; it is not a
mirror of this package. It remains an unreviewed alternative, not a download
channel. The official Google Drive and Baidu checkpoint links remain useful
for model developers, but are not directly importable by the desktop app.

The Model Center checks archive and declared-file hashes, extracts only the
manifest allowlist into a temporary directory, runs the existing schema-v2
model verifier, and moves the verified pack into the managed user model
directory before registration. The selected sidecar supplies the integrity
pins; its hashes do not authenticate the publisher.

The pinned upstream Practical-RIFE README explicitly places its linked trained
model files under the project's MIT license, and this package carries that
pinned license and source manifest. This resolves the model-specific license
scope question for the RIFE 4.25 pack; the missing upstream checkpoint checksum
remains a provenance limit, not a different license grant.

The client now includes the signed-catalog verifier and publisher trust root.
The signed catalog still needs to be published to both fallback locations,
with at least two independent public model mirrors and anonymous read-back
checks proving each serves the same package bytes. The direct downloader can
fetch a user-selected URL and resume an interrupted archive, but its adjacent
sidecar does not authenticate the publisher. No RifeLove model channel is
published yet. The application release remains closed pending the exact
runtime, codec, clean-machine, and installer qualification gates in
`WINDOWS_REDISTRIBUTION_AUDIT.md`.

## Signed catalog v1

The native client verifies a detached ECDSA P-256/SHA-256 signature before it
parses any catalog JSON. The trusted public point is compiled into the native
package; the signing private key stays in the publisher's current-user
certificate store and is never exported into the repository. The envelope is
UTF-8 text with this exact four-line structure:

```text
RIFELOVE MODEL CATALOG 1
key-id=rifelove-model-catalog-2026-v1
payload=<base64 of the exact UTF-8 JSON bytes>
signature=<base64 of the 64-byte IEEE P1363 signature>
```

`tools/model_catalog/Sign-ModelCatalog.ps1` requires the exact finalized
distribution manifest and a fresh anonymous read-back receipt, validates that
their package identity and channels match the catalog, signs the exact payload
bytes, confirms that the publisher certificate's public point matches the
trust root compiled into the client, and verifies the signature locally before
writing the envelope. A version-1 catalog has one recommended RIFE entry, a
SHA-256 and byte-size pin for its distribution manifest, and at
least two HTTPS archive channels on distinct hosts. The app fetches the
catalog only after the user chooses “安装官方推荐模型”; it then verifies the
catalog, downloads the pinned manifest, checks the entry identity, and uses
the existing archive and model-pack verifier. The Cloudflare Pages and GitHub
Raw catalog URLs are compiled as fallback sources. Publishing the signed file
and reading the same bytes back anonymously are still release gates.

## Cloudflare Pages model mirror

The model mirror uses the existing `rifelove-updatelog` Pages project at the
paths already bound into the client. A model-only deployment would replace the
update-log site, so `tools/release/build-site.mjs` accepts an external staging
directory and builds the update-log files together with the verified model
assets. Normal `npm run build` remains update-log-only unless
`RIFELOVE_MODEL_PAGES_STAGE` is set.

Cloudflare Pages limits an individual site asset to 25 MiB. The current model
ZIP is 21,121,121 bytes; the builder checks this limit and the archive hash
before it copies anything into `release-site/dist`. Use a different large-file
host for a future model package that exceeds that limit.

After the model has passed the release gates above, first publish the ZIP to
the Pages channel so its stable URL can be anonymously verified. Keep the
candidate manifest local during this bootstrap deployment:

```powershell
$release = '.\out\model-distribution\public-candidate'
$bootstrap = Join-Path $release 'pages-bootstrap'
New-Item -ItemType Directory -Force "$bootstrap\model-distribution" | Out-Null
Copy-Item '.\out\model-distribution\RifeLove-RIFE-4.25-FP32-SDR8-v3.zip' `
  "$bootstrap\model-distribution\RifeLove-RIFE-4.25-FP32-SDR8-v3.zip"
Copy-Item '.\out\model-distribution\RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json' `
  "$bootstrap\model-distribution\RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json"
$env:RIFELOVE_MODEL_PAGES_STAGE = (Resolve-Path $bootstrap).Path
$env:RIFELOVE_MODEL_PAGES_MODE = 'archive-bootstrap'
npm run --prefix release-site build
npm run --prefix release-site deploy
Remove-Item Env:RIFELOVE_MODEL_PAGES_STAGE
Remove-Item Env:RIFELOVE_MODEL_PAGES_MODE
```

The bootstrap mode accepts only a hash-matched local RIFE candidate and places
only its ZIP at the public path; it does not copy the local manifest or enable
the signed-catalog install action. The full-channel build accepts only the
finalized manifest and a catalog envelope that verifies against the native
trust root and matches that manifest. Stage the final tree as follows after
channel read-backs and catalog signing:

```text
<stage>/
  model-distribution/
    RifeLove-RIFE-4.25-FP32-SDR8-v3.zip
    RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json
  model-catalog/
    v1/catalog.signed.txt
```

Then set `RIFELOVE_MODEL_PAGES_STAGE` to `<stage>`, set
`RIFELOVE_MODEL_PAGES_MODE` to `final`, and run the same build and deploy
commands. The final build also checks that two to five distinct HTTPS hosts are
declared, the Pages URLs match these exact paths, and the signed catalog pins
the staged archive and sidecar. It retains the update-log page and adds
immutable caching for versioned model files plus short caching for the catalog.
After deployment, run the anonymous read-back tools in the procedure below.

## Repeatable publication procedure

The publication scripts are intentionally separate from account login and file
upload. Their network operations are anonymous HTTPS GETs; they never read a
publisher credential. Keep the channel file, generated read-back receipts, and
the signing key outside the source repository.

1. Upload the exact ZIP from `out/model-distribution` to two durable HTTPS
   hosts. Do not use expiring query-string links. Prepare a local `channels.json`
   with each host's public archive and `.distribution.json` URLs:

   ```json
   [
     {
       "name": "GitHub Releases",
       "archiveUrl": "https://github.com/Aelurillus/RiFeLove/releases/download/<release>/RifeLove-RIFE-4.25-FP32-SDR8-v3.zip",
       "manifestUrl": "https://github.com/Aelurillus/RiFeLove/releases/download/<release>/RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json"
     },
     {
       "name": "Cloudflare Pages",
       "archiveUrl": "https://rifelove-updatelog.pages.dev/model-distribution/RifeLove-RIFE-4.25-FP32-SDR8-v3.zip",
       "manifestUrl": "https://rifelove-updatelog.pages.dev/model-distribution/RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json"
     }
   ]
   ```

2. Have the tool anonymously download each hosted ZIP, stream its SHA-256,
   compare the full byte count and hash to the local candidate, and only then
   write a separate finalized sidecar with state `archive-channels-verified`.
   That state means the archive read-backs passed; it does not claim the sidecar
   or catalog has been published. The tool refuses to overwrite outputs:

   ```powershell
   $release = '.\out\model-distribution\public-candidate'
   .\tools\model_catalog\Set-ModelReleaseChannels.ps1 `
     -DistributionManifestPath '.\out\model-distribution\RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json' `
     -ArchivePath '.\out\model-distribution\RifeLove-RIFE-4.25-FP32-SDR8-v3.zip' `
     -ChannelsPath "$release\channels.json" `
     -OutputManifestPath "$release\RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json"
   ```

3. Upload that exact finalized sidecar to every `manifestUrl`. Then verify
   anonymous read-backs of both the ZIP and sidecar from every host and build
   the native catalog payload plus a local audit receipt:

   ```powershell
   .\tools\model_catalog\New-ModelCatalogPayload.ps1 `
     -DistributionManifestPath "$release\RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json" `
     -ArchivePath '.\out\model-distribution\RifeLove-RIFE-4.25-FP32-SDR8-v3.zip' `
     -OutputPayloadPath "$release\catalog.payload.json" `
     -ReadbackReceiptPath "$release\model-readback.json"
   .\tools\model_catalog\Sign-ModelCatalog.ps1 `
     -PayloadPath "$release\catalog.payload.json" `
     -DistributionManifestPath "$release\RifeLove-RIFE-4.25-FP32-SDR8-v3.distribution.json" `
     -ReadbackReceiptPath "$release\model-readback.json" `
     -OutputPath "$release\catalog.signed.txt"
   ```

4. Publish the signed envelope to both catalog fallback paths already compiled
   into the native client. Verify both public URLs return the exact local
   envelope bytes before enabling the official install action in a release:

   ```powershell
   .\tools\model_catalog\Confirm-ModelCatalogPublication.ps1 `
     -EnvelopePath "$release\catalog.signed.txt" `
     -ReadbackReceiptPath "$release\catalog-readback.json"
   ```

`Set-ModelReleaseChannels.ps1` proves only the anonymous archive read-backs at
the time it runs. `New-ModelCatalogPayload.ps1` independently checks both
archive and finalized-manifest bytes from every channel immediately before
payload generation. The final catalog confirmation checks both client fallback
URLs. A successful local script run does not upload files, approve third-party
runtime redistribution, or qualify the Windows application release; those
remain separate gates in `WINDOWS_REDISTRIBUTION_AUDIT.md`.
