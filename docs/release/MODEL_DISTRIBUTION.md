# Model package distribution

## Current state

There is not yet a public RifeLove application release or a public RifeLove
model download. The native Model Center can verify packs supplied by the user,
but it does not download files. Do not treat an old development-preview entry
or a source-checkpoint URL as a published, ready-to-use RifeLove model.

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
validation report. It does not claim HDR support, a universal quality ranking,
or author authentication. Small-profile numerical checks and a local SDR video
pipeline run are not broad perceptual acceptance.

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

The product goal requires multiple independent, verified download channels.
Until the package is available through those channels and the installed
application can guide users through download, verification, and registration,
model downloads and the public application release remain incomplete.
