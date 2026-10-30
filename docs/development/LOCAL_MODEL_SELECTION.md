# 本机模型选择与清理记录

本机默认只需要一个插帧模型和一个超分模型。当前默认选择是 RIFE 4.25 FP32 SDR8 与 Real-ESRGAN x2plus；NAFNet GoPro 作为独立的单帧去模糊模型保留，只有用户明确选择去模糊时才使用。它不是插帧或超分模型，也不能把自然运动模糊当作去模糊任务。

“选择”表示在本机、已记录的候选和设置下证据最完整的工作点，不表示任何内容类型中绝对画质最高。当前所有感知画质比较都是本地合成 BT.709 素材；没有用户视频上传，也没有足够的真实素材参考集，所以真实视频的主观排名仍未完成。

## 默认模型

| 用途 | 默认包 | ONNX SHA-256 | 许可与证据边界 |
| --- | --- | --- | --- |
| 插帧 | `models/cache/rife-4.25-fp32-sdr8-v3`（RIFE 4.25 FP32 SDR8） | `8905ab350363251e45e1b46fec355042da9514a34941e64c62b82d9c422205d3` | 上游 Practical-RIFE 在固定提交的 README 中明确说明已链接训练模型适用项目 MIT；本地 CPU、CUDA、TensorRT FP32 小尺寸数值检查和 720p 端到端记录。HDR、真实素材主观画质和 FP16 未通过产品验收。 |
| 超分 | `models/cache/realesrgan-x2plus-sdr8-candidate-v2`（Real-ESRGAN x2plus） | `1288582ed18dad8f7b6083b8cd132665b44bab99510d352d1f13084a38cc5053` | 上游代码仓库为 BSD-3-Clause，模型清单链接独立 release 权重；本项目没有把仓库许可扩展到权重再分发，因此只作本机使用，禁止随产品发布。CPU 参考检查和本机 TensorRT 输出尺寸/时序检查已通过，真实素材画质未验收。 |
| 独立去模糊 | `models/cache/nafnet-gopro-deblur-sdr8-fp32-eval`（NAFNet GoPro FP32） | `b99b39f0bdf29f15205aa50dd23b7a14b571b0c7ae9576ba2f9417fccad4f056` | OpenCV Zoo 本地来源记录为 MIT；本地 CPU 派生图参考、CUDA/队列恢复小样本已通过。它是单帧 BT.709 SDR8 去模糊，可能产生时间闪烁，来源权重再分发范围仍未独立确认，只作本机使用。 |

RIFE 候选使用同一份 1280×720、30 FPS、2 秒、BT.709、FFV1 素材，以一次预热加三次测量、60 FPS、8 Mbps、MKV、自动编码设置比较。非源帧合成参考的均值为：4.22 Lite 30.272 dB/0.9643 SSIM，4.25 30.615 dB/0.9649，4.25 Lite 30.877 dB/0.9681，4.26 30.617 dB/0.9647。4.25 Lite 在这份短合成参考上最高，但没有真实素材证据证明它对所有场景更好；端到端中位速度为 4.25 13.979 FPS、4.25 Lite 13.965 FPS、4.26 13.934 FPS。选择 4.25 是为了采用上游对多数场景的推荐、保留完整本机 CUDA/TensorRT/端到端证据，并避免把一次短合成排名写成普遍画质结论。

## 目标尺寸和 GTX 1660 SUPER 证据

Real-ESRGAN x2plus 的倍率固定为 2。现有相同素材的 TensorRT 记录将 960×540 输出到 1920×1080，端到端输出速度为 0.3006 FPS，流水线速度为 0.4798 FPS；输出尺寸、帧数和时间戳校验通过。x4plus 将相同输入输出到 3840×2160，端到端输出速度为 0.1082 FPS，流水线速度为 0.1264 FPS；它没有在本机显示出足以抵消约 2.8 倍速度差的产品证据，因此已清理候选缓存。

