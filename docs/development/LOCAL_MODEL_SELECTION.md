# 本机模型选择与清理记录

本机基础选择只需要一个插帧模型和一个超分模型：RIFE 4.25 FP32 SDR8 与 Real-ESRGAN x2plus。r17 本机评估包另外包含默认关闭的 FastDVDnet 和 STDF-R3，不能把“四模型随包”写成“四个默认处理步骤”。NAFNet GoPro 是另外保留的单帧去模糊本机候选，仅在对应处理路径支持且用户明确选择时使用；它不是插帧或超分模型，也不能把自然运动模糊自动判为需要修复。

“选择”表示当前本机证据较完整的工作点，不表示所有内容类型中绝对画质最高。历史候选排名主要来自合成 BT.709 素材；2026-10-31 新增连续 15 帧 recorded-luma STDF 研究，但其参考是已解码灰度 proxy，不是真实 ground truth，也不是自然彩色视频盲评。Sintel 720p trailer 原始帧有明确 CC BY 3.0 许可，压缩包已按官方 SHA-256 下载，逐帧清单和确定性色彩约定下的参考媒体已核验。RIFE 4.25 与 4.26 使用同一修复版流程完成整片动画对照；在去掉端点保持帧后的 1,253 帧参考上，4.26 的非源采样均值略高（46.315 dB/0.98489 对 46.159 dB/0.98475），单次吞吐分别约 6.87 与 6.81 输出帧/秒。该小差异只代表一段未经镜头拆分的动画 trailer，不能视为通用模型胜出或产品验收；4.25 继续作为产品默认。动画结果不构成真实视频证据，候选训练集是否包含 Sintel 也未知。虽然新增两段短实拍中点评测，目前仍缺少覆盖不同题材和运动条件的多段可信参考素材，实拍盲评未完成。参见 [真实亮度 proxy 质量记录](STDF_RECORDED_LUMA_QUALITY.md)和[补帧基准协议](VFI_BENCHMARK_PROTOCOL.zh-CN.md)。

2026-11-03，参考评估器已在 RIFE 4.25/4.26 Sintel 对照中运行，生成严格对齐的逐帧 PSNR/SSIM、相邻帧差分 PSNR/SSIM、共享低分复核清单和评估工具哈希。先修复了切镜边界量化时源帧被前一帧替换的问题；后续复核又发现切镜恰落在输出中点时保持前一帧会令新镜头晚一帧出现。切镜非源采样现改为保持时间上最近的输入帧，等距时选后帧；重复帧和异常运动策略不变。用同一 627 帧 12 fps 源、GTX 1660 SUPER CUDA FP32、FFV1 yuv444p，对 RIFE 4.25/4.26 的旧/新策略共四个输出做 1,253 帧原始参考对照；四者的 627 个源帧时刻都逐像素匹配。帧 621（25.875 秒）从 11.19 dB/0.758767 提升到 23.86 dB/0.929645，触及该帧的两个时间差分过渡从 12.34/12.40 dB 提升到 24.64/23.93 dB。全片 626 个非源样本中，4.25 为 46.159 dB/0.984748，4.26 为 46.315 dB/0.984892；最低 PSNR 分别为 17.27 和 17.89 dB。RIFE 4.26 均值仍略高，但重复帧/切镜保护样本仍在非源集合中；只有单段动画 trailer，镜头拆分、多评审者盲评和授权实拍素材集仍未完成。该结果不能代替真实素材验收。

同日对已保存的 shot-aware 报告做了镜头级重分析，没有重跑推理：只比较每个镜头内非源时刻样本，不计源帧样本或跨切镜样本。21 个有此类样本的镜头合计 605 个样本。比较各镜头平均 PSNR，4.26 在 10 个镜头较高，4.25 在 6 个较高，另 5 个镜头两者的所有样本均为 `perfect`；比较平均 SSIM，4.26 在 9 个镜头较高，4.25 在 6 个较高，6 个持平。对两者 PSNR 均非 `perfect` 的 16 个镜头，4.26−4.25 的均值差为 −0.49 至 +0.34 dB。胜负随镜头变化，部分镜头的样本数也很少，因此整片小幅领先不能直接外推成通用模型排名。此处仍是同一段 Sintel 动画 trailer 的报告再分析，不是新推理、实拍泛化结论或感知盲评；RIFE 4.25 保持默认。

## UVG-VCM 实拍中点评测（2026-11-03）

