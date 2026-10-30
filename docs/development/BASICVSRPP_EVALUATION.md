# BasicVSR++ compressed-video evaluation

审计日期：2026-10-29

本记录只描述 BasicVSR++ 的官方压缩视频增强候选，不表示该模型已经进入 RifeLove，也不表示已经完成 Windows、ONNX、TensorRT 或画质验收。

## 结论

本轮不修改 UI/core、不复制运行库，也不把该候选加入模型缓存或发行包。官方来源没有发现禁止本地研究的条款，因此已将一份 track 1 权重和一份 SPyNet 权重下载到独立的 `work/basicvsrpp-eval/` 目录，仅用于本机评估。它们不属于 RifeLove 发行包，也没有上传到 OSS 或 Storj。原因是：

1. 官方代码仓库采用 Apache-2.0，但模型页只提供 `.pth` 下载链接，没有在审计到的 README、配置或模型条目中给出独立的 checkpoint 再分发许可。代码许可证不能自动证明权重可以随 RifeLove 打包；这限制公开再分发，不等同于禁止用户在本机进行小规模研究。
2. 压缩模型使用 MMCV 的 `ModulatedDeformConv2d`。MMagic 没有提供该模型的官方 ONNX/TensorRT 导出配置；直接导出预计需要 MMCV 自定义算子或 TensorRT plugin，不能作为当前纯 ONNX 模型包直接使用。

## 官方模型与配置

审计的配置是：

`basicvsr-pp_c128n25_600k_ntire-decompress-track1.py`

关键配置：

- `BasicVSRPlusPlusNet`
- `mid_channels=128`
- `num_blocks=25`
- `is_low_res_input=False`
- `cpu_cache_length=100`
- 输入预处理为 RGB，`mean=[0,0,0]`、`std=[255,255,255]`
- 模型源码接收 `[N,T,3,H,W]` 视频序列，并返回同样的时间长度
- `is_low_res_input=False` 时，压缩修复模型输出与输入保持相同空间尺寸；它不是 x4 超分路径
- 源码对低分辨率特征有至少 64 像素的轴向要求；该配置先将全分辨率输入缩小到四分之一，因此实际输入至少应满足约 256 像素的宽和高

网络包含四路时序传播、SPyNet 光流和二阶可变形对齐。`ModulatedDeformConv2d` 来自 MMCV 的 CUDA 扩展，不是标准 ONNX 算子。

官方模型条目还列出了 NTIRE decompression track 2、track 3 和 VSR 模型；它们不能因为名称相近就混用。第一轮候选应固定 track 1，避免同时引入多个大权重。

官方来源：

- MMagic 配置：`https://github.com/open-mmlab/mmagic/blob/main/configs/basicvsr_pp/basicvsr-pp_c128n25_600k_ntire-decompress-track1.py`
- MMagic BasicVSR++ 模型页：`https://github.com/open-mmlab/mmagic/blob/main/configs/basicvsr_pp/README.md`
- BasicVSR++ 网络源码：`https://github.com/open-mmlab/mmagic/blob/main/mmagic/models/editors/basicvsr/basicvsr_plusplus_net.py`
- MMagic 许可证：`https://github.com/open-mmlab/mmagic/blob/main/LICENSE`

## 权重来源和体积

官方 track 1 权重链接：

`https://download.openmmlab.com/mmediting/restorers/basicvsr_plusplus/basicvsr_plusplus_c128n25_ntire_decompress_track1_20210223-7b2eba02.pth`

2026-10-29 对官方下载端点执行 HEAD 并完成本地下载：

- BasicVSR++ track 1：176,414,076 bytes，ETag `0F7742FAE247338E338BE528D4ADA9C3-69`
- SPyNet：5,770,988 bytes，ETag `5185D0677645416ACD18928B1DB9C1E0`

文件体积对于隔离研究目录是可接受的。实际文件和 SHA-256：

