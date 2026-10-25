#!/usr/bin/env python3
"""Fill the checked-in Qt catalogs and reject missing or unsafe translations."""

from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CATALOGS = {
    "en": ROOT / "ui/translations/rifelove_en.ts",
    "zh_TW": ROOT / "ui/translations/rifelove_zh_TW.ts",
    "ja": ROOT / "ui/translations/rifelove_ja.ts",
}

_DATA = r"""
尚未选择视频|No video selected|尚未選擇影片|動画が選択されていません
选择原视频|Select source video|選擇原始影片|元動画を選択
视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)|Video files (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)|影片檔案 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)|動画ファイル (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)
所有文件 (*)|All files (*)|所有檔案 (*)|すべてのファイル (*)
选择补帧视频|Select interpolated video|選擇補幀影片|補間済み動画を選択
对比预览|Comparison Viewer|比較預覽|比較プレビュー
同步播放、分屏比较，逐帧检查补帧结果|Synchronized playback, split-screen comparison, and frame-by-frame review|同步播放、分割畫面比較，逐格檢查補幀結果|同期再生、分割比較、フレーム単位で補間結果を確認
本地播放，不会上传视频。步进速度根据补帧视频的帧率读取。|Playback stays on this device. Videos are never uploaded. Frame stepping follows the interpolated video's frame rate.|本機播放，不會上傳影片。步進速度依照補幀影片的影格率讀取。|動画はこの PC で再生され、アップロードされません。コマ送りの間隔は補間動画のフレームレートに従います。
同步播放|Synchronized playback|同步播放|同期再生
原视频|Source video|原始影片|元動画
选择原片|Select source|選擇原片|元動画を選択
补帧视频|Interpolated video|補幀影片|補間済み動画
选择补帧结果|Select interpolated result|選擇補幀結果|補間結果を選択
打开位置|Open location|開啟位置|保存先を開く
选择视频以开始预览|Select videos to start previewing|選擇影片以開始預覽|プレビューする動画を選択してください
暂停|Pause|暫停|一時停止
播放|Play|播放|再生
步进 1/%1 秒|Step by 1/%1 second|每 1/%1 秒步進|1/%1 秒ずつ移動
已按 FFmpeg 实际解码帧精确预览 · 输出时间 %1|Exact preview from FFmpeg-decoded frames · output time %1|已依 FFmpeg 實際解碼影格精確預覽 · 輸出時間 %1|FFmpeg で実際にデコードしたフレームを表示 · 出力時刻 %1
正在精确解码所选时间的原片与补帧画面…|Decoding both videos at the selected timestamp…|正在精確解碼所選時間的原片與補幀畫面…|選択した時刻の元動画と補間動画を正確にデコードしています…
两路视频按时间戳同步；输出帧率 %1 FPS|Both videos are synchronized by timestamp; output rate %1 FPS|兩個影片依時間戳同步；輸出影格率 %1 FPS|2 本の動画をタイムスタンプで同期；出力フレームレート %1 FPS
请选择原视频和补帧结果|Select a source video and an interpolated result|請選擇原始影片和補幀結果|元動画と補間結果を選択してください
请选择本机视频，并输入有效的预览时间。|Select local videos and enter a valid preview time.|請選擇本機影片並輸入有效的預覽時間。|ローカル動画を選択し、有効なプレビュー時刻を入力してください。
只能打开本机文件的位置。|Only local file locations can be opened.|只能開啟本機檔案的位置。|開けるのはローカルファイルの場所だけです。
文件不存在，无法打开所在位置。|The file does not exist, so its location cannot be opened.|找不到檔案，無法開啟所在位置。|ファイルが存在しないため、保存先を開けません。
无法打开此文件的位置。|Could not open this file's location.|無法開啟此檔案的位置。|このファイルの保存先を開けませんでした。
等待处理|Queued|等待處理|待機中
正在处理|Processing|正在處理|処理中
已暂停|Paused|已暫停|一時停止
已中断|Interrupted|已中斷|中断
失败|Failed|失敗|失敗
已取消|Cancelled|已取消|キャンセル済み
已完成|Completed|已完成|完了
未知状态|Unknown status|未知狀態|不明な状態
RIFELOVE · 本机工作区|RIFELOVE · LOCAL WORKSPACE|RIFELOVE · 本機工作區|RIFELOVE · ローカル ワークスペース
让每一帧运动都更连贯|Make every movement flow smoothly|讓每一格動作都更連貫|すべての動きをなめらかに
选择视频开始处理，或继续查看本机任务。原片保持不变，模型和输出都留在本机。|Choose a video to process, or review local tasks. Originals stay untouched; models and output remain on this device.|選擇影片開始處理，或繼續查看本機任務。原片保持不變，模型和輸出都留在本機。|動画を選んで処理するか、ローカルのタスクを確認できます。元動画は変更せず、モデルと出力はこの PC に保存します。
导入视频|Import video|匯入影片|動画を読み込む
打开工作室|Open Studio|開啟工作室|スタジオを開く
当前工作状态|Current status|目前工作狀態|現在の状態
本机处理|Local processing|本機處理|ローカル処理
视频仅在本机处理；自动模式会按驱动和源格式选择兼容的本地编码器。|Videos are processed locally. Auto mode selects a compatible local encoder for your driver and source format.|影片只在本機處理；自動模式會依照驅動程式和來源格式選擇相容的本機編碼器。|動画はこの PC で処理します。自動モードではドライバーとソース形式に合うローカルエンコーダーを選びます。
NVIDIA GPU|NVIDIA GPU|NVIDIA GPU|NVIDIA GPU
尚未检测到 GPU|No GPU detected|尚未偵測到 GPU|GPU が検出されていません
显存 %1 GB · CUDA %2|%1 GB VRAM · CUDA %2|顯示記憶體 %1 GB · CUDA %2|VRAM %1 GB · CUDA %2
需要可用的 NVIDIA 驱动|A working NVIDIA driver is required|需要可用的 NVIDIA 驅動程式|NVIDIA ドライバーが必要です
本地模型|Local model|本機模型|ローカルモデル
尚未选择模型|No model selected|尚未選擇模型|モデルが選択されていません
已通过本机文件校验|Local file integrity verified|已通過本機檔案完整性檢查|ローカルファイルの整合性を確認済み
正在校验本地模型…|Verifying local model…|正在驗證本機模型…|ローカルモデルを検証しています…
请在模型中心检查此模型|Check this model in Model Center|請在模型中心檢查此模型|モデルセンターでこのモデルを確認してください
任务队列|Task queue|任務佇列|タスク キュー
%1 项任务|%1 tasks|%1 個任務|%1 件のタスク
等待 %1 · 处理中 %2 · 已完成 %3|Queued %1 · Processing %2 · Completed %3|等待 %1 · 處理中 %2 · 已完成 %3|待機 %1 · 処理中 %2 · 完了 %3
最近任务|Recent tasks|最近任務|最近のタスク
查看全部|View all|查看全部|すべて表示
这里会显示最近加入队列的视频。|Videos recently added to the queue will appear here.|最近加入佇列的影片會顯示在這裡。|最近キューに追加した動画がここに表示されます。
本地视频|Local video|本機影片|ローカル動画
%1 · %2 / 约 %3 帧|%1 · %2 / about %3 frames|%1 · %2 / 約 %3 格|%1 · %2 / 約 %3 フレーム
目标 %1 FPS|Target %1 FPS|目標 %1 FPS|目標 %1 FPS
模型中心|Model Center|模型中心|モデルセンター
模型校验失败|Model verification failed|模型驗證失敗|モデルの検証に失敗しました
本地模型未校验|Local model not verified|本機模型尚未驗證|ローカルモデルは未検証です
软件 H.264|Software H.264|軟體 H.264|ソフトウェア H.264
软件 HEVC|Software HEVC|軟體 HEVC|ソフトウェア HEVC
软件 AV1|Software AV1|軟體 AV1|ソフトウェア AV1
兼容的软件编码器|Compatible software encoder|相容的軟體編碼器|互換性のあるソフトウェア エンコーダー
导入后会检查本机的 GPU 与视频编码能力。|After import, RifeLove checks local GPU and video encoding capabilities.|匯入後會檢查本機 GPU 與影片編碼能力。|読み込み後、ローカル GPU と動画エンコード機能を確認します。
所选模型没有声明支持这个 10-bit SDR 输入配置。|The selected model does not declare support for this 10-bit SDR input profile.|所選模型未宣告支援此 10-bit SDR 輸入設定檔。|選択したモデルは、この 10-bit SDR 入力プロファイルをサポートしていません。
NVENC 恒定质量 CQ 可用；画质参数由 NVIDIA 视频编码器处理。|NVENC constant-quality (CQ) is available; quality is controlled by the NVIDIA video encoder.|NVENC 固定品質 CQ 可用；畫質參數由 NVIDIA 影片編碼器處理。|NVENC の固定品質 CQ を使用できます。品質は NVIDIA ビデオ エンコーダーで設定します。
当前源视频无法初始化 NVENC；请切换到自动编码或软件编码。|NVENC could not be initialized for this source. Switch to Auto or software encoding.|目前來源影片無法初始化 NVENC；請改用自動或軟體編碼。|このソース動画では NVENC を初期化できません。自動またはソフトウェア エンコードに切り替えてください。
NVENC 当前不可用；自动模式会用%1编码，RIFE 仍由 CUDA 推理。|NVENC is unavailable. Auto mode will encode with %1; RIFE inference will still run on CUDA.|NVENC 目前無法使用；自動模式會使用%1編碼，RIFE 仍由 CUDA 推論。|NVENC は利用できません。自動モードでは%1でエンコードし、RIFE 推論は CUDA で続行します。
NVENC 可以初始化；最终以实际导出结果为准。|NVENC initialized successfully; the final export confirms actual encoding.|NVENC 已成功初始化；實際編碼結果仍以匯出檔案為準。|NVENC を初期化しました。実際のエンコード結果は書き出した動画で確認してください。
没有兼容的自动编码器，请检查本机视频运行环境。|No compatible automatic encoder is available. Check the local video runtime.|沒有相容的自動編碼器，請檢查本機影片執行環境。|互換性のある自動エンコーダーがありません。ローカルの動画環境を確認してください。
色彩格式暂不支持|Color format not supported|暫不支援此色彩格式|色形式は未対応です
NVIDIA GPU 不可用|NVIDIA GPU unavailable|NVIDIA GPU 無法使用|NVIDIA GPU を利用できません
请先在模型中心选择本地模型|Select a local model in Model Center first|請先在模型中心選擇本機模型|先にモデルセンターでローカルモデルを選択してください
正在校验所选本地模型…|Verifying selected local model…|正在驗證所選本機模型…|選択したローカルモデルを検証しています…
所选模型未通过本机校验，请在模型中心检查|The selected model failed local verification. Check it in Model Center.|所選模型未通過本機驗證，請在模型中心檢查|選択したモデルはローカル検証に失敗しました。モデルセンターで確認してください。
所选模型不支持此输入配置|The selected model does not support this input profile|所選模型不支援此輸入設定檔|選択したモデルはこの入力プロファイルに対応していません
所选编码器不可用|Selected encoder unavailable|所選編碼器無法使用|選択したエンコーダーは利用できません
请配置本地推理运行库|Configure the local inference runtime|請設定本機推論執行階段|ローカル推論ランタイムを設定してください
可以开始|Ready to start|可以開始|開始できます
等待导入|Waiting for import|等待匯入|読み込み待ち
读取视频信息|Reading video information|正在讀取影片資訊|動画情報を読み取っています
校验源文件|Verifying source file|正在驗證來源檔案|ソースファイルを検証しています
等待处理引擎|Waiting for processing engine|等待處理引擎|処理エンジンを待機しています
加载本地模型|Loading local model|正在載入本機模型|ローカルモデルを読み込んでいます
正在补帧|Interpolating frames|正在補幀|フレーム補間中
正在编码|Encoding video|正在編碼|動画をエンコードしています
验证输出视频|Validating output video|正在驗證輸出影片|出力動画を検証しています
切换编码器|Switching encoder|正在切換編碼器|エンコーダーを切り替えています
处理完成|Processing complete|處理完成|処理が完了しました
处理失败|Processing failed|處理失敗|処理に失敗しました
正在安全取消|Cancelling safely|正在安全取消|安全にキャンセルしています
已取消|Cancelled|已取消|キャンセル済み
处理组件未启用|Processing components are not enabled|處理元件未啟用|処理コンポーネントが有効ではありません
选择视频|Select video|選擇影片|動画を選択
保存补帧视频|Save interpolated video|儲存補幀影片|補間済み動画を保存
选择 ONNX CUDA 运行库目录|Select ONNX CUDA runtime folder|選擇 ONNX CUDA 執行階段資料夾|ONNX CUDA ランタイム フォルダーを選択
正在处理视频|Processing video|正在處理影片|動画を処理しています
取消导入或当前任务，并等待资源清理后，可关闭窗口。|Cancel the import or current task, then wait for resources to be released before closing.|取消匯入或目前任務，並等待資源清理後再關閉視窗。|読み込みまたは現在のタスクをキャンセルし、リソースの解放を待ってからウィンドウを閉じてください。
离线视频工作台|Offline video workspace|離線影片工作區|オフライン動画ワークスペース
工作区|Workspace|工作區|ワークスペース
本机加速器|Local accelerator|本機加速器|ローカル アクセラレーター
未检测到 NVIDIA GPU|No NVIDIA GPU detected|未偵測到 NVIDIA GPU|NVIDIA GPU が検出されていません
NVENC 不可用 · 自动使用%1|NVENC unavailable · using %1 automatically|NVENC 無法使用 · 自動改用%1|NVENC は利用不可 · 自動的に%1を使用
NVENC 编码器可初始化|NVENC encoder initialized|NVENC 編碼器已初始化|NVENC エンコーダーを初期化しました
H.264 编码器不可用|H.264 encoder unavailable|H.264 編碼器無法使用|H.264 エンコーダーは利用できません
源文件不会被修改|Source files are never modified|來源檔案不會被修改|ソースファイルは変更されません
让每一段运动都更流畅|Make every movement smoother|讓每一段動作都更流暢|すべての動きをよりなめらかに
选择视频与目标帧率，RifeLove 会保留原片并生成新文件。|Choose a video and target frame rate. RifeLove keeps the original and creates a new file.|選擇影片與目標影格率，RifeLove 會保留原片並建立新檔案。|動画と目標フレームレートを選択してください。RifeLove は元動画を保持し、新しいファイルを作成します。
选择文件|Choose file|選擇檔案|ファイルを選択
将一个视频拖到这里，或选择文件|Drop a video here or choose a file|將影片拖曳到此處，或選擇檔案|ここに動画をドロップするか、ファイルを選択してください
目标帧率|Target frame rate|目標影格率|目標フレームレート
编码器|Encoder|編碼器|エンコーダー
自动编码（优先 NVENC）|Automatic encoding (prefer NVENC)|自動編碼（優先使用 NVENC）|自動エンコード（NVENC 優先）
H.264 软件|H.264 software|H.264 軟體|H.264 ソフトウェア
HEVC 软件|HEVC software|HEVC 軟體|HEVC ソフトウェア
AV1 软件|AV1 software|AV1 軟體|AV1 ソフトウェア
NVIDIA H.264（NVENC）|NVIDIA H.264 (NVENC)|NVIDIA H.264（NVENC）|NVIDIA H.264（NVENC）
输出文件|Output file|輸出檔案|出力ファイル
选择位置|Choose location|選擇位置|保存先を選択
导入视频后自动生成新文件名|A new file name is generated after importing a video|匯入影片後會自動產生新檔名|動画の読み込み後に新しいファイル名を自動生成
收起高级编码|Hide advanced encoding|收合進階編碼|詳細なエンコード設定を閉じる
高级编码设置|Advanced encoding settings|進階編碼設定|詳細なエンコード設定
高级编码|Advanced encoding|進階編碼|詳細なエンコード設定
码率控制|Rate control|位元率控制|レート制御
目标码率|Target bitrate|目標位元率|目標ビットレート
恒定量化 QP|Constant quantization (QP)|固定量化 QP|固定量子化 QP
恒定质量 CRF|Constant quality (CRF)|固定品質 CRF|固定品質 CRF
恒定质量 CQ|Constant quality (CQ)|固定品質 CQ|固定品質 CQ
QP（0–51）|QP (0–51)|QP（0–51）|QP（0～51）
CQ（0–51）|CQ (0–51)|CQ（0–51）|CQ（0～51）
CRF（0–63）|CRF (0–63)|CRF（0–63）|CRF（0～63）
目标质量|Target quality|目標品質|目標品質
目标码率（kbit/s）|Target bitrate (kbit/s)|目標位元率（kbit/s）|目標ビットレート（kbit/s）
关键帧间隔（帧）|Keyframe interval (frames)|關鍵影格間隔（格）|キーフレーム間隔（フレーム）
HEVC 恒定量化使用 Kvazaar QP；较低 QP 通常会生成更大的文件。|HEVC constant quantization uses Kvazaar QP; lower QP usually creates larger files.|HEVC 固定量化使用 Kvazaar QP；較低的 QP 通常會產生較大的檔案。|HEVC の固定量子化では Kvazaar QP を使用します。QP が低いほど、通常はファイルが大きくなります。
AV1 恒定质量使用 CRF；较低 CRF 通常会生成更大的文件。|AV1 constant quality uses CRF; lower CRF usually creates larger files.|AV1 固定品質使用 CRF；較低的 CRF 通常會產生較大的檔案。|AV1 の固定品質では CRF を使用します。CRF が低いほど、通常はファイルが大きくなります。
NVENC 恒定质量使用 VBR + CQ（0–51），需要可用的 NVIDIA 视频编码驱动；较低 CQ 通常会生成更大的文件。|NVENC constant quality uses VBR + CQ (0–51) and requires a working NVIDIA video-encoding driver; lower CQ usually creates larger files.|NVENC 固定品質使用 VBR + CQ（0–51），需要可用的 NVIDIA 影片編碼驅動程式；較低的 CQ 通常會產生較大的檔案。|NVENC の固定品質は VBR + CQ（0～51）を使用し、利用可能な NVIDIA ビデオ エンコード ドライバーが必要です。CQ が低いほど、通常はファイルが大きくなります。
自动模式按驱动与源像素格式选择 NVENC、软件 H.264、HEVC 或 AV1，并尽量保留源位深。|Auto mode selects NVENC, software H.264, HEVC, or AV1 according to the driver and source pixel format, while preserving source bit depth where possible.|自動模式會依照驅動程式與來源像素格式選擇 NVENC、軟體 H.264、HEVC 或 AV1，並盡可能保留來源位元深度。|自動モードはドライバーとソースのピクセル形式に応じて NVENC、ソフトウェア H.264、HEVC、AV1 を選び、可能な限り元のビット深度を維持します。
收起模型与运行库设置|Hide model and runtime settings|收合模型與執行階段設定|モデルとランタイムの設定を閉じる
高级设置 · 模型与运行库|Advanced settings · model and runtime|進階設定 · 模型與執行階段|詳細設定 · モデルとランタイム
本地模型与运行库|Local model and runtime|本機模型與執行階段|ローカルモデルとランタイム
模型包|Model pack|模型套件|モデルパック
尚未选择已校验模型|No verified model selected|尚未選擇已驗證模型|検証済みモデルが選択されていません
打开模型中心|Open Model Center|開啟模型中心|モデルセンターを開く
运行库|Runtime|執行階段|ランタイム
包含 ONNX Runtime / CUDA DLL 的目录|Folder containing ONNX Runtime / CUDA DLLs|包含 ONNX Runtime / CUDA DLL 的資料夾|ONNX Runtime / CUDA DLL を含むフォルダー
浏览|Browse|瀏覽|参照
模型必须先在本地模型中心通过完整性校验，再设为默认模型。色彩路径支持明确标记的 BT.709 与 BT.470BG/SMPTE 170M 8/10-bit SDR；HDR、其他位深和未标记色彩的视频会被拦截。模型和运行库只从本机目录读取。|Verify a model's integrity in the local Model Center before setting it as default. The color path accepts explicitly tagged BT.709 and BT.470BG/SMPTE 170M 8/10-bit SDR. HDR, other bit depths, and untagged color are rejected. Models and runtimes are read only from local folders.|模型必須先在本機模型中心通過完整性檢查，才能設為預設模型。色彩路徑支援明確標記的 BT.709 與 BT.470BG/SMPTE 170M 8/10-bit SDR；HDR、其他位元深度及未標記色彩的影片會被拒絕。模型與執行階段只會從本機資料夾讀取。|ローカルのモデルセンターで整合性を確認してから既定モデルに設定してください。色処理は明示的にタグ付けされた BT.709 および BT.470BG/SMPTE 170M の 8/10-bit SDR に対応します。HDR、その他のビット深度、色タグのない動画は拒否されます。モデルとランタイムはローカルフォルダーからのみ読み込みます。
CPU H.264|CPU H.264|CPU H.264|CPU H.264
CPU HEVC|CPU HEVC|CPU HEVC|CPU HEVC
CPU AV1|CPU AV1|CPU AV1|CPU AV1
CPU 软件|CPU software|CPU 軟體|CPU ソフトウェア
%1 / 约 %2 帧|%1 / about %2 frames|%1 / 約 %2 格|%1 / 約 %2 フレーム
%1 帧|%1 frames|%1 格|%1 フレーム
 处理 FPS| processing FPS| 處理 FPS| 処理 FPS
%1 编码 · CUDA RIFE 插帧|%1 encoding · CUDA RIFE interpolation|%1 編碼 · CUDA RIFE 補幀|%1 エンコード · CUDA RIFE 補間
剩余约 %1|About %1 remaining|剩餘約 %1|残り約 %1
预计剩余时间计算中|Calculating estimated time remaining|正在計算預估剩餘時間|残り時間を計算しています
取消|Cancel|取消|キャンセル
开始补帧|Start interpolation|開始補幀|補間を開始
累计耗时：RIFE 推理 %1 秒 · 视频编码 %2 秒 · 解码 %3 秒 · 预处理/色彩 %4 秒|Elapsed: RIFE inference %1 s · video encoding %2 s · decoding %3 s · preprocessing/color %4 s|累計耗時：RIFE 推論 %1 秒 · 影片編碼 %2 秒 · 解碼 %3 秒 · 前處理/色彩 %4 秒|経過時間：RIFE 推論 %1 秒 · 動画エンコード %2 秒 · デコード %3 秒 · 前処理/色調 %4 秒
 · 输出验证 %1 秒| · output validation %1 s| · 輸出驗證 %1 秒| · 出力検証 %1 秒
请使用启用媒体和 ONNX CUDA 的桌面构建。|Use a desktop build with media and ONNX CUDA enabled.|請使用已啟用媒體與 ONNX CUDA 的桌面建置版本。|メディアと ONNX CUDA を有効にしたデスクトップ ビルドを使用してください。
本地模型中心|Local Model Center|本機模型中心|ローカル モデルセンター
重新校验|Verify again|重新驗證|再検証
添加本地模型|Add local model|新增本機模型|ローカルモデルを追加
正在后台读取并校验模型包；不会阻塞视频工作流界面。|Reading and verifying model packs in the background; video workflow remains responsive.|正在背景讀取並驗證模型套件；不會阻塞影片工作流程介面。|バックグラウンドでモデルパックを読み込み、検証しています。動画ワークフロー画面は引き続き操作できます。
RIFE 模型|RIFE model|RIFE 模型|RIFE モデル
默认模型|Default model|預設模型|既定モデル
不可用|Unavailable|無法使用|利用不可
ONNX SHA-256\s\s|ONNX SHA-256\s\s|ONNX SHA-256\s\s|ONNX SHA-256\s\s
许可\s\s|License\s\s|授權條款\s\s|ライセンス\s\s
未声明|Not declared|未宣告|未宣言
数值档案\s\s|Numerical profile\s\s|數值設定檔\s\s|数値プロファイル\s\s
未记录|Not recorded|未記錄|未記録
HDR\s\s|HDR\s\s|HDR\s\s|HDR\s\s
已验证|Verified|已驗證|検証済み
未验证|Not verified|未驗證|未検証
已记录的验证后端\s\s|Recorded validation backends\s\s|已記錄的驗證後端\s\s|記録された検証バックエンド\s\s
上游来源声明\s\s|Upstream source declaration\s\s|上游來源聲明\s\s|上流ソースの宣言\s\s
许可范围依据\s\s|License scope basis\s\s|授權範圍依據\s\s|ライセンス範圍の根拠\s\s
完整性校验通过：权重、许可证、来源清单和验证报告的 SHA-256 均匹配。此校验不代表作者签名或画质保证。|Integrity check passed: the weights, license, source manifest, and validation report match their SHA-256 values. This does not authenticate the author or guarantee image quality.|完整性檢查通過：權重、授權條款、來源清單與驗證報告的 SHA-256 均相符。此檢查不代表作者簽章或畫質保證。|整合性チェックに合格しました。重み、ライセンス、ソースマニフェスト、検証レポートの SHA-256 が一致します。作者の認証や画質を保証するものではありません。
旧版清单：权重 SHA-256 已校验；许可证、来源清单和验证报告未绑定 SHA-256。|Legacy manifest: weight SHA-256 verified; the license, source manifest, and validation report are not bound by SHA-256.|舊版清單：權重 SHA-256 已驗證；授權條款、來源清單與驗證報告未綁定 SHA-256。|旧形式のマニフェスト：重みの SHA-256 は検証済みです。ライセンス、ソースマニフェスト、検証レポートは SHA-256 に紐付いていません。
模型包无法读取。|Could not read model pack.|無法讀取模型套件。|モデルパックを読み込めませんでした。
设为默认模型|Set as default model|設為預設模型|既定モデルに設定
从目录列表移除|Remove from catalog|從清單移除|カタログから削除
队列忙碌或尚未连接，请等待当前操作结束。|The queue is busy or disconnected. Wait for the current operation to finish.|佇列忙碌或尚未連線，請等待目前作業完成。|キューが処理中または未接続です。現在の操作が終わるまでお待ちください。
本地任务队列尚未连接，无法导出报告。|The local task queue is not connected; the report cannot be exported.|本機任務佇列尚未連線，無法匯出報告。|ローカル タスク キューに接続されていないため、レポートを出力できません。
正在校验本地模型及 SHA-256…|Verifying local model and SHA-256…|正在驗證本機模型與 SHA-256…|ローカルモデルと SHA-256 を検証しています…
队列忙碌或本地模型校验正在运行。|The queue is busy or local model verification is in progress.|佇列忙碌或本機模型正在驗證。|キューが処理中、またはローカルモデルを検証中です。
只能添加本机目录中的模型包。|Only model packs in local folders can be added.|只能新增本機資料夾中的模型套件。|ローカルフォルダーにあるモデルパックのみ追加できます。
正在验证模型描述、许可证和 ONNX SHA-256…|Verifying model descriptor, license, and ONNX SHA-256…|正在驗證模型描述檔、授權條款與 ONNX SHA-256…|モデル記述子、ライセンス、ONNX SHA-256 を検証しています…
正在更新本地模型列表…|Updating local model catalog…|正在更新本機模型清單…|ローカルモデルカタログを更新しています…
默认模型必须位于本机目录。|The default model must be in a local folder.|預設模型必須位於本機資料夾。|既定モデルはローカルフォルダーに配置してください。
请先校验该模型包，再将其设为默认模型。|Verify this model pack before setting it as the default.|請先驗證此模型套件，再設為預設模型。|既定モデルにする前に、このモデルパックを検証してください。
加载模型|Loading model|載入模型|モデルを読み込み中
AI 补帧|AI interpolation|AI 補幀|AI フレーム補間
读取已完成片段|Reading completed segments|讀取已完成片段|完了済みセグメントを読み込み中
编码视频|Encoding video|正在編碼影片|動画をエンコード中
验证输出|Validating output|正在驗證輸出|出力を検証中
校验发布文件|Verifying published file|正在驗證已發布檔案|公開済みファイルを検証中
安全暂停点|Safe pause point|安全暫停點|安全に一時停止できる位置
批量选择视频|Select videos in batch|批次選擇影片|動画をまとめて選択
视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.m2ts *.wmv *.flv *.mxf *.ogv *.vob *.3gp *.asf *.gif)|Video files (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.m2ts *.wmv *.flv *.mxf *.ogv *.vob *.3gp *.asf *.gif)|影片檔案 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.m2ts *.wmv *.flv *.mxf *.ogv *.vob *.3gp *.asf *.gif)|動画ファイル (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.m2ts *.wmv *.flv *.mxf *.ogv *.vob *.3gp *.asf *.gif)
导出队列报告|Export queue report|匯出佇列報告|キューレポートを出力
JSON 报告 (*.json)|JSON report (*.json)|JSON 報告 (*.json)|JSON レポート (*.json)
选择包含视频的文件夹|Select a folder containing videos|選擇包含影片的資料夾|動画を含むフォルダーを選択
选择输出文件夹|Select output folder|選擇輸出資料夾|出力フォルダーを選択
选择已校验的本地 RIFE 模型包|Select a verified local RIFE model pack|選擇已驗證的本機 RIFE 模型套件|検証済みのローカル RIFE モデルパックを選択
选择 ONNX Runtime CUDA 运行库目录|Select ONNX Runtime CUDA runtime folder|選擇 ONNX Runtime CUDA 執行階段資料夾|ONNX Runtime CUDA ランタイム フォルダーを選択
批量导入并持久保存任务。已完成片段会在本机校验和保存；中断后可继续，也可从头重启。|Import multiple files and save tasks locally. Completed segments are verified and kept on this device; interrupted jobs can resume or restart.|批次匯入並持續保存任務。已完成片段會在本機驗證並保存；中斷後可繼續或從頭重啟。|複数の動画を読み込み、タスクをローカルに保存します。完了したセグメントは検証してこの PC に保存し、中断後は続行または最初から再開できます。
重新连接|Reconnect|重新連線|再接続
导入到队列|Import into queue|匯入至佇列|キューに読み込む
选择多个视频|Select multiple videos|選擇多個影片|複数の動画を選択
选择文件夹|Select folder|選擇資料夾|フォルダーを選択
拖入视频文件或文件夹；自动检查媒体轨道和模型支持的色彩格式。重复文件、已有队列输出和输出目录中的已生成文件会跳过。|Drop video files or folders. Media streams and model color support are checked automatically. Duplicate files, queued outputs, and existing outputs are skipped.|拖曳影片檔案或資料夾；系統會自動檢查媒體軌道與模型支援的色彩格式。重複檔案、佇列中已有的輸出，以及輸出資料夾內已存在的檔案都會略過。|動画ファイルまたはフォルダーをドロップしてください。メディアストリームとモデルの色形式を自動確認します。重複ファイル、キュー内の出力先、既存の出力ファイルはスキップします。
目标 FPS|Target FPS|目標 FPS|目標 FPS
输出容器|Output container|輸出容器|出力コンテナー
输出位置|Output location|輸出位置|出力先
留空时使用首个来源文件夹|If blank, use the first source folder|留空時使用第一個來源資料夾|空欄の場合は最初のソースフォルダーを使用
选择|Choose|選擇|選択
清除|Clear|清除|クリア
收起模型与运行库|Hide model and runtime|收合模型與執行階段|モデルとランタイムを閉じる
选择 model.json 所在的已校验目录|Select the verified folder containing model.json|選擇含有 model.json 的已驗證資料夾|model.json を含む検証済みフォルダーを選択
CUDA 运行库|CUDA runtime|CUDA 執行階段|CUDA ランタイム
选择 ONNX Runtime 与 CUDA DLL 目录|Select the folder containing ONNX Runtime and CUDA DLLs|選擇含有 ONNX Runtime 與 CUDA DLL 的資料夾|ONNX Runtime と CUDA DLL を含むフォルダーを選択
扫描子文件夹|Scan subfolders|掃描子資料夾|サブフォルダーを検索
取消导入|Cancel import|取消匯入|読み込みをキャンセル
加入队列|Add to queue|加入佇列|キューに追加
%1 项|%1 items|%1 個項目|%1 件
等待 %1 · 运行 %2 · 完成 %3 · 失败 %4|Queued %1 · Running %2 · Completed %3 · Failed %4|等待 %1 · 執行中 %2 · 完成 %3 · 失敗 %4|待機 %1 · 実行中 %2 · 完了 %3 · 失敗 %4
导出报告|Export report|匯出報告|レポートを出力
开始队列|Start queue|開始佇列|キューを開始
继续|Resume|繼續|再開
停止队列|Stop queue|停止佇列|キューを停止
 FPS| FPS| FPS| FPS
模型：|Model: |模型：|モデル：
 · AI %1 帧 · %2 FPS| · AI %1 frames · %2 FPS| · AI %1 格 · %2 FPS| · AI %1 フレーム · %2 FPS
 · 剩余约 %1| · about %1 remaining| · 剩餘約 %1| · 残り約 %1
 · 预计剩余时间计算中| · calculating estimated time remaining| · 正在計算預估剩餘時間| · 残り時間を計算しています
取消任务|Cancel task|取消任務|タスクをキャンセル
重试|Retry|重試|再試行
从检查点继续|Resume from checkpoint|從檢查點繼續|チェックポイントから再開
从头重启|Restart from beginning|從頭重啟|最初から再開
打开输出位置|Open output location|開啟輸出位置|出力先を開く
队列为空\n导入多个视频或整个文件夹开始处理|The queue is empty\nImport multiple videos or a folder to get started|佇列是空的\n匯入多個影片或整個資料夾以開始處理|キューは空です\n複数の動画またはフォルダーを読み込んで開始してください
已校验并自动恢复 %1 个已发布结果。此处的恢复不需要重新补帧。|Verified and recovered %1 published results. No frame interpolation was needed for this recovery.|已驗證並自動復原 %1 個已發布結果。此次復原不需要重新補幀。|公開済みの結果 %1 件を検証して復元しました。この復元ではフレーム補間をやり直していません。
发现 %1 个仍中断的任务。可校验已完成片段后继续，也可选择新文件名从头生成。|Found %1 interrupted tasks. Verify completed segments to resume, or choose a new file name to restart.|發現 %1 個仍中斷的任務。可驗證已完成片段後繼續，或選擇新檔名從頭產生。|中断したタスクが %1 件あります。完了済みセグメントを検証して続行するか、新しいファイル名を選んで最初から生成できます。
任务库暂不可用：|Task database unavailable: |任務資料庫目前無法使用：|タスク データベースを利用できません：
任务已重新加入队列。|Task added to the queue again.|任務已重新加入佇列。|タスクをキューに再追加しました。
已校验检查点并加入队列。开始后会从未完成片段继续。|Checkpoint verified and added to the queue. Processing will resume from the unfinished segment.|已驗證檢查點並加入佇列。開始後會從未完成片段繼續。|チェックポイントを検証してキューに追加しました。開始すると未完了のセグメントから続行します。
已加入从头生成的任务：|Added task to restart from the beginning: |已加入從頭產生的任務：|最初から生成するタスクを追加しました：
队列报告已保存：|Queue report saved: |佇列報告已儲存：|キューレポートを保存しました：
已校验并加入本地模型：|Local model verified and added: |已驗證並加入本機模型：|ローカルモデルを検証して追加しました：
正在检查视频 %1 / %2|Checking video %1 / %2|正在檢查影片 %1 / %2|動画を確認しています %1 / %2
视频色彩标签、像素格式或位深不在支持范围内。|The video's color tags, pixel format, or bit depth are not supported.|影片色彩標籤、像素格式或位元深度不在支援範圍內。|動画の色タグ、ピクセル形式、またはビット深度はサポートされていません。
当前模型未声明支持这个视频输入配置。|The selected model does not declare support for this video input profile.|目前模型未宣告支援此影片輸入設定檔。|選択したモデルはこの動画入力プロファイルをサポートしていません。
所选视频编码器无法初始化：|Could not initialize the selected video encoder: |所選影片編碼器無法初始化：|選択したビデオ エンコーダーを初期化できませんでした：
文件中没有视频轨。|The file contains no video stream.|檔案中沒有影片軌。|ファイルに動画ストリームがありません。
无法读取媒体：|Could not read media: |無法讀取媒體：|メディアを読み込めませんでした：
没有找到当前模型可处理的视频。\n|No video was found that the selected model can process.\n|找不到目前模型可處理的影片。\n|選択したモデルで処理できる動画が見つかりません。\n
已加入 %1 个任务。输出使用新文件名。|Added %1 tasks. Outputs will use new file names.|已加入 %1 個任務。輸出會使用新檔名。|タスクを %1 件追加しました。出力には新しいファイル名を使用します。
\n跳过 %1 个不支持的文件：\n|\nSkipped %1 unsupported files:\n|\n略過 %1 個不支援的檔案：\n|\n未対応ファイルを %1 件スキップしました：\n
\n仅显示前 50 条详情。|\nShowing the first 50 details only.|\n僅顯示前 50 筆詳細資料。|\n詳細は最初の 50 件のみ表示しています。
导入已取消，未加入部分任务。|Import cancelled; no partial tasks were added.|匯入已取消，未加入部分任務。|読み込みをキャンセルしました。タスクは一部だけ追加されていません。
队列已停止。已排队但未运行的任务会保留；中断任务可校验片段后继续或从头重启。|Queue stopped. Queued but unstarted tasks are kept; interrupted tasks can resume after segment verification or restart from the beginning.|佇列已停止。已排隊但尚未執行的任務會保留；中斷的任務可在驗證片段後繼續，或從頭重啟。|キューを停止しました。未開始のタスクは保持されます。中断したタスクはセグメントを検証して続行するか、最初から再開できます。
默认设置未保存。|Default settings were not saved.|預設設定未儲存。|既定の設定を保存できませんでした。
高级编码参数超出范围或与当前编码器不兼容，默认设置未保存。|Advanced encoding values are out of range or incompatible with the selected encoder. Defaults were not saved.|進階編碼參數超出範圍或與目前編碼器不相容，預設設定未儲存。|詳細なエンコード設定が範囲外か、選択したエンコーダーと互換性がありません。既定値は保存されませんでした。
任务可继续使用，但默认设置未能保存。|The task can continue, but default settings could not be saved.|任務可繼續使用，但預設設定未能儲存。|タスクは続行できますが、既定の設定を保存できませんでした。
NVENC 驱动接口不兼容：当前提供 API %1，FFmpeg 需要 API %2（驱动 %3 或更新）。现在由 CPU 使用%4编码，RIFE 仍由 CUDA 插帧。|NVENC driver API mismatch: this system provides API %1, while FFmpeg requires API %2 (driver %3 or newer). CPU %4 encoding is used; RIFE interpolation remains on CUDA.|NVENC 驅動 API 不相容：目前提供 API %1，FFmpeg 需要 API %2（驅動程式 %3 或更新版本）。現在由 CPU 使用%4編碼，RIFE 仍由 CUDA 補幀。|NVENC ドライバー API の不一致：この PC は API %1 を提供していますが、FFmpeg には API %2（ドライバー %3 以降）が必要です。CPU で%4エンコードし、RIFE 補間は CUDA で続行します。
当前 NVIDIA 驱动无法初始化此 FFmpeg 的 NVENC 接口。现在由 CPU 使用%1编码，RIFE 仍由 CUDA 插帧。|The current NVIDIA driver cannot initialize this FFmpeg NVENC interface. CPU %1 encoding is used; RIFE interpolation remains on CUDA.|目前 NVIDIA 驅動程式無法初始化此 FFmpeg 的 NVENC 介面。現在由 CPU 使用%1編碼，RIFE 仍由 CUDA 補幀。|現在の NVIDIA ドライバーでは、この FFmpeg の NVENC インターフェースを初期化できません。CPU で%1エンコードし、RIFE 補間は CUDA で続行します。
NVENC 未能启动，现在由 CPU 使用%1编码；RIFE 插帧仍由 CUDA 运行。|NVENC did not start. CPU %1 encoding is used; RIFE interpolation still runs on CUDA.|NVENC 無法啟動，目前由 CPU 使用%1編碼；RIFE 補幀仍由 CUDA 執行。|NVENC を起動できませんでした。CPU で%1エンコードし、RIFE 補間は CUDA で続行します。
请选择本机视频文件。|Select a local video file.|請選擇本機影片檔案。|ローカル動画ファイルを選択してください。
请等待当前操作结束。|Wait for the current operation to finish.|請等待目前作業完成。|現在の操作が終わるまでお待ちください。
请选择本机输出位置。|Choose a local output location.|請選擇本機輸出位置。|ローカルの出力先を選択してください。
TensorRT 当前不可用，已回退到 ONNX Runtime CUDA 推理。|TensorRT is unavailable; inference has fallen back to ONNX Runtime CUDA.|TensorRT 目前無法使用，已回退至 ONNX Runtime CUDA 推論。|TensorRT を利用できないため、ONNX Runtime CUDA 推論に切り替えました。
未知|Unknown|未知|不明
NVENC 当前无法初始化；自动编码将使用%1，RIFE 插帧仍使用 GPU。|NVENC cannot initialize; Auto will use %1 for encoding while RIFE interpolation continues on the GPU.|NVENC 目前無法初始化；自動編碼會使用%1，RIFE 補幀仍使用 GPU。|NVENC を初期化できません。自動エンコードでは%1を使用し、RIFE 補間は GPU で続行します。
NVENC 和兼容的软件编码器均未能初始化；请检查本机视频编码运行环境。|Neither NVENC nor a compatible software encoder could be initialized. Check the local video-encoding runtime.|NVENC 與相容的軟體編碼器皆無法初始化；請檢查本機影片編碼執行環境。|NVENC と互換性のあるソフトウェア エンコーダーを初期化できませんでした。ローカルの動画エンコード環境を確認してください。
HEVC|HEVC|HEVC|HEVC
AV1|AV1|AV1|AV1
当前视频配置无法初始化：%1。选择这些编码器时不可开始。|Could not initialize the current video configuration: %1. Processing cannot start with this encoder.|目前影片設定無法初始化：%1。選擇此編碼器時無法開始處理。|現在の動画設定を初期化できません：%1。このエンコーダーを選択した状態では処理を開始できません。
当前模型路径支持明确标记的 BT.709 或 BT.470BG/SMPTE 170M 8/10-bit SDR；HDR、其他位深和未标记色彩需要单独验证。|The current model path supports explicitly tagged BT.709 or BT.470BG/SMPTE 170M 8/10-bit SDR. HDR, other bit depths, and untagged color require separate validation.|目前模型路徑支援明確標記的 BT.709 或 BT.470BG/SMPTE 170M 8/10-bit SDR；HDR、其他位元深度及未標記色彩都需要獨立驗證。|現在のモデル処理は、明示的にタグ付けされた BT.709 または BT.470BG/SMPTE 170M の 8/10-bit SDR に対応します。HDR、その他のビット深度、色タグのない動画には個別の検証が必要です。
文件中没有可处理的视频轨。 |The file contains no processable video stream. |檔案中沒有可處理的影片軌。 |処理可能な動画ストリームがファイルにありません。
已生成 %1 帧，其中 %2 帧由 RIFE 生成。输出已通过解码和时间轴检查。|Generated %1 frames, including %2 interpolated by RIFE. The output passed decode and timeline checks.|已產生 %1 格，其中 %2 格由 RIFE 產生。輸出已通過解碼與時間軸檢查。|%1 フレームを生成し、そのうち %2 フレームを RIFE で補間しました。出力はデコードとタイムラインの検証に合格しました。
TensorRT 未能启动，已使用 ONNX Runtime CUDA 完成推理。|TensorRT could not start; inference completed with ONNX Runtime CUDA.|TensorRT 無法啟動，已使用 ONNX Runtime CUDA 完成推論。|TensorRT を起動できなかったため、ONNX Runtime CUDA で推論を完了しました。
操作已取消，未发布部分视频。|Operation cancelled. No partial video was published.|作業已取消，未發布部分影片。|操作をキャンセルしました。未完成の動画は公開されていません。
界面语言|Interface language|介面語言|表示言語
无法保存界面语言设置。|Could not save the interface language setting.|無法儲存介面語言設定。|表示言語の設定を保存できませんでした。
界面语言已保存；关闭并重新启动 RifeLove 后生效。|Language saved. Restart RifeLove to apply it.|介面語言已儲存；關閉並重新啟動 RifeLove 後生效。|言語設定を保存しました。RifeLove を再起動すると反映されます。
界面语言设置只保存在本机。|The interface language setting is stored only on this device.|介面語言設定只會儲存在本機。|表示言語の設定はこの PC にのみ保存されます。
推理后端|Inference backend|推理後端|推論バックエンド
镜头切换阈值|Scene-cut threshold|鏡頭切換閾值|シーン切り替えしきい値
较低值会把更多画面变化当作切镜，并减少跨切镜生成帧。|Lower values detect more scene changes and reduce interpolation across cuts.|較低的值會將更多畫面變化判定為鏡頭切換，並減少跨切鏡補幀。|値を低くすると、より多くの画面変化をカットとして扱い、カットをまたぐ補間を減らします。
自动：TensorRT 优先，失败时回退 CUDA|Automatic: prefer TensorRT and fall back to CUDA|自動：優先使用 TensorRT，失敗時回退至 CUDA|自動：TensorRT を優先し、失敗時は CUDA に切り替え
仅 ONNX Runtime CUDA|ONNX Runtime CUDA only|僅使用 ONNX Runtime CUDA|ONNX Runtime CUDA のみ
必须使用 TensorRT|Require TensorRT|必須使用 TensorRT|TensorRT を必須にする
推理精度当前固定为 FP32。FP16 尚未通过数值与画质验证，因此不会用于处理。|Inference precision is fixed to FP32. FP16 has not passed numerical and image-quality validation, so it is not used for processing.|推論精度目前固定為 FP32。FP16 尚未通過數值與畫質驗證，因此不會用於處理。|推論精度は FP32 に固定されています。FP16 は数値と画質の検証に合格していないため、処理には使用されません。
FP32 固定；FP16 尚未通过数值与画质验证。|FP32 only; FP16 has not passed numerical and image-quality validation.|固定使用 FP32；FP16 尚未通過數值與畫質驗證。|FP32 のみ。FP16 は数値と画質の検証に合格していません。
推理后端选择无效，默认设置未保存。|Invalid inference backend selection; defaults were not saved.|推論後端選擇無效，預設設定未儲存。|推論バックエンドの選択が無効なため、既定設定を保存できませんでした。
RIFELOVE  /  MOTION STUDIO|RIFELOVE  /  MOTION STUDIO|RIFELOVE  /  MOTION STUDIO|RIFELOVE  /  MOTION STUDIO
源帧     ·     RIFE 生成     ·     补帧输出|Source frames     ·     RIFE interpolation     ·     output|來源影格     ·     RIFE 補幀     ·     輸出影片|入力フレーム     ·     RIFE 補間     ·     出力動画
●  本地模型已校验 · CUDA 推理|●  Local model verified · CUDA inference|●  本機模型已驗證 · CUDA 推論|●  ローカルモデル検証済み · CUDA 推論
●  CUDA 推理|●  CUDA inference|●  CUDA 推論|●  CUDA 推論
选择本地模型包目录|Select a local model-pack folder|選擇本機模型套件資料夾|ローカルモデルパックフォルダーを選択
管理本机的 RIFE 插帧模型和画质修复模型。模型只从本机目录读取；此页面不会联网或下载权重。|Manage local RIFE interpolation and video restoration models. Model files are read only from this device; this page does not connect to the internet or download weights.|管理本機的 RIFE 補幀模型與畫質修復模型。模型只會從本機目錄讀取；此頁面不會連線或下載權重。|ローカルの RIFE 補間モデルと映像修復モデルを管理します。モデルファイルはこの PC からのみ読み込み、このページからインターネット接続や重みのダウンロードは行いません。
本地模型|Local model|本機模型|ローカルモデル
插帧|Interpolation|補幀|フレーム補間
画质修复|Video restoration|畫質修復|映像修復
修复任务：|Restoration tasks: |修復任務：|修復タスク：
；前/后文帧数：|; Context frames before/after: |；前／後文影格數：|；前後のコンテキストフレーム数：
；空间倍率：|; Spatial scale: |；空間倍率：|；空間倍率：
尚未登记本地模型\n添加包含 model.json 的 RIFE 或画质修复模型包|No local models registered yet\nAdd a RIFE or video restoration model pack containing model.json|尚未登記本機模型\n新增包含 model.json 的 RIFE 或畫質修復模型套件|ローカルモデルがまだ登録されていません\nmodel.json を含む RIFE または映像修復モデルパックを追加してください
目录校验会检查模型权重、许可、来源、能力和本地验证记录的文件哈希。哈希一致不代表作者身份或画质排名；未完成独立色彩与模型测试的路径不会标记为 HDR。移除只会从目录列表中取消登记，不会删除本机模型文件。|Catalog verification checks hashes for model weights, license, provenance, capabilities, and local validation records. Matching hashes do not authenticate the author or rank image quality. HDR is not shown unless the color and model paths pass separate tests. Removing a model only unregisters it; it does not delete local files.|目錄驗證會檢查模型權重、授權、來源、能力與本機驗證記錄的檔案雜湊。雜湊一致不代表作者身分或畫質排名；未通過獨立色彩與模型測試的路徑不會標示為 HDR。移除只會取消目錄登記，不會刪除本機模型檔案。|カタログ検証ではモデルの重み、ライセンス、出所、機能、ローカル検証記録のハッシュを確認します。ハッシュ一致は作者の認証や画質順位を保証しません。HDR は色とモデルの経路が個別テストに合格するまで表示しません。削除操作は登録解除のみで、ローカルファイルは削除しません。
镜头切换阈值必须介于 0 和 1 之间，默认设置未保存。|Scene-cut threshold must be between 0 and 1; defaults were not saved.|鏡頭切換閾值必須介於 0 和 1 之間，預設設定未儲存。|シーン切り替えのしきい値は 0 から 1 の範囲で指定してください。既定値は保存されませんでした。
"""


