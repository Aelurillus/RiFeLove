# NAFNet deblur: recorded-video proxy experiment

This local experiment evaluates one candidate, not a release acceptance test or
a comparison proving that it is the best model. The decoded RGB frames are the
reference, not pristine camera originals. The source was the first three frames
of `work/temporal-restoration/realbasicvsr/data/demo_001.mp4`, scaled from
480x360 to 384x288. Its original color tags were unknown; this is not an HDR or
color-management validation. A nine-pixel horizontal box blur was applied to
create an artificial test case. The clean case was also processed to see whether
the model changes already clear material.

| Compared against decoded proxy | Blurred input | NAFNet output |
| --- | ---: | ---: |
| PSNR (dB) | 22.465 | 23.461 |
| Horizontal gradient error (levels) | 8.829 | 7.654 |
| Consecutive-frame error difference RMSE (levels) | 17.288 | 15.173 |

On the clean input, the output had 30.705 dB PSNR and 7.435 levels RMSE
against the decoded proxy. The network therefore alters clean detail too. The
frame-to-frame error metric is not motion-compensated and does not establish
absence of flicker. Three frames and one synthetic blur cannot establish quality
on real motion blur, compression damage, animation, or diverse footage.

The source SHA-256 was
`d45fef592cb4d9397eee17357599a0cb06f1acc46fc35d363ad45c5958edc097`;
the NAFNet graph SHA-256 was
`b99b39f0bdf29f15205aa50dd23b7a14b571b0c7ae9576ba2f9417fccad4f056`.
ONNX Runtime reported both CUDA and CPU provider events (4,075 and 765 per
profile respectively), so this is mixed-provider execution. Each frame used a
fresh process and model session: observed model times of approximately
2.5–2.7 s and total times of 8.5–9.4 s per frame do not measure application
throughput with a reused session.

Reproduce locally with the same inputs and verified model pack, using a fresh
output directory:

```powershell
work/model-tools/Scripts/python.exe tools/validation/nafnet_video_quality.py --video work/temporal-restoration/realbasicvsr/data/demo_001.mp4 --pack models/cache/nafnet-gopro-deblur-sdr8-fp32-eval --runtime out/RifeLoveNative-local-20261031-r25/runtime --validator build/windows-release/rifelove-validate-restoration.exe --ffmpeg work/codec-ffmpeg-build-x86asm/ffmpeg-install/bin/ffmpeg.exe --output work/verification/nafnet-recorded-blur-new --frames 3 --provider cuda
```

The tool checks that the graph matches the model descriptor, refuses to
overwrite an existing evidence directory, and does not upload video or models.
Further work needs representative clips, real and controlled degradations,
motion-aware temporal assessment, visual review, and performance measurements
in the actual pipeline before selecting default behavior. Image deblurring alone
does not establish general temporal detail restoration.

## Experimental automatic soft-edge estimate

The independent `sdr8-directional-soft-edge-v1` prototype detects the synthetic
directional checkerboard in its unit test while bypassing clean and isotropically
blurred checkerboards. That test does not establish real-world reliability.
On the three decoded proxy frames above and the same frames with nine-pixel
horizontal blur, the prototype returned zero strength and zero confidence on all
six inputs (25,993 sample positions per frame). It therefore failed to detect
this artificially blurred recorded material. An intentional soft edge can also
look like a blurred edge; the confidence is a heuristic, not a probability that
the material should be restored. The estimator remains experimental and is not
connected to desktop processing or enabled by default.

To reproduce the estimate, first generate the proxy files using the earlier
command, build the developer-only `rifelove-validate-detail-estimate` target,
then run it for each `clean-*-input.rgb` and `blurred-*-input.rgb` file with
width `384` and height `288`. The tool reads local RGB24 and reports the raw
estimate; it does not execute a neural model or upload material. Representative
real footage, deliberately soft material, motion direction and temporal behavior
must be evaluated before designing any user-facing automatic control.