目标尺寸已接入媒体管线。`VideoEncodingSettings.outputWidth/outputHeight` 必须成对设置；两者为零时保留原有默认行为。选择 x2plus 时，默认编码尺寸仍是模型产生的完整 x2 尺寸；显式设置 2560×1440 时，管线先生成 3840×2160，再由写入器使用 `SWS_LANCZOS` 降采样到 2560×1440。显式 3840×2160 与生成帧同尺寸，不发生空间重采样。目标尺寸必须保持编码宽高比、不能超过模型生成尺寸且不能超过 16384 像素；不满足条件的任务会明确失败，避免默默拉伸。写入器保留源 SAR、色彩标签、位深、HDR side data、PTS、帧时长和辅助音轨复制语义。

任务配置从 schema 5 升至 schema 6，目标尺寸随编码设置持久化；旧 schema 1–5 读取时默认目标尺寸为零。检查点清单包含完整任务配置，因此目标尺寸会参与检查点身份匹配；旧 schema 5 检查点升级为 schema 6 时安全地补零，旧检查点不会被错误地解释成 1440p 或 2160p。独立构建 `build/model-selection-target` 中，20 项目标测试已通过，包含恢复帧到目标尺寸的短片写入、逐帧比例保护、音轨复制、时间戳、非法尺寸、任务往返和检查点升级。当前没有在 GTX 1660 SUPER 上启动新的长片 GPU 基准，以避开原生桌面代理的运行窗口；当前快照约 5029 MiB 空闲（6144 MiB 总显存），仅记录为设备状态，不作为 1080p 长片性能证据。

NAFNet 的本机小样本 RIFE+去模糊运行输出 256×144、60 FPS，流水线速度为 0.3999 FPS；报告明确没有参考视频，因此没有 PSNR、SSIM 或 LPIPS。它只证明能运行和恢复队列，不能证明真实画质修复或时间一致性。

## 清理边界和结果

清理前重新计算了保留包的模型、许可证和验证报告哈希，三者均与描述文件一致，状态均为 `passed_cpu_reference`。随后只删除了 `models/cache` 下没有队列或检查点精确路径引用的目录：

- `rife-4.22-lite-fp32-sdr8`
- `rife-4.25-fp32-sdr8`
- `rife-4.25-fp32-sdr8-v1`
- `rife-4.25-lite-fp32-sdr8-v1`
- `rife-4.26-fp32-sdr8-v1`
- `realesrgan-x2plus-sdr8-candidate`（与保留的 v2 ONNX 字节相同）
- `realesrgan-x4plus-sdr8-candidate`
- `nafnet-gopro-deblur-sdr8-eval`（旧的量化来源图，未被队列使用）

`rife-4.25-fp32-sdr8-v2` 仍保留，因为 `work/queue-debug/native-queue.sqlite` 的任务配置精确引用它；RIFE v3 和 NAFNet FP32 仍保留，因为三个本地队列/检查点数据库精确引用它们。没有删除视频、结果、SQLite 任务库、检查点或输出包。旧来源权重和下载归档位于 `models/weights` 与 `work/downloads`，不属于本次仅限模型缓存的清理范围，仍不上传、不作为运行时目录。

清理前后审计文件：`out/model-evaluation-2026-10-28/model-selection-cleanup-audit-before.json`、`out/model-evaluation-2026-10-28/model-selection-cleanup-audit-after.json`。清理后保留包仍存在且 ONNX SHA-256 与描述一致；被删除的八个目录均不存在，数据库扫描没有发现对它们的精确路径引用。

## 后续可执行任务

1. 准备本机真实 1080p SDR 素材和带可信参考的短片段，在相同解码、编码、帧率和分辨率策略下盲评 RIFE 4.25 与候选复核结果；不上传素材。
2. 在 GTX 1660 SUPER 空闲窗口验证 1920×1080→3840×2160，以及 x2 后降采样到 2560×1440 的长片段，记录峰值显存、TensorRT tactic 回退、输出色彩和时序。
3. 为 NAFNet 增加真实运动模糊和静态细节参考，逐帧检查闪烁；在这项证据完成前，不把“画质修复”写成已达成的产品质量承诺。
4. 发行包继续只放默认 RIFE 与 x2plus，并在独立权重再分发审查完成前保持本机权重策略；去模糊是否随包由明确的许可和真实画质结果决定。

完整数值、TensorRT 和视频报告位于 `out/model-evaluation-2026-10-26`；RIFE 来源与导出边界见 [RIFE 模型验证](RIFE_MODELS.md)。
