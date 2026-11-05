# 本地参考帧画质评估

`evaluate_reference.py` 基础模式对比低帧率原片、同一内容的高帧率参考视频和一个 RifeLove 输出；多候选模式则让多个模型输出共用同一原片与参考。它先校验视频尺寸、像素格式、明确的 SDR 色彩标签、逐帧 PTS 和目标帧率时间网格，再用本机 FFmpeg 计算 PSNR 与 SSIM。报告按实际输出 PTS 与最近原片 PTS 的距离分类；距离不超过半个输出容器刻度的帧计入源帧组，兼容 Matroska 等容器的时间戳舍入。这样可避免把同一源帧误归到插值样本组。

评估器还会比较真值与输出的相邻帧变化。它分别对两段视频应用 FFmpeg `tblend=all_mode=grainextract`；滤镜缓存首帧，从第二帧起输出变化图，因此 N 帧输入会产生 N−1 个过渡，再计算这些变化图的 PSNR/SSIM。报告把包含非源采样的过渡与源帧到源帧的过渡分开。它是帧间变化与参考的一致性指标，不是运动补偿指标，也不能单独证明没有闪烁。镜头切换、相机运动和参考对齐都会影响结果。滤镜定义见 [FFmpeg blend/tblend 文档](https://ffmpeg.org/ffmpeg-filters.html#blend) 与 [PSNR 文档](https://ffmpeg.org/ffmpeg-filters.html#psnr)。

## 输入要求

- 原片、参考视频和每个 RifeLove 输出必须是不同文件，尺寸、像素格式及 SDR 色彩标记一致。
- 参考视频和每个 RifeLove 输出必须具有完全相同的帧数与逐帧 PTS；每个结果 PTS 还必须与原片起点和目标帧率组成的输出网格匹配。
- 通过 `--fps` 指定 RifeLove 的目标帧率；结果 PTS 必须落在该时间网格的半个输出容器刻度范围内。
- 参考视频需要来自同一画面内容的真实高帧率录制或可信的合成真值。普通低帧率素材不能自行充当真值。
- 当前评估限定于标记明确且匹配的 SDR 色彩路径。HDR、未知色彩标记和需要色彩变换的视频会被拒绝。

## 已授权的 Sintel 动画回归切片

Sintel/Durian 原始帧按 CC BY 3.0 发布，可在署名 `© copyright Blender Foundation | durian.blender.org` 后复用，包括商业用途；标志、商标和明显非项目第三方素材除外，见[官方许可说明](https://durian.blender.org/sharing/)。Xiph 提供了 trailer PNG 包，但未发布 SHA-256 校验值；本仓库按获取到的 720p 包字节固定了项目摘要 `6324f7ea1a054d2e0b477c070c8f291b7a796926ba7199af51b1701d80c5911b` 和大小。该动画切片不证明真实视频质量；候选训练集是否含 Sintel 也未知。

## 已授权的 UVG-VCM 实拍高帧率试点

[UVG-VCM 官方数据页](https://ultravideo.fi/UVG-VCM/index.html)将数据集标记为 CC BY 4.0，并说明只分发已取得必要权限、个人信息已按需匿名化的内容；使用时需按页面引用数据集论文。初始试点选 `Karting Time`：3840×2160、60 fps、70 帧、YUV444 16-bit。准备器也支持从 4K60 序列中选取连续偶数帧数的字节范围片段，报告完整原始序列帧范围、本机摘要和显式色彩假设。上游没有发布这些片段的 SHA-256 或色彩范围、矩阵、传递函数信息；原始视频不提交到 Git。

```powershell
$data = 'out/datasets/uvg-vcm'
New-Item -ItemType Directory -Force $data | Out-Null
curl.exe --location --fail --output "$data/KartingTime_3840x2160_60fps_yuv444_16bits_70.yuv" `
  'https://ultravideo.fi/UVG-VCM/KartingTime/KartingTime_3840x2160_60fps_yuv444_16bits_70.yuv'

py -3 tools/quality/prepare_uvg_vcm_pilot.py `
  --raw "$data/KartingTime_3840x2160_60fps_yuv444_16bits_70.yuv" `
  --output-directory "$data/prepared-1080p" `
  --ffmpeg 'C:/path/to/ffmpeg.exe' --ffprobe 'C:/path/to/ffprobe.exe' `
  --source-range tv
```

`1080p` 是默认配置；要准备较低分辨率对照时，设置 `--output-profile 720p` 并使用独立的输出目录。

例如可以下载 `Floorball Game` 的原始帧 140–209，建立第二个 70 帧场景。UVG-VCM 原始 4K60 YUV444 16-bit 每帧 49,766,400 字节，下面按帧号计算 HTTP Range 字节偏移，下载结果仍只落在本机忽略目录：

```powershell
$data = 'out/datasets/uvg-vcm'
New-Item -ItemType Directory -Force $data | Out-Null
$frameBytes = [long]3840 * 2160 * 3 * 2
$firstFrame = 140
$frameCount = 70
$byteStart = [long]$firstFrame * $frameBytes
$byteEnd = $byteStart + [long]$frameCount * $frameBytes - 1
$raw = "$data/floorball-game-140-209.yuv"
curl.exe --location --fail --range "$byteStart-$byteEnd" `
  --output $raw `
  'https://ultravideo.fi/UVG-VCM/FloorballGame/FloorballGame_3840x2160_60fps_yuv444_16bits_420.yuv'

py -3 tools/quality/prepare_uvg_vcm_pilot.py `
  --raw $raw --sequence 'Floorball Game' `
  --raw-url 'https://ultravideo.fi/UVG-VCM/FloorballGame/FloorballGame_3840x2160_60fps_yuv444_16bits_420.yuv' `
  --source-start-frame-index $firstFrame --frame-count $frameCount `
  --sequence-total-frame-count 420 --output-profile 1080p `
  --output-directory "$data/floorball-game-140-209-1080p" `
  --ffmpeg 'C:/path/to/ffmpeg.exe' --ffprobe 'C:/path/to/ffprobe.exe' `
  --source-range tv
```

准备器默认每隔 2 帧取样，用局部帧 `0,2,…,68` 生成 35 帧、30 fps 原片，并以原始索引 `0…68` 生成 69 帧、60 fps 参考；34 个奇数索引 `1,3,…,67` 是有效中点。若设置 `--input-rate 15`，则每隔 4 帧取样，以 `0,4,…,68` 生成 18 帧、15 fps 原片；同一参考中有 51 个被两侧输入括住的非源样本，其中 17 个是中点。原始索引 69 没有右侧输入帧，端到端输出在参考窗口后的边界帧也不纳入质量候选。运行插帧后，将结果无损限制到与 69 帧参考相同的 PTS，再使用本节的多候选评估器比较。只有明确固定、记录的色彩假设下的分数可复现；原始网页没有为此序列公布色彩元数据。该试点约 1.17 秒且偏机器视觉场景，不构成通用真实视频排名。

准备同一 Floorball 原始片段的 4×输入时，使用独立输出目录，避免覆盖既有 2×媒体：

```powershell
py -3 tools/quality/prepare_uvg_vcm_pilot.py `
  --raw 'out/datasets/uvg-vcm/floorball-game-140-209.yuv' `
  --sequence 'Floorball Game' `
  --raw-url 'https://ultravideo.fi/UVG-VCM/FloorballGame/FloorballGame_3840x2160_60fps_yuv444_16bits_420.yuv' `
  --source-start-frame-index 140 --frame-count 70 `
  --sequence-total-frame-count 420 --output-profile 1080p `
  --input-rate 15 `
  --output-directory 'out/datasets/uvg-vcm/prepared-floorball-game-140-209-4x-1080p' `
  --ffmpeg 'C:/path/to/ffmpeg.exe' --ffprobe 'C:/path/to/ffprobe.exe' `
  --source-range tv
```

把官方包解压到被 Git 忽略的本机 `out/datasets/sintel-trailer-720p/` 后，先生成来源和逐帧哈希清单，再准备媒体：

```powershell
$data = 'out/datasets/sintel-trailer-720p'
py -3 tools/quality/prepare_sintel_manifest.py `
  --archive "$data/sintel_trailer-720-png.tar.gz" `
  --frames "$data/720" `
  --output "$data/manifest.json"

py -3 tools/quality/prepare_sintel_media.py `
  --manifest "$data/manifest.json" `
  --frames "$data/720" `
  --output-directory "$data" `
  --ffmpeg 'work/codec-ffmpeg-build-x86asm/ffmpeg-install/bin/ffmpeg.exe' `
  --ffprobe 'work/codec-ffmpeg-build-x86asm/ffmpeg-install/bin/ffprobe.exe'
```

媒体准备器先按本机清单重新核验全部 1,253 张 PNG 的大小和 SHA-256，然后生成每隔一帧的 12 fps 源、1,253 帧真实 24 fps 参考，以及末尾多一帧保持的端到端参考；只在同意重建被忽略目录里的输出时加 `--force`。该流程已在本机执行，三份媒体和 FFmpeg 工具身份记录在 `prepared/prepared-media.json`；独立准备的基线素材与其解码像素逐帧一致。所有 PNG 编号连续且可解码；原图没有 sRGB、gAMA、cHRM、iCCP 或 cICP 色彩信息。为让桌面媒体契约和客观评估器一致，准备器将 RGB 像素按明确的 BT.709 评测约定转成 BT.709 限幅 YUV444；这是评测假设，不是原图色彩元数据。顶部 88 行和底部 87 行经逐帧哈希核对为恒定黑边，保留 1280×545 有效画面。端到端参考的末尾保持帧不属于 Sintel 原始帧，模型专属指标须将它单独处理。

## 可选的镜头分组评估

对已人工核准镜头边界的 CFR 数据，可额外传入 `--shot-manifest shots.json`。清单须为 schema v1，包含 `datasetId`、`frameIndexBase: 0`、`frameRate`、`frameCount`、当前原片与参考文件的 `sourceSha256` / `referenceSha256`，以及 `shotStarts`。`shotStarts` 是参考时间轴的零基帧索引，首项必须是 `0`，后续每项是新镜头首帧；索引须严格递增。清单最多 1 MiB，参考时间轴须符合清单声明的 CFR，且各帧时间戳与理想网格的偏差不得超过半个参考容器刻度；原片每个 PTS 都须对齐某一参考帧，源片和参考片哈希必须完全匹配。评测报告会记录清单路径、SHA-256、数据集 ID 和镜头数。

每个候选会得到逐镜头 PSNR/SSIM 和镜头内帧间差分指标。每镜头统计保留源帧，并只计入其两端源帧都属于该镜头的非源采样；跨越切镜的非源采样会单独列出帧号、两端源帧和分数。帧间差分仅聚合镜头内且两端帧均属于该镜头统计集的过渡。原有整片指标与共享复核清单不变，因此切镜样本仍可用于检查产品的切镜保护行为。镜头清单需要人工复核并绑定具体媒体哈希；场景检测器只能提供候选，不能自动视为已确认边界。

本机已使用 [sintel-trailer-720p-shot-manifest-v1.json](../../docs/development/sintel-trailer-720p-shot-manifest-v1.json) 对 RIFE 4.25/4.26 cut-nearest 输出完成一次真实媒体分组运行：22 段、21 个跨镜插值样本已单列，605 个镜头内非源样本保留在逐镜头统计中。完整报告位于被 Git 忽略的 out/datasets/sintel-trailer-720p/quality-rife-4.25-vs-4.26-shot-aware-manifest-v1.json。这只是 Sintel 动画回归证据，不构成跨镜头训练/验证拆分、盲评或真实视频模型排名。

```powershell
py -3 tools/quality/evaluate_reference.py `
  --source source-12fps.mkv --reference reference-24fps.mkv `
  --candidate 'RIFE 4.25=outputs/rife-4.25.mkv' `
  --candidate 'RIFE 4.26=outputs/rife-4.26.mkv' `
  --fps 24 --shot-manifest shots.json `
  --ffmpeg C:/path/to/ffmpeg.exe --ffprobe C:/path/to/ffprobe.exe `
  --report model-comparison.json
```

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

## 同一真值下比较多个模型

给每个候选结果一个名称，并重复传入 `--candidate NAME=PATH`。所有候选必须来自同一原片、具有相同的逐帧 PTS，并匹配参考视频的分辨率、像素格式和 SDR 色彩标签：

```powershell
py -3 tools/quality/evaluate_reference.py `
  --source source-30.mkv --reference reference-60.mkv `
  --candidate 'RIFE 4.25=.\outputs\rife-4.25.mkv' `
  --candidate 'RIFE 4.26=.\outputs\rife-4.26.mkv' `
  --fps 60 `
  --ffmpeg C:/path/to/ffmpeg.exe --ffprobe C:/path/to/ffprobe.exe `
  --report model-comparison.json
```

多候选报告使用 schema v2：最多支持 64 个候选，报告超过 16 MiB 时会拒绝写入。它记录共享原片/真值哈希，并为每个候选分别记录输出哈希、时间轴、逐帧 PSNR/SSIM 和帧间差分指标。报告同时记录实际使用的 FFmpeg/FFprobe 版本和可执行文件 SHA-256，并在评估期间检查这些文件未改变。每个候选保留自己的最低分复核清单；此外，报告按所有候选在同一时刻中的最低有限 PSNR 选出共用帧/过渡时间点，并记录每个候选在这些相同时间点的指标，供公平的同步画面对比。默认每份清单最多 12 项；`--review-samples 0` 关闭，或设置 0–1000 的上限。候选数较多时，评估器会按报告 10,000 个复核点上限自动降低每候选数量。这些低分清单只用于挑选复核样本，不能自动判断伪影或闪烁。若候选视频旁存在不超过 1 MiB 的 RifeLove 处理报告 `<video>.json`（例如 `result.mkv.json`），评估器会记录模型 ID/版本/哈希、适配器、后端、编码器、GPU 和源/输出路径是否匹配，并检查评估期间 sidecar 哈希未改变；过大的 sidecar 会标记为 `too_large`，不读取。路径与字段仍是报告自述，哈希不认证发布者或模型权重。当前工作树已实现 schema v2 导入与共享时间点盲评，Windows 桌面目标已编译通过；真实报告导入、窗口交互、DPI 和播放节奏仍未验收。工具不会把指标压成单一总分或自动宣布赢家；应结合对应画面做盲评。原有 `--result` 用法继续生成 schema v1 的单结果报告，不能与 `--candidate` 混用。

当前工作树的原生同步比较窗口已加入 schema v1/v2 导入入口：它读取报告所列高帧率参考和候选视频，可切换候选并按时间跳转复核清单。带共用复核点的多候选报告默认随机重排并匿名显示候选，所有候选都在相同时间点复核；盲看时隐藏分数、候选原名、报告文件名和结果路径，确认结束盲评后显示指标及对应关系。旧版 schema v2 报告仍可导入，但没有共用时间点时不启用盲评。桌面导入最多接受 64 个候选、10,000 个复核点和 16 MiB 报告。首次加载会在后台显示进度并以可取消只读租约核对参考与当前候选的 SHA-256；校验期间暂时锁定定位和候选切换。每个候选首次选中时会核对其 SHA-256、读取首个实际呈现时间戳并确认与参考相同；两者应匹配，因为评测报告要求候选和参考逐帧 PTS 完全一致。媒体身份确认后缓存参考和候选的时间轴原点，后续定位复用缓存；已核对候选后续切换或定位时会检查文件身份，并在解码期间保持租约。更换媒体文件后需重新导入报告。2026-11-03 的 `cmake --build build/windows-dev --target rifelove-native-desktop --parallel 4` 编译链接成功；真实报告导入、窗口交互、DPI 和播放帧率仍未验收。

单结果报告记录原片、参考和结果的 SHA-256；多候选报告记录共享原片/参考及各候选结果的 SHA-256。每个结果都包含色彩配置、帧数、目标网格误差、与输出时间戳匹配的原片帧数量和非源采样数量，以及全部帧、源帧和非源采样各自的 PSNR/SSIM 均值、中位数、最小值和最大值。帧间变化指标也按全部过渡、触及非源采样的过渡、源帧到源帧的过渡分别统计。完全匹配帧的 PSNR 为无穷大，会作为 perfect frame 计数，不参与有限数值的均值；共享复核点中若某候选完全匹配，PSNR 字段写为字符串 `perfect`，以保持 JSON 有限数值格式。非源采样可能包括质量保护策略保持的原帧，所以不能直接视为 RIFE 推理帧数。

合成移动方块只用于检查评估链路和已知运动场景，不能代表实拍、动画、游戏录屏或 HDR 的画质验收。PSNR/SSIM 和帧间变化指标都不能替代盲测与时间一致性人工复核。所有输入、计算和报告都留在本机。

## 汇总多场盲评

桌面比较窗口中完成盲评并揭示候选身份后，可把多个最终导出按输出 SHA-256 和完全相同的帧/过渡时间点汇总：

```powershell
py -3 tools/quality/aggregate_reviews.py `
  reviewer-01-revealed.json reviewer-02-revealed.json `
  --output aggregated-review.json
```


如需按评审者场次归并，可从每份 JSON 顶层读取 32 位 `sessionId` 并传入匿名代码；以下示例 ID 需替换为文件中的实际值：

```powershell
py -3 tools/quality/aggregate_reviews.py `
  reviewer-01-revealed.json reviewer-02-revealed.json `
  --reviewer-code 11111111111111111111111111111111=R01 `
  --reviewer-code 22222222222222222222222222222222=R02 `
  --output aggregated-review.json
```
输入必须是 schema v1 的最终身份揭示导出，且仍标记为盲评；匿名导出不含候选映射，不能用于跨报告配对。每个 `sessionId` 只能输入一次。汇总报告保留每个候选、每个时间点的自然度和 1–5 信心度分布、均值/中位数、无法判断数、伪影类别计数及每场原始评分/备注（`reviewerVotes`），并标出已分配和未分配匿名代码的场次数；不合并相邻时间点，也不生成候选总排名。信心度和备注是可选字段，旧导出仍可汇总；盲评备注可能泄露候选身份，桌面输入框会提醒不要写候选名称，备注最多 1,000 个 UTF-16 字符。这里的“场次”不能证明是不同的人。桌面可在首条评分前设置可选匿名评审代号，新导出会写入顶层 reviewerCode，聚合器自动识别；旧导出仍可使用 --reviewer-code 指定。该化名可用于多场汇总，但不能验证真实身份。对于旧导出，可重复传入 `--reviewer-code SESSION_ID=CODE`，用导出中的 `sessionId` 将多个场次标为同一匿名代码；若对已内嵌代号的场次再次指定，代码必须一致；代码限 1–32 位字母、数字、下划线或连字符，并按大小写不敏感处理。未标注场次会计入 `unassignedReviewerSessionCount`；汇总报告保留场次到匿名代码的映射和唯一代码数，但 `uniqueHumanReviewerCount` 仍为 null，因为代码由用户提供且无法验证真实身份。同一代码若对同一候选和时间点出现多次评分，工具会拒绝汇总，避免一个人被重复计票。旧导出若缺少随机种子或原始候选顺序，汇总器会在 `randomizationAudit` 中单独计数。输入文件每份最多 16 MiB、每次最多 256 场、100,000 条评分和 4 MiB 备注；单条备注上限 4 KiB。汇总器与桌面交互仍待运行验证，本轮没有运行评估器。
