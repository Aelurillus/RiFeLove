# 本地参考帧画质评估

`evaluate_reference.py` 对比三份本地视频：低帧率原片、同一内容的高帧率参考视频，以及 RifeLove 输出。它先校验视频尺寸、像素格式、明确的 SDR 色彩标签、逐帧 PTS 和目标帧率时间网格，再用本机 FFmpeg 计算 PSNR 与 SSIM。报告按实际输出 PTS 与最近原片 PTS 的距离分类；距离不超过半个输出容器刻度的帧计入源帧组，兼容 Matroska 等容器的时间戳舍入。这样可避免把同一源帧误归到插值样本组。

## 输入要求

- 原片、参考视频和 RifeLove 输出必须是三个独立文件，尺寸、像素格式及 SDR 色彩标记一致。
- 参考视频和 RifeLove 输出必须具有完全相同的帧数与逐帧 PTS；结果 PTS 还必须与原片起点和目标帧率组成的输出网格匹配。
- 通过 `--fps` 指定 RifeLove 的目标帧率；结果 PTS 必须落在该时间网格的半个输出容器刻度范围内。
- 参考视频需要来自同一画面内容的真实高帧率录制或可信的合成真值。普通低帧率素材不能自行充当真值。
- 当前评估限定于标记明确且匹配的 SDR 色彩路径。HDR、未知色彩标记和需要色彩变换的视频会被拒绝。

## 合成真值示例

先创建 60 FPS 真值，再按相同时间轴抽取 30 FPS 原片：

```powershell
ffmpeg -f lavfi -i "testsrc2=size=320x180:rate=60:duration=2" `
  -vf "format=yuv420p,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited" `
  -fps_mode cfr -c:v ffv1 reference-60.mkv

ffmpeg -i reference-60.mkv `
  -vf "fps=30,format=yuv420p,setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=limited" `
  -fps_mode cfr -c:v ffv1 source-30.mkv
```

用本机 RifeLove CLI 生成与真值同帧率、同时间轴的无损结果：

```powershell
rifelove-cli.exe interpolate source-30.mkv rife-60.mkv `
  --fps 60 --model-dir .\models\rife-4.25-fp32-sdr8 `
  --runtime-dir .\runtime --encoder ffv1
```

运行评估：

```powershell
py -3 tools/quality/evaluate_reference.py `
  --source source-30.mkv --reference reference-60.mkv --result rife-60.mkv `
  --fps 60 `
  --ffmpeg C:/path/to/ffmpeg.exe --ffprobe C:/path/to/ffprobe.exe `
  --report quality-report.json
```

报告记录三个输入文件的 SHA-256、色彩配置、帧数、目标网格误差、与输出时间戳匹配的原片帧数量和非源采样数量，以及全部帧、源帧和非源采样各自的 PSNR/SSIM 均值、中位数、最小值和最大值。完全匹配帧的 PSNR 为无穷大，会作为 perfect frame 计数，不参与有限数值的均值。非源采样可能包括质量保护策略保持的原帧，所以不能直接视为 RIFE 推理帧数。

合成移动方块只用于检查评估链路和已知运动场景，不能代表实拍、动画、游戏录屏或 HDR 的画质验收。PSNR/SSIM 也不能替代盲测与时间一致性人工复核。所有输入、计算和报告都留在本机。
