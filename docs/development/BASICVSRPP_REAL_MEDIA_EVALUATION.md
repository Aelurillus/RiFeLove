# BasicVSR++ real-media smoke evaluation

审计日期：2026-10-29

这份记录把本地 RealBasicVSR 演示目录中的 `demo_001.mp4` 作为真实视频样本，验证 BasicVSR++ NTIRE decompression track 1 是否能在现有隔离 CPU 参考环境中处理真实时序内容。它是研究证据，不是 RifeLove 产品验收，也不把该模型加入模型缓存、发行包或核心流水线。

## 样本关系与限制

`demo_001.mp4` 与同目录的 `demo_000/*.png` 不是一组对齐的输入/参考帧：

- MP4：480×360、H.264 High 4:4:4 Predictive、`yuv444p`、25 FPS、100 帧、4 秒。
- PNG：640×360、31 帧；首帧与 MP4 首帧是不同场景。

因此原始目录本身不能把 PNG 当作 MP4 的清晰真值。MP4 码率约 27.94 Mbps，且首帧 H.264 SEI 中的 x264 参数含有 `qp=0`；这与高质量或近无损编码相符，不能代表低码率社交媒体压缩。为获得可解释的真实场景对照，本轮用本地 `C:\Program Files (x86)\blur\lib\ffmpeg\ffmpeg.exe` 的 `libx264 -crf 35 -pix_fmt yuv420p` 将同一段 MP4 转成 148,395 字节、约 296.8 kbps 的压缩版本。原始 MP4 只作为高质量代理参考，不是相机母版。

## 可复现实验

工具：

- Python：`work/model-tools/Scripts/python.exe`，Python 3.11.17。
- PyTorch：2.8.0 CPU；torchvision：0.23.0+cpu。
- FFmpeg：`work/codec-ffmpeg-build/ffmpeg-install/bin/ffmpeg.exe`。
- 权重：`work/basicvsrpp-eval/basicvsr_plusplus_c128n25_ntire_decompress_track1_20210223-7b2eba02.pth`。
- 算子：`torchvision.ops.deform_conv2d` CPU 替代 MMCV 的 `ModulatedDeformConv2d`，仅用于研究参考。

命令（无参考样片）：

```powershell
$py = 'work\model-tools\Scripts\python.exe'
& $py tools\validation\evaluate_basicvsrpp_real_media.py --frames 5 --write-preview
```

脚本从 MP4 管道解码前 5 帧，不复制源视频；输入和输出预览仅写入：
`work/basicvsrpp-eval/real-media-eval/`。

受控真实场景压缩对照：

```powershell
$ff = 'C:\Program Files (x86)\blur\lib\ffmpeg\ffmpeg.exe'
$src = 'work\temporal-restoration\realbasicvsr\data\demo_001.mp4'
$compressed = 'work\basicvsrpp-eval\real-media-eval\demo_001_x264_crf35.mp4'
& $ff -y -hide_banner -loglevel error -i $src -map 0:v:0 `
  -vf 'format=yuv420p' -c:v libx264 -preset veryfast -crf 35 `
  -g 25 -bf 2 -an $compressed

$py = 'work\model-tools\Scripts\python.exe'
& $py tools\validation\evaluate_basicvsrpp_real_media.py `
  --video $compressed --reference-video $src --ffmpeg $ff `
  --ffprobe 'work\codec-ffmpeg-build\ffmpeg-install\bin\ffprobe.exe' `
  --frames 5 --output-dir 'work\basicvsrpp-eval\real-media-eval\x264-crf35' `
  --write-preview
```

## 实测结果

结果文件：`work/basicvsrpp-eval/real-media-eval/basicvsrpp-real-media-result.json`

| 项目 | 数值 |
| --- | ---: |
| 输入 / 输出形状 | `[1, 5, 3, 360, 480]` |
| CPU 推理总时间 | 23.0841 s |
| CPU 推理速度 | 4.6168 s/frame |
| 输出与输入平均绝对差 | 0.0172047（归一化 0–1） |
| 输入时序相邻帧平均绝对差 | 0.0694263 |
| 输出时序相邻帧平均绝对差 | 0.0672571 |
| 时序变化量差异的平均绝对值 | 0.0176304 |
| 输入 Laplacian 能量 | 0.0232469 |
| 输出 Laplacian 能量 | 0.0259434 |
| 输出/输入 Laplacian 能量比 | 1.11599 |