[UVG-VCM 官方页面](https://ultravideo.fi/UVG-VCM/index.html)将数据集标记为 CC BY 4.0，并要求引用数据集论文；页面说明只以可辨识形式分发已取得必要授权的内容。本机仅保留原始与派生媒体，不把视频上传到仓库。Karting Time 是偏机器视觉/车牌任务的 3840×2160、60 fps、YUV444 16-bit 70 帧序列；本机 SHA-256 为 `fe573428bec26917934840d502769740fd5e596965946bd5212269bd6811355c`。Floorball Game 是 420 帧同格式运动序列，本次取原始帧 140–209（HTTP Range 字节 `6967296000–10450943999`），70 帧片段 SHA-256 为 `9e7f1d30d4816faa4de3319295af324547099a09938a7fc95ee6fd2e1a6f9df1`。两段上游数据都没有公布色彩元数据或文件 SHA-256；输入按 `tv` range、BT.709 矩阵/传递/原色解释，再以固定 Lanczos 转成 10-bit YUV444 FFV1。这些只是 SDR 评测假设。

每段输入均由 70 个连续上游帧构造：偶数相对索引组成 35 帧、30 fps 原片，索引 0…68 构成 69 帧、60 fps 参考；34 个奇数索引中点用于补帧质量评估，末端未被右侧输入括住的帧不计分。Floorball 当前准备媒体 SHA-256 为 source `9ceaab41be7c8bf0fc5d74c8efc6b00965baac7efa0bad9b7a8cb3d27fb18b36`、reference `da4116e65ab225a5beff351e41f541b5e3e1e3f474a6016522083aa1f7cf5375`。模型使用同一 GTX 1660 SUPER、FP32、FFV1：720p 使用 ONNX Runtime CUDA；1080p 使用 ONNX Runtime TensorRT EP。4.25 ONNX SHA-256 为 `8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3`；4.26 为 `e6df922305d77a7db5685a9f964bae3736cc1f394cd914f34db4c1411e5c0726`。

| 序列 | 尺寸 / 后端 | 模型 | 非源 PSNR (dB) | 非源 SSIM | 时间差分 PSNR (dB) | 时间差分 SSIM | 端到端输出速度 (FPS) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| Karting Time | 720p / ONNX Runtime CUDA | RIFE 4.25 | 33.5097 | 0.943677 | 33.6726 | 0.939203 | 2.3325 |
| Karting Time | 720p / ONNX Runtime CUDA | RIFE 4.26 | 33.5235 | 0.944966 | 33.6868 | 0.940394 | 2.3162 |
| Karting Time | 1080p / ONNX Runtime TensorRT EP | RIFE 4.25 | 31.6179 | 0.927101 | 31.8315 | 0.915520 | 1.0456 |
| Karting Time | 1080p / ONNX Runtime TensorRT EP | RIFE 4.26 | 31.9132 | 0.929632 | 32.1146 | 0.917381 | 1.1415 |
| Floorball Game | 1080p / ONNX Runtime TensorRT EP | RIFE 4.25 | 39.2935 | 0.931747 | 39.3037 | 0.935909 | 1.2615 |
| Floorball Game | 1080p / ONNX Runtime TensorRT EP | RIFE 4.26 | 39.3344 | 0.932283 | 39.3435 | 0.936432 | 1.4132 |

4.26 在两段素材及三个已测配置下的非源帧与时间差分均值均高于 4.25；差异随场景变化。两种 1080p TensorRT 对照中，Karting 的 PSNR/SSIM 差为 +0.2953 dB/+0.002531，Floorball 为 +0.0409 dB/+0.000536。时间差分未做运动补偿，分数不能代替同步盲看。1080p ONNX Runtime CUDA 4.25 曾在第一个 AI 中点分配失败（ORT 报告可用 190,440,960 字节，申请 233,963,520 字节）；同卡 TensorRT FP32 完成两段两版的 70 帧输出，PTS/帧数验证通过且无后端回退。端到端速度是各候选一次 FFV1 全流水线实测，含解码、编码、输出校验；TensorRT 的流水线 FPS 彼此接近，端到端差值受运行时和校验耗时影响，不能视为稳定速度排名。所有配置都远低于 60 fps。

两段各约 1.17 秒，合计只有 68 个有效中点，并且都来自机器视觉数据集；候选训练数据是否含有这些片段也未核验。它们没有覆盖足够多的消费视频、运动模糊、遮挡与镜头切换，也没有做人工盲评。结果支持继续扩大实拍对照，但还不足以把 4.26 设为产品默认；RIFE 4.25 继续作为当前基线。完整准备报告、候选运行报告和质量 JSON 留在本机忽略目录 `out/datasets/uvg-vcm/`，不包含在版本库中。许可和基准方法见[补帧基准协议](VFI_BENCHMARK_PROTOCOL.zh-CN.md)。

## 基础选择与独立保留模型

| 用途 | 默认包 | ONNX SHA-256 | 许可与证据边界 |
| --- | --- | --- | --- |
| 插帧 | `models/cache/rife-4.25-fp32-sdr8-v3`（RIFE 4.25 FP32 SDR8） | `8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3` | 上游 Practical-RIFE 在固定提交的 README 中明确说明已链接训练模型适用项目 MIT；本地 CPU、CUDA、TensorRT FP32 小尺寸数值检查和 720p 端到端记录。HDR、真实素材主观画质和 FP16 未通过产品验收。 |
| 超分 | `models/cache/realesrgan-x2plus-sdr8-candidate-v2`（Real-ESRGAN x2plus） | `1288582ed18dad8f7b6083b8cd132665b44bab99510d352d1f13084a38cc5053` | 上游代码仓库为 BSD-3-Clause，模型清单链接独立 release 权重；本项目没有把仓库许可扩展到权重再分发，因此只作本机使用，禁止随产品发布。CPU 参考检查和本机 TensorRT 输出尺寸/时序检查已通过，真实素材画质未验收。 |
| 独立去模糊 | `models/cache/nafnet-gopro-deblur-sdr8-fp32-eval`（NAFNet GoPro FP32） | `b99b39f0bdf29f15205aa50dd23b7a14b571b0c7ae9576ba2f9417fccad4f056` | OpenCV Zoo 本地来源记录为 MIT；本地 CPU 派生图参考、CUDA/队列恢复小样本已通过。它是单帧 BT.709 SDR8 去模糊，可能产生时间闪烁，来源权重再分发范围仍未独立确认，只作本机使用。 |

另外安装的评估模型为 `fastdvdnet-clipped-five-frame-sdr8-eval`（五帧时序去噪）和 `stdf-r3-y7-sdr8-local-eval`（七帧亮度压缩修复）。两者均不是基础模型选择，功能默认关闭。STDF 图 SHA-256 为 `dcd09f22cbc42f02820135c471a00708b08933d6b228f996329bbe757aad0721`，描述文件记录 CPU/CUDA 后端；它的独立权重许可仍为未确认的本地评估范围，不能随公开发行包再分发。

历史文档记录：RIFE 候选使用同一份 1280×720、30 FPS、2 秒、BT.709、FFV1 素材，以一次预热加三次测量、60 FPS、8 Mbps、MKV、自动编码设置比较。非源帧合成参考的均值为：4.22 Lite 30.272 dB/0.9643 SSIM，4.25 30.615 dB/0.9649，4.25 Lite 30.877 dB/0.9681，4.26 30.617 dB/0.9647。4.25 Lite 在这份短合成参考上最高，但没有真实素材证据证明它对所有场景更好；端到端中位速度为 4.25 13.979 FPS、4.25 Lite 13.965 FPS、4.26 13.934 FPS。选择 4.25 是为了采用上游对多数场景的推荐并保留已记录的本机验证工作点，不把一次短合成排名写成普遍画质结论。原始 `out/model-evaluation-2026-10-26/` 当前不存在，这些数字是历史记录，本次没有据此重做实时排名或声称原报告仍可打开。

## 目标尺寸和 GTX 1660 SUPER 证据

Real-ESRGAN x2plus 的倍率固定为 2。历史相同素材的 TensorRT 记录将 960×540 输出到 1920×1080，端到端输出速度为 0.3006 FPS，流水线速度为 0.4798 FPS；输出尺寸、帧数和时间戳曾通过校验。x4plus 将相同输入输出到 3840×2160，端到端输出速度为 0.1082 FPS，流水线速度为 0.1264 FPS；历史记录没有显示足以抵消约 2.8 倍速度差的产品证据。x4plus 当前未安装到活动缓存，不是本次已执行磁盘删除的证明；原评估目录缺失也意味着本次未重验这些数字。

目标尺寸已接入媒体管线。`VideoEncodingSettings.outputWidth/outputHeight` 必须成对设置；两者为零时保留原有默认行为。选择 x2plus 时，默认编码尺寸仍是模型产生的完整 x2 尺寸；显式设置 2560×1440 时，管线先生成 3840×2160，再由写入器使用 `SWS_LANCZOS` 降采样到 2560×1440。显式 3840×2160 与生成帧同尺寸，不发生空间重采样。目标尺寸必须保持编码宽高比、不能超过模型生成尺寸且不能超过 16384 像素；不满足条件的任务会明确失败，避免默默拉伸。写入器保留源 SAR、色彩标签、位深、HDR side data、PTS、帧时长和辅助音轨复制语义。

目标尺寸最初由 schema 6 引入，旧 schema 1–5 读取时默认尺寸为零；这里不是把 schema 6 写成当前版本。检查点清单包含完整任务配置，目标尺寸参与身份匹配；旧 schema 5 升级补零，不能被解释成 1440p 或 2160p。历史独立构建 `build/model-selection-target` 曾记录 20 项目标测试通过，覆盖恢复帧到目标尺寸、逐帧比例、音轨、时间戳、非法尺寸与检查点升级。压缩分块配置在 schema 11 引入，schema 12 自动压缩已提交并通过短合成素材的本地队列测试；它仍是实验功能，见 [原生压缩控制](NATIVE_COMPRESSION_CONTROLS.md)。历史瞬时空闲显存不是当前设备状态或长片性能证据。

NAFNet 的本机小样本 RIFE+去模糊运行输出 256×144、60 FPS，流水线速度为 0.3999 FPS；报告明确没有参考视频，因此没有 PSNR、SSIM 或 LPIPS。它只证明能运行和恢复队列，不能证明真实画质修复或时间一致性。

## 后续研究候选（2026-10-31）

当前 Real-ESRGAN x2plus 是可运行的本机超分基线，不是已经通过真实视频画质、时间一致性或权重再分发验收的正式预设。新推荐只评估一个通用超分候选：`realesr-general-x4v3`（SRVGGNetCompact）。它还没有在本项目下载、导出、注册或验证；不能在界面或发行说明中写成已支持。上游代码为 BSD-3-Clause，但作者链接的 checkpoint 仍需独立确认权重授权和再分发范围。

该候选原生固定 x4，1080p 对应的神经网络输出为 7680×4320，再缩至 2560×1440 或 3840×2160；不能把最终输出倍率当成模型倍率。上游分块辅助代码仍分配完整高分辨率输出。未来实现必须验证有界 tile 输出/重采样、halo、接缝、峰值显存及逐帧闪烁，不能仅凭“小模型”宣传就承诺 GTX 1660 SUPER 能实时或不会 OOM。官方默认去噪强度通过两份 checkpoint 做 DNI 参数混合；单包实验应显式固定来源与混合策略。

STDF-R3 已从纯研究候选推进到可选本机评估实现：七帧、Y 通道，CPU/CUDA 图和原生适配器已验证；r17 提供默认关闭的压缩修复与独立高清分块开关。有界分块使用 256×128 core、halo 64、全图右/下 pad8，每个图输入不超过 384×256；可见输入每轴不超过 3840、面积不超过 8,294,400。生成的横竖 UHD 已有全像素 CPU/CUDA 对照，见 [分块证据](STDF_R3_TILING.md)。这不证明真实素材普遍提升、TensorRT 已验收或权重可公开再分发。

连续 recorded-luma proxy 的 H.264 80 kbps 与 HEVC QP37 受控重压缩实验已显示小幅平均 PSNR-Y 提升；但 HEVC 100% 强度有一帧变差，未新增压缩的 clean proxy 在所有非零强度下均变差。因此 STDF 仍是需要明确选择的实验选项，不是默认“自动增强”，也不等同于高斯去噪、单帧去模糊或 Topaz 级质量。schema-12 自动压缩已提交，但短合成回归不等于真实素材质量验收。VRT 因作者 CC BY-NC 4.0 非商业条款，不纳入当前产品选择。

完整的一手来源、代码/权重许可分别核验结果、实现依赖及待执行的视觉/性能/恢复验收矩阵见 [视频修复架构的本机候选决策](../architecture/VIDEO_RESTORATION.md#local-candidate-decision--2026-10-31)。架构候选说明需结合上述后续 STDF 本机证据阅读；本次文档修订没有新增权重或运行时功能。

## 新一代补帧候选观察（更新于 2026-11-04）

当前 RIFE 4.25 仍是唯一已接入产品流程的补帧基线；新论文的作者指标不能代替在同一真实素材、硬件和输出策略下的复测。

| 候选 | 当前证据 | 集成决定 |
| --- | --- | --- |
| RDVFI（ICLR 2026） | [作者项目仓库](https://github.com/Mayongrui/RDVFI)提出用低分辨率稀疏运动关键帧生成连续像素轨迹，再把高分辨率输入帧 warp 到目标时间；作者报告 1024×576 下 17 FPS 和主观偏好结果。仓库当前只有项目页、论文和静态资源；README 的 Code 链接回到该项目仓库，没有可复核的推理源码、权重或许可材料。 | 观察名单。不能据论文速度或作者盲评声称可在本机 GTX 1660 SUPER 运行；先等待有效一手实现、模型文件和各自的许可，再评估显存、长序列和输出质量。 |
| SPEED（ACM MM 2026） | [作者项目页](https://bbldcver.github.io/SPEED/)链接到[官方 PyTorch 仓库](https://github.com/bbldCVer/SPEED)和 Hugging Face 权重页。本次核对的默认分支 `main` 最近可见提交为 `40fadbe85c88cc6e4015062389da464fd7e85ab9`；根目录没有 LICENSE 文件，GitHub 文件 API 对 `LICENSE` 返回 404，因此代码许可仍未确认。[权重页](https://huggingface.co/bbldCVer-hf/SPEED)标注 Apache-2.0，但该标记不覆盖 GitHub 代码。官方 README 建议 Python 3.10、注明 CUDA 11.8 tested，并依赖 xFormers、CuPy 和 CUDA correlation kernel；官方 `inference.py` 支持 FP32/FP16/BF16，checkpoint 是 `.pt` pickle。论文报告的 SNU-FILM 和 4K 指标仍是作者结果，没有在本机复现。 | 暂停本机执行与集成，直到代码使用许可明确。本机 `nvidia-smi --query-gpu=name,compute_cap,memory.total --format=csv,noheader` 返回 GTX 1660 SUPER、CC 7.5、6144 MiB；驱动报告 CUDA 12.6。[NVIDIA CUDA 文档](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/mathematical-functions.html)要求 BF16 至少 CC 8.0，因此不能把官方 BF16 示例当成本机可用路径。FP16/FP32 的实际显存、速度、稳定性和质量仍未测；即使之后取得代码许可，也须先做受控低分辨率显存冒烟，再用本机已有可信参考执行盲评与端到端对照。权重许可、代码许可和依赖许可须分别审查。 |
| LDF-VFI（CVPR 2026） | [作者官方仓库](https://github.com/xypeng9903/LDF-VFI)标示 Apache-2.0，[作者链接的模型页](https://huggingface.co/onecat-ai/LDF-VFI)也标示 Apache-2.0，显示约 1B 参数、F32 权重。官方 README 的 8× quick start 说明需要约 20 GB GPU 显存；模型面向 2×–16×片段插值与长序列上下文。 | 研究观察项，不进入当前 GTX 1660 SUPER 6 GB 的候选执行队列。长序列建模方向值得后续关注，但作者给出的约 20 GB 需求已超过本机显存；只有官方发布更小权重/明确低显存配置，或换用足够显存设备后再评估。 |
| SpeedVFI（ICML 2026） | [PMLR 论文页](https://proceedings.mlr.press/v306/ding26t.html)描述整段序列一次前向、一步去噪和时序 RoPE；这与现有相邻帧 RIFE adapter 的调用契约不同。此次核对没有找到可审计的公开权重、实现和许可组合。 | 研究观察项，不接入当前模型中心或默认工作流。若后续发布，必须先设计有界片段/上下文和恢复身份，再做同集盲评及设备测量。 |
| MoG（2025） | [官方代码仓库](https://github.com/MCG-NJU/MoG-VFI)与[作者模型卡](https://huggingface.co/MCG-NJU/MoG)均标示 Apache-2.0，提供实拍/动画两类扩散式补帧权重；模型仓库约 21 GB。官方流程指定 CUDA 12.1，建议可启用 FP16 降低显存，并说明采样结果可能因随机性变化；推理工作流含文本提示。依赖的 DynamiCrafter 与 ToonCrafter 上游项目也各有许可和致谢要求。 | 仅作为高显存生成式对照候选。当前 GTX 1660 SUPER 的显存峰值、速度、提示敏感度、逐帧保真和可重复性均未资格验证；不替代 RIFE，也不进入默认工作流。 |
| AMT（CVPR 2023） | [作者官方仓库](https://github.com/MCG-NKU/AMT)提供 AMT-S/L/G 权重入口，但代码仓库明确采用 CC BY-NC 4.0，并说明商业使用需取得正式许可。权重许可需单独核验。 | 非商业研究参考；在取得适用于目标发行方式的书面许可前，不复制其代码或把其模型纳入可发布产品。 |

下一轮候选准入仍按同一顺序执行：锁定上游提交、模型权重来源及分别适用的许可；证明可导出到受支持运行时并满足内存上限；在已对齐的可信高帧率参考上记录逐帧指标、时间稳定性、速度和峰值显存；最后对大运动、遮挡、细线文字、动漫和游戏 HUD 做随机匿名全速盲评。候选在完成这些门槛前不会替换 RIFE 4.25，也不会仅按单一 PSNR 或论文作者排名淘汰当前模型。

## 清理边界和结果

2026-10-31 只读核对时，当前 `models/cache` 包含 RIFE v3、恢复引用需要的 RIFE v2、Real-ESRGAN x2plus v2、NAFNet FP32、FastDVDnet 和 STDF 共六个目录。以下八个旧运行缓存名当前不在该目录清单中；本次没有执行删除，也不能由“当前不存在”反推某次删除的执行结果：

- `rife-4.22-lite-fp32-sdr8`
- `rife-4.25-fp32-sdr8`
- `rife-4.25-fp32-sdr8-v1`
- `rife-4.25-lite-fp32-sdr8-v1`
- `rife-4.26-fp32-sdr8-v1`
- `realesrgan-x2plus-sdr8-candidate`（与保留的 v2 ONNX 字节相同）
- `realesrgan-x4plus-sdr8-candidate`
- `nafnet-gopro-deblur-sdr8-eval`（旧的量化来源图，未被队列使用）

`rife-4.25-fp32-sdr8-v2` 仍保留，历史审计记录 `work/queue-debug/native-queue.sqlite` 的任务配置精确引用它；该说明不代表本次打开或迁移了用户数据库。未删除视频、结果、SQLite 任务库、检查点或输出包。`models/weights/rife4.22-lite-reviewed`、`rife4.25-lite-reviewed`、`rife4.26-reviewed` 和 `work/downloads/rife4.22-lite.zip`、`rife4.25-lite.zip`、`rife4.26.zip` 当前仍存在。此前旧权重/归档删除被 automatic approval review 拒绝，未执行成功；本次没有重试。旧来源材料不是运行时模型选择，不能称为“已从磁盘全部清除”。

历史文本引用的 `out/model-evaluation-2026-10-28/model-selection-cleanup-audit-before.json` 和 `model-selection-cleanup-audit-after.json` 当前均不可用：其父目录不存在；`out/model-evaluation-2026-10-26/` 也不存在。不得把这些缺失路径当作本次可复核的审计证据。后续若清理，需重新审计运行进程、硬链接、精确恢复引用和实际磁盘释放量，不能用历史列表直接执行。

## 后续可执行任务

1. 准备本机真实 1080p SDR 素材和带可信参考的短片段，在相同解码、编码、帧率和分辨率策略下盲评 RIFE 4.25 与候选复核结果；不上传素材。
2. 在 GTX 1660 SUPER 空闲窗口验证 1920×1080→3840×2160，以及 x2 后降采样到 2560×1440 的长片段，记录峰值显存、TensorRT tactic 回退、输出色彩和时序。
3. 为 NAFNet 增加真实运动模糊和静态细节参考，逐帧检查闪烁；在这项证据完成前，不把“画质修复”写成已达成的产品质量承诺。
4. 本机可运行包沿用当前 RIFE 与 x2plus 选择；公开发行包、模型镜像与更新渠道必须分别遵守权重再分发审查，不能把“本机可运行”等同于“可随产品发布”。去模糊是否随包由明确的许可和真实画质结果决定；新研究候选不进默认包。
5. 按关联架构文档的验收矩阵，先验证 `realesr-general-x4v3` 的授权、导出及有界内存可行性，再决定是否实施本机画质对比；STDF-R3 单独评估，不与当前能力合并宣称已完成。

历史数值、TensorRT 和视频报告曾引用 `out/model-evaluation-2026-10-26`，该目录当前不存在；现存 STDF 亮度证据位于 `work/verification/stdf-real-luma-continuous-20261031/quality.json`。RIFE 来源与导出边界见 [RIFE 模型验证](RIFE_MODELS.md)。
