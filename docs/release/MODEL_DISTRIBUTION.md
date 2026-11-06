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

`tools/model_catalog/Sign-ModelCatalog.ps1` validates the catalog shape and
channel hosts, signs the exact payload bytes, and verifies the signature
locally before writing the envelope. A version-1 catalog has one recommended
RIFE entry, a SHA-256 and byte-size pin for its distribution manifest, and at
least two HTTPS archive channels on distinct hosts. The app fetches the
catalog only after the user chooses “安装官方推荐模型”; it then verifies the
catalog, downloads the pinned manifest, checks the entry identity, and uses
the existing archive and model-pack verifier. The Cloudflare Pages and GitHub
Raw catalog URLs are compiled as fallback sources. Publishing the signed file
and reading the same bytes back anonymously are still release gates.