这些数字只说明：当前 CPU 替代算子路径能对真实视频产生同尺寸输出，输出改变了约 1.72% 的平均归一化像素绝对值，时序差分略低，边缘能量约增加 11.6%。边缘能量增加可能来自细节恢复，也可能来自过锐化或伪影；没有干净参考，不能判断哪一种。该样本也没有证明低码率压缩伪影被修复。

### 受控 CRF 35 对照

在真实街景上用原始高质量 MP4 作为代理参考、对 CRF 35 压缩版本的前 5 帧进行测试，得到：

| 指标 | 压缩输入 | BasicVSR++ 输出 | 差值 |
| --- | ---: | ---: | ---: |
| RGB PSNR（代理参考） | 24.8838 dB | 25.7166 dB | +0.8328 dB |
| Global SSIM（代理参考） | 0.975219 | 0.979698 | +0.0044789 |
| 相邻帧平均绝对差 | 0.0608176 | 0.0575518 | -0.0032658 |
| Laplacian 能量 | 0.0146722 | 0.0134402 | -8.40% |

逐帧 PSNR 变化为：`+0.7753 dB`、`+0.8737 dB`、`+0.8750 dB`、`+0.8817 dB`、`+0.7438 dB`；5 帧全部改善，但窗口很短。该结果支持“在这一个真实场景、这一档人为编码压缩和这一 CPU 替代算子路径上，有初步恢复信号”的判断，不能外推到所有视频或宣称达到 Topaz 水平。输出 Laplacian 能量下降也说明它偏向平滑压缩噪声；它不是无条件的锐化或细节增强。

曾短暂尝试使用项目 FFmpeg 的 `h264_nvenc` 生成同等样本，但解码结果出现整帧绿紫条纹，故该文件和对应数字均被判为无效，不进入任何质量结论。这个现象也说明发布前必须对每个编码后端做“编码后重新解码、帧校验、色彩和时间轴验证”，不能只依据编码器返回成功。

## 工程结论

BasicVSR++ track 1 值得保留为本地研究候选，理由是它确实是面向 NTIRE 视频解压缩任务的时序模型，且在 480×360、5 帧输入上能够完成研究性前向推理。但当前最值得实施的工作是建立可解释的评估集，而不是直接接入产品：

1. 选择一段有清晰源片的真实视频，使用 FFmpeg 生成 H.264/H.265 的多档码率版本，并保存编码参数、源哈希和每段 PTS。
2. 把本轮 CRF 35 结果扩展到完整 100 帧和至少 3 个场景；用同一时间窗比较输入、BasicVSR++、FastDVDnet 和保守的无处理基线，至少记录 Y 通道 PSNR/SSIM、LPIPS、边缘误差、运动对齐后的闪烁和文字区域稳定性。
3. 对 5/15/30 帧窗口分别测量显存、冷启动、稳态速度和边界帧行为；任何 GPU 测试都必须使用官方 MMCV 算子或明确记录替代算子的差异。
4. 只有在多档真实压缩集上证明收益且没有稳定性回归后，才设计“压缩修复”可选处理阶段；默认应关闭，并允许按素材跳过。

FastDVDnet 的现有本机报告已经显示：它能改善固定高斯噪声，但在压缩样本上 PSNR 从 34.16 降到 33.19 dB，干净片也会被改变。因此不能把 BasicVSR++ 与 FastDVDnet 组合成默认链路，也不能以“时序模型”或边缘能量上升宣称达到 Topaz 水平。

## 许可与部署边界

RealBasicVSR/ MMEditing 源码目录带 Apache-2.0，但模型权重下载页没有在本次审计到的 README 或模型条目中给出独立的 checkpoint 再分发条款。BasicVSR++ 代码与权重也应分别审查。当前权重仅保留在本机隔离研究目录，不进入 `models/cache`、安装包、OSS 或 Storj；没有完成权重、训练数据和 Windows 推理依赖审查前，不发布给用户。

此外，当前路径使用 torchvision CPU 替代 MMCV 自定义算子，不能作为 ONNX/TensorRT 部署证据。产品化前必须单独完成 CUDA 算子数值对照、显存测试、导出图审计和许可证审查。
