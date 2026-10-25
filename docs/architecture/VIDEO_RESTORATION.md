# Video restoration and enhancement

Status: temporal request/result contract and local restoration-pack metadata verifier implemented; no restoration inference backend or model has passed product qualification.

RifeLove treats frame interpolation and video restoration as separate AI stages. RIFE predicts frames at new presentation times. Restoration models may denoise, deblur, reduce compression artifacts, or increase spatial resolution across one or more existing frames. A two-frame interpolation request is not an adequate contract for those jobs.

The initial C++ boundary is in `src/restoration/video_restoration_backend.hpp` and `.cpp`. Its versioned registry and validators cover model identity, task, stage placement, canonical SDR profile tags, ordered temporal context, scene/clip-edge context, preserved center PTS/duration/color metadata and spatial scale. Seven focused tests exercise this contract.

The local pack reader in `src/restoration/model_pack.hpp` and `.cpp` accepts a declarative `video-restoration` schema-v1 `model.json`. It verifies the ONNX file through streaming SHA-256, confines every referenced sidecar to the pack directory, and checks hash-bound license text, source manifest, validation report, adapter identity, declared tasks/placement/profile/window/scale and passed report cases. An adapter is selected from application-owned code; packs cannot include executable preprocessing. These checks establish local file consistency and matching declarations, not author authenticity or perceived quality. Four pack-verifier tests cover valid packs, changed data, capability mismatches, unsafe paths and invalid scale; a separate bounded streaming-hash test covers the graph file reader. The offline Model Center persists a separate restoration-pack catalog, shows declared tasks/window/scale, rejects unsupported families, and unregisters paths without deleting the local files. No concrete inference backend, model manager download/qualification flow, TensorRT path or pipeline stage exists yet; importing a pack does not process a video.

## Product rules

- Processing remains offline by default. Source video, model selection, settings, temporary frames, reports, and task history stay on the user's computer.
- Restoration is optional and non-destructive. It writes to the selected output; it never replaces the source.
- A model cannot be called “best” from its name, paper result, or single synthetic benchmark. RifeLove must qualify each model for its task, content type, resolution, color profile, runtime and target GPU.
- Temporal stability is a release requirement. A model that improves single-frame sharpness while producing flicker, crawling texture, unstable text/HUD, or scene-cut contamination is not enabled as a video-restoration preset.
- A model pack contains declarative metadata and weights only. It cannot load or register executable code.
- No model weights are copied into Git. Downloads/imports are local, hash-checked, and accompanied by source, version, license scope and redistribution evidence.

## Proposed architecture

Continue the dedicated restoration interface rather than extending `IInterpolationBackend` or `IModelAdapter` with unrelated fields. The full product path still needs to represent:

- Restoration tasks: denoising, deblurring, compression cleanup and super-resolution.
- A temporal context window, including the center frame and exact rational presentation timestamps.
- The supported position relative to interpolation: source-rate frames before RIFE, or output-rate frames after RIFE. A model declares the positions it actually supports; the pipeline never guesses.
- Input and output color profiles, bit depth, spatial scale, dimension constraints, memory estimates and cancellation behavior.
- An owned output frame with its original presentation time preserved. Frame-count and audio timing stay under the existing timeline and muxing contracts.

The application service selects and validates the local model before starting a job. The scheduler applies bounded decode/inference/encode backpressure and never holds an unbounded video window in memory. Scene boundaries reset temporal context. Results retain model ID, adapter version, weight hash, backend, precision and settings in the local job identity and report.

## Qualification gate

Every candidate needs all of the following before a preset is offered:

1. Separate review of source-code and weight licenses, training-data restrictions where stated, attribution, and redistribution rights. A code license alone is not evidence that its weights can be bundled.
2. Verified source provenance and SHA-256 for every weight and sidecar.
3. Numerical comparison against paired ground truth where available, using task-appropriate metrics and exact PTS alignment.
4. Temporal review on real clips: camera motion, occlusion, scene cuts, grain, animation, fine text/HUD, compression damage, and low-light noise. Include frame-difference/flicker diagnostics and blind visual review.
5. Runtime and VRAM measurements on supported NVIDIA hardware, including the GTX 1660 SUPER development device, representative 1080p/4K input, cancellation, OOM handling, and software/backend failure behavior.
6. End-to-end tests proving duration, output cadence, source audio/subtitle policy, color tags, bit depth, and non-destructive publication.

No universal quality ranking is claimed. Fast/Balanced/Quality presets must be selected from measured task-specific evidence and remain clearly labeled by their tested content and hardware envelope.

## Candidate notes

- [RVRT](https://github.com/JingyunLiang/RVRT) is a temporal video-restoration research candidate for super-resolution, deblurring and denoising. Its official repository states that the project is released under CC-BY-NC. It is excluded from a redistributable default package unless the project obtains compatible rights.
- [VRT](https://github.com/JingyunLiang/VRT) is another temporal restoration research candidate. Its official repository also states CC-BY-NC terms, so it has the same distribution gate.
- [BasicVSR++](https://github.com/ckkelvinchan/BasicVSR_PlusPlus) is a temporal video super-resolution/restoration candidate, and its official source repository is Apache-2.0. Pretrained weights are linked as separate downloads without an explicit weight-redistribution statement in the repository README; therefore the weights stay out of RifeLove packages until their rights and training-data terms are resolved.
- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) has BSD-3-Clause source code and offers video inference tooling, but its restoration network is image-oriented. Framewise use is only a spatial candidate: RifeLove must independently establish temporal consistency, weight rights, color behavior and end-to-end performance before offering it for video.

These are research notes, not a selected model or an endorsement that any candidate is universally superior. A future permissively licensed or project-trained temporal model can replace them after it passes the same gates.