- `work/basicvsrpp-eval/basicvsr_plusplus_c128n25_ntire_decompress_track1_20210223-7b2eba02.pth`
  - SHA-256：`7b2eba02a24989bfbf8b2ed4a06c8e6fd5dbeb193b1178ef7171cd1c455ddb0f`
- `work/basicvsrpp-eval/spynet_20210409-c6c1bd09.pth`
  - SHA-256：`c6c1bd09b52d05ba17f3e701f549d6faf5e314aabce8ae462c1c171a8d6c4914`

权重没有写入 `models/cache`，没有创建第二份大文件，也没有上传 OSS 或 Storj。ETag 仅作为官方下载端点记录，RifeLove 后续仍应使用 SHA-256 绑定本地评估文件。

## 权重内部元数据检查

使用项目已有的 `work/model-tools/Scripts/python.exe` 在 CPU 上读取 checkpoint，没有运行模型：

- `state_dict`：567 个条目
- 参数数量：44,075,638
- checkpoint 内部训练环境：MMEditing 0.5.0、MMCV 1.2.1、PyTorch 1.6.0、CUDA 10.1、Linux、Tesla V100 32 GB
- checkpoint 内部训练配置声明 `num_input_frames=30`，训练 patch 为 256，测试配置使用 100 帧
- checkpoint 内部模型名为旧版 `CompressionSecondOrderGridPropagationv2`，而当前 MMagic 配置注册为 `BasicVSRPlusPlusNet`

因此不能只把 `.pth` 文件交给当前 MMagic 配置就宣称兼容。下一步必须验证当前网络的 state-dict key、层形状和输出数值；旧版 checkpoint 的元数据还显示其原始环境与本机 CUDA 12.8/6 GB 环境差异很大。

## Python、Windows 和本机环境

本机只读检查结果：

- 系统 Python 3.15.0 不包含研究依赖，但它不是项目唯一环境
- 项目已有 `work/model-tools/Scripts/python.exe`：Python 3.11.17、CPU PyTorch 2.8.0、ONNX 1.17.0、ONNX Runtime 1.22.0
- 该隔离环境额外安装了官方 PyTorch CPU wheel `torchvision 0.23.0+cpu`，并补充 Pillow 11.3.0 以满足 torchvision 导入依赖
- 该隔离环境仍未安装 `mmagic`、`mmengine` 或 `mmcv`
- GPU：NVIDIA GeForce GTX 1660 SUPER，6144 MiB，compute capability 7.5
- 驱动：560.94
- RifeLove 原生包使用 ONNX Runtime 1.22.0、TensorRT 10.9.0.34、CUDA 12.8

MMagic 的官方运行链是 Python、PyTorch、MMEngine、MMCV 和自定义 CUDA 扩展。已有隔离环境可以复用其 ONNX 工具，但只有 CPU PyTorch，不能运行 GPU reference。安装完整 MMagic/MMCV CUDA 环境会引入编译依赖和缓存，本轮没有安装大型 GPU 环境。

## CPU reference 结果

为了验证 checkpoint 和网络结构是否至少可以在本机进行研究性推理，`work/basicvsrpp-eval/run_cpu_reference.py` 从官方 MMagic 源码副本加载网络，并用官方 PyTorch CPU 索引中的 `torchvision 0.23.0+cpu` `torchvision.ops.deform_conv2d` 替代 MMCV 算子。该脚本不属于 RifeLove 核心，也不读取用户视频。

验证结果：

- `--check-only`：567 个 checkpoint 参数条目严格匹配，missing keys=0，unexpected keys=0
- T=5、256x256、CPU：8.947334 秒，总计约 1.789467 秒/帧
- 输出形状：`[1, 5, 3, 256, 256]`
- 本地合成退化素材输入 PSNR：33.2976 dB
- 本地合成退化素材输出 PSNR：33.6139 dB
- 结果文件：[cpu-reference-result.json](../../work/basicvsrpp-eval/cpu-reference-result.json)