def load_entries() -> dict[str, dict[str, str]]:
    entries: dict[str, dict[str, str]] = {}
    for line in _DATA.strip().splitlines():
        source, english, traditional, japanese = (part.replace(r"\s", " ") for part in line.split("|", 3))
        source = source.replace(r"\n", "\n")
        values = {
            "en": english.replace(r"\n", "\n"),
            "zh_TW": traditional.replace(r"\n", "\n"),
            "ja": japanese.replace(r"\n", "\n"),
        }
        if source in entries and entries[source] != values:
            raise ValueError(f"Duplicate source translation: {source!r}")
        entries[source] = values
    return entries


def main() -> None:
    entries = load_entries()
    expected_placeholders = re.compile(r"%L?\d+")
    for locale, path in CATALOGS.items():
        tree = ET.parse(path)
        sources: set[str] = set()
        missing: list[str] = []
        count = 0
        for message in tree.findall(".//message"):
            source = message.findtext("source", default="")
            sources.add(source)
            translation = entries.get(source, {}).get(locale)
            node = message.find("translation")
            if translation is None and node is not None and node.text:
                translation = node.text
            if translation is None:
                missing.append(source)
                continue
            if sorted(expected_placeholders.findall(source)) != sorted(expected_placeholders.findall(translation)):
                raise ValueError(f"Placeholder mismatch in {locale} translation for {source!r}")
            if node is None:
                node = ET.SubElement(message, "translation")
            node.attrib.pop("type", None)
            node.text = translation
            count += 1
        extra = set(entries) - sources
        if missing:
            raise ValueError(f"Missing {locale} translations: {missing!r}")
        if extra:
            raise ValueError(f"Unused {locale} translations: {sorted(extra)!r}")
        tree.write(path, encoding="utf-8", xml_declaration=True)
        print(f"{locale}: translated {count} messages")


if __name__ == "__main__":
    main()
