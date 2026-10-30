# 本地端到端性能基准

`benchmark_pipeline.py` 在同一台 Windows NVIDIA 电脑上重复运行 RifeLove CLI，记录完整处理速度、实际推理后端、实际编码器和回退原因。它只调用本地 CLI 与本地 FFprobe，不下载文件，也不修改源视频。

## 运行

先按[本地桌面构建指南](../../docs/development/DESKTOP.md)配置 FFmpeg、ONNX Runtime CUDA 和 TensorRT DLL 的 `PATH`，再运行：

```powershell
py -3 tools/benchmarking/benchmark_pipeline.py `
  --cli out/RifeLove-local/rifelove-cli.exe `
  --ffprobe C:/path/to/ffprobe.exe `
  --source C:/videos/sample.mkv `
  --model-dir out/RifeLove-local/models/rife-4.25-fp32-sdr8 `
  --runtime-dir out/RifeLove-local/runtime `
  --fps 60 `
  --output-dir build/benchmarks/gtx1660s-1080p-60fps `
  --warmups 1 --runs 3 --encoder auto
```

可以重复传入 `--source` 和 `--fps`，例如 `--fps 60 --fps 120`，形成多素材、多目标帧率矩阵。帧率支持有理数，例如 `--fps 60000/1001`。每次运行使用唯一的临时输出；成功并通过 FFprobe 帧数校验后会删除视频，除非设置 `--keep-videos`。失败时会保留诊断文件。

`--output-dir` 必须是新目录或空目录。程序会在其中逐次原子写入 `benchmark.json`，并保留 CLI 详细报告。此文件包含本机路径、硬件与软件版本、输入文件 SHA-256、所用模型哈希、每次运行的墙钟时间和吞吐、各阶段耗时、实际编码器、回退原因、输出帧数与 FFprobe 验证结果。输入源处理前后都会重新计算哈希。

## 如何读结果

- `aggregates[].endToEndFps` 汇总不含预热的端到端输出帧速度；`pipelineFps` 是流水线报告中的速度。
- `runs[].actualEncoder` 说明最终输出使用的编码器。若 NVENC 不能初始化，`encoderFallback` 会说明回退原因；软件编码不影响 RIFE 继续使用 CUDA 推理。
- `inferenceBackend` 与 `inferenceFallback` 记录 TensorRT / ONNX Runtime CUDA 的实际选择。
- 把运行结果和 `environment`、`configuration` 一起比较；不同 GPU、驱动、模型、分辨率、编码器或目标帧率不应直接当作同一基准。
- 第一次运行可能包含 CUDA/TensorRT 初始化成本；预热会单独记录，并从汇总统计排除。
- 此工具测量速度和结构校验，不衡量视觉质量。画质评估仍需要真实参考帧或独立的盲测流程。

所有输入、模型、运行库、CLI、FFprobe、临时视频和 JSON 报告均在本机处理。
