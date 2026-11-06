# Model package distribution

## Current state

There is not yet a public RifeLove application release or a public RifeLove
model download. The native Model Center can install a local ZIP or fetch a ZIP
from a user-supplied HTTPS URL with its adjacent distribution manifest. It can
try fallback mirrors and resume an interrupted archive transfer. It does not
fetch a built-in catalog or authenticate the manifest publisher. This direct
URL workflow is not a public RifeLove model channel. Do not treat an old
development-preview entry or a source-checkpoint URL as a published,
ready-to-use RifeLove model.

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

The product still needs a signed in-app catalog and publisher trust root, at
least two independent public mirrors, and anonymous read-back checks proving
each mirror serves the exact same package bytes. The direct downloader can
fetch a user-selected URL and resume an interrupted archive, but its adjacent
sidecar does not authenticate the publisher. No RifeLove model channel is
published yet. The application release remains closed pending the exact
runtime, codec, clean-machine, and installer qualification gates in
`WINDOWS_REDISTRIBUTION_AUDIT.md`.
