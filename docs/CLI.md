# Local command line workflows

The CLI runs locally and does not download models or contact a service. `rifelove-cli profile-check FILE.json` validates version 1, 2 and 3 profiles exported from RifeLove Studio and prints its local paths and processing settings as one JSON object. Version 1 profiles default to automatic inference-backend selection and a scene-cut threshold of 0.25; version 2 profiles retain their saved inference backend and default the threshold to 0.25.

Use a validated desktop profile as the starting configuration for direct processing or a queued job:

```text
rifelove-cli interpolate INPUT.mp4 OUTPUT.mkv --profile PROFILE.json --backend auto --progress --report REPORT.json
rifelove-cli queue-add QUEUE.sqlite INPUT.mp4 OUTPUT.mkv --profile PROFILE.json --backend cuda
rifelove-cli queue-run QUEUE.sqlite --progress
```

The input and output paths stay explicit in the command. The profile supplies the frame rate, local model folder, ONNX Runtime folder, inference backend, scene-cut threshold, encoder, encoding quality and GOP size. Its saved output location is shown by `profile-check` but is not used as a command output path. The output file extension must match the profile container (`.mkv`, `.mp4` or `.mov`).

`--backend` accepts `auto`, `cuda` or `tensorrt`. `auto` prefers TensorRT and falls back to ONNX Runtime CUDA, `cuda` uses only ONNX Runtime CUDA, and `tensorrt` fails if TensorRT cannot initialize. `--cut-threshold` overrides the profile scene-cut threshold. The inference choice is stored in queued tasks and checkpoint identity. Processing precision remains FP32 because FP16 has not passed the model numerical and visual qualification gates.

Put an explicit option after `--profile` to override its value. For example, append `--fps 24000/1001 --encoder libopenh264 --bitrate 6000000` to select a different output rate and bitrate. Changing encoder or bitrate clears the profile's mutually exclusive CQ/QP/CRF options. A local profile can refer only to fully encoded `file:///` paths; network URLs and UNC shares are rejected.

Profile validation checks the file format, values, paths and supported setting combinations. It does not verify that a model folder contains valid weights, that the runtime can load CUDA, or that a chosen encoder is available. Those checks occur when the corresponding processing job starts.