这个数字只证明当前权重可以通过一个 CPU 研究替代路径产生有限输出。`torchvision.ops.deform_conv2d` 与 MMCV 实现的数值等价性、真实压缩视频质量、GPU 性能、TensorRT 支持和时序稳定性均未被证明；不能把 0.3163 dB 的合成差异宣传为产品画质提升。

## ONNX/TensorRT 可行性判断

结论是“理论上可以做实验性导出，当前不能直接纳入 RifeLove 原生后端”。

可能遇到的阻塞点：

- `ModulatedDeformConv2d` 需要 MMCV 自定义算子。标准 ONNX 图不包含该算子，导出后需要自定义 domain、ONNX Runtime custom-op library 或 TensorRT plugin。
- BasicVSR++ 的前后向传播和可变长度 `[N,T,3,H,W]` 序列需要先固定一个实验时间窗，例如 T=5 或 T=15；不能先假定动态 T、动态空间尺寸和现有 RifeLove 的 tile 逻辑都能工作。
- 官方配置的 `cpu_cache_length=100` 是 Python 推理的缓存策略，不等同于 6 GB 显存下的安全上限。GTX 1660 SUPER 应从 256x256 或 512x288、T=5/8/15、单并发和 FP32 开始测量。
- 如果图被分区到 CUDA/CPU，TensorRT EP 创建成功也不能证明整张 BasicVSR++ 图在 TensorRT 上运行。每个节点的 provider、输出数值和时序一致性都必须记录。
- 当前 RifeLove 模型包约束是声明式 ONNX 权重包，不能携带任意可执行预处理或未审计的自定义算子库。因此即使 Python 参考推理可运行，也不能直接作为发行模型。

## 后续可复现实验（获得权重许可后）

1. 在独立环境固定 Python、PyTorch、MMEngine、MMCV、MMagic 和 CUDA 版本；不要修改 RifeLove 的运行库目录。当前 CPU reference 环境可用于结构检查，但不能替代 GPU reference。
2. 复用已下载的 track 1 和 SPyNet，重新核对 URL、文件长度、SHA-256、版本和许可证证据；不要把它们复制到 `models/cache`。
3. 用官方配置先跑 PyTorch reference：T=5、T=15；分辨率 256x256、512x288、960x540；记录冷启动、稳态 FPS、峰值显存和输出尺寸。
4. 对固定 T=5 和 T=15 分别尝试 ONNX 导出，运行 `onnx.checker` 并列出自定义节点。当前 CPU reference 只完成了 PyTorch forward，尚未导出；若出现 MMCV custom op，停止声称“纯 ONNX 可部署”。
5. 仅在 custom-op 许可、实现和数值校验都明确后，评估 ONNX Runtime CUDA；TensorRT 需要单独确认 plugin 和 engine cache，不应复用 RIFE/Real-ESRGAN 的缓存。
6. 使用干净源片、H.264/H.265 多码率、压缩加噪声、动画/HUD、快速运动和镜头切换，比较 PSNR、SSIM、LPIPS、边缘误差、运动对齐后的闪烁和文字稳定性。
7. 只有当权重许可、Windows 运行链、纯本地依赖、显存、时序质量和取消/失败行为全部通过后，才创建 RifeLove 专用 adapter；在此之前保持研究候选状态。

## 当前不能做到的部分

- 已有权重文件，并完成了基于 torchvision 算子替代的 CPU reference；尚未完成原生 MMagic/MMCV reference、GPU 画质、显存或速度实测。
- 没有证据证明官方 `.pth` 可以随 RifeLove 再分发。
- 没有官方 MMagic/mmdeploy BasicVSR++ Windows + CUDA 12.8 + TensorRT 10.9 部署配方。
- 没有证据证明该模型可以在不使用自定义 MMCV/TensorRT 算子的情况下导出为当前原生后端可接受的纯 ONNX 图。
- 不应把 BasicVSR++ 的论文或官方 benchmark 数字转化为 RifeLove 在 GTX 1660 SUPER 上的性能承诺。
