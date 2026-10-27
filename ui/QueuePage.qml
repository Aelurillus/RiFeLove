import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "ProgressEstimate.js" as ProgressEstimate
import "PlatformExportProfiles.js" as PlatformExportProfiles

Item {
    id: page
    objectName: "queuePage"
    required property var queueController
    required property bool processingAvailable
    property url modelFolder: ""
    property url runtimeFolder: ""
    property url restorationModelFolder: ""
    property url outputFolder: ""
    property string targetRate: "60"
    property string encoder: "auto"
    property string inferenceBackend: "auto"
    property real cutMeanDifference: 0.25
    property string container: "mkv"
    property string platformExportProfile: "custom"
    property bool applyingPlatformExportProfile: false
    property bool recursive: false
    property bool advancedOpen: false
    property bool encodingAdvancedOpen: false
    property var selectedSources: []

    function restorationModelOptions() {
        const options = [{label: qsTr("关闭视频修复"), directory: ""}]
        for (const model of (queueController.models || [])) {
            const tasks = model.tasks || []
            const placements = model.placements || []
            const scale = Number(model.scaleNumerator || 0)
            const deblur = tasks.length === 1 && tasks.indexOf("deblur") >= 0 && scale === 1 && model.adapterId === "nafnet-zoo-deblur-rgb-f32-nchw-v1"
            const superResolution = tasks.length === 1 && tasks.indexOf("super-resolution") >= 0 && scale >= 2 && scale <= 4 && model.adapterId === "image-sr-rgb-f32-nchw-v1"
            if (!model.valid || !model.runtimeSupported || model.family !== "video-restoration" ||
                    (!deblur && !superResolution) || placements.indexOf("output-rate-after-interpolation") < 0 ||
                    Number(model.temporalFramesBefore || 0) !== 0 || Number(model.temporalFramesAfter || 0) !== 0 ||
                    Number(model.scaleDenominator || 0) !== 1 || Number(model.adapterVersion || 0) !== 1) continue
            const modelName = model.displayName || model.id || qsTr("本地修复模型")
            const label = deblur ? modelName + " · " + qsTr("去模糊（实验）") : modelName + " · ×" + scale
            options.push({label: label, directory: model.directory || "", task: deblur ? "deblur" : "super-resolution", scale: scale})
        }
        return options
    }
    function selectedRestorationModelIndex() {
        const selected = queueController.localPath(restorationModelFolder)
        if (!selected) return 0
        const normalize = value => String(value).replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase()
        const target = normalize(selected)
        const options = restorationModelOptions()
        for (let index = 1; index < options.length; ++index)
            if (normalize(options[index].directory) === target) return index
        return 0
    }

    function applyDefaults(profileValues) {
        applyingPlatformExportProfile = true
        const values = profileValues || queueController.defaults || ({})
        if (values.rate) { targetRate = values.rate; rateBox.editText = values.rate }
        if (profileValues) {
            modelFolder = values.model && values.model.toString() ? values.model : ""
            runtimeFolder = values.runtime && values.runtime.toString() ? values.runtime : ""
            outputFolder = values.output && values.output.toString() ? values.output : ""
            restorationModelFolder = queueController.defaults.restorationModel || ""
        } else {
            if (values.model && values.model.toString()) modelFolder = values.model
            if (values.runtime && values.runtime.toString()) runtimeFolder = values.runtime
            if (values.output && values.output.toString()) outputFolder = values.output
            restorationModelFolder = queueController.defaults.restorationModel || ""
        }
        if (values.encoder) { encoder = values.encoder; encoderBox.currentIndex = ["auto", "libopenh264", "libkvazaar", "libaom-av1", "h264_nvenc"].indexOf(values.encoder) }
        if (values.container) { container = values.container; containerBox.currentIndex = ["mkv", "mp4", "mov"].indexOf(values.container) }
        platformExportProfile = values.platformExportProfile || "custom"
        inferenceBackend = ["auto", "cuda", "tensorrt"].includes(values.inferenceBackend) ? values.inferenceBackend : "auto"
        const threshold = Number(values.cutMeanDifference)
        cutMeanDifference = Number.isFinite(threshold) && threshold >= 0 && threshold <= 1 ? threshold : 0.25
        encodingBitrate.value = Number(values.bitrateKbps || 8000)
        encodingQuality.value = Number(values.quality || 32)
        encodingGop.value = Number(values.gopSize === undefined ? 120 : values.gopSize)
        encodingQualityMode.currentIndex = values.qualityMode === "qp" || values.qualityMode === "crf" || values.qualityMode === "cq" ? 1 : 0
        normalizeEncodingMode()
        applyingPlatformExportProfile = false
    }
    function markPlatformExportCustom() {
        if (!applyingPlatformExportProfile) platformExportProfile = "custom"
    }
    function applyPlatformExportProfile(profileId) {
        const preset = PlatformExportProfiles.makePreset(profileId, 1080)
        applyingPlatformExportProfile = true
        platformExportProfile = profileId || "custom"
        if (preset) {
            targetRate = preset.rate
            rateBox.editText = preset.rate
            container = preset.container
            containerBox.currentIndex = ["mkv", "mp4", "mov"].indexOf(preset.container)
            encoder = preset.encoder
            encoderBox.currentIndex = ["auto", "libopenh264", "libkvazaar", "libaom-av1", "h264_nvenc"].indexOf(preset.encoder)
            encodingBitrate.value = preset.bitrateKbps
            encodingQualityMode.currentIndex = 0
            encodingGop.value = preset.gopSize
        }
        applyingPlatformExportProfile = false
        persistDefaults()
    }
    function platformExportHint() {
        if (platformExportProfile === "bilibili120")
            return qsTr("Bilibili 的 120 FPS 依据历史官方活动信息；当前账号与转码行为仍需实测。")
        if (platformExportProfile === "youtube60")
            return qsTr("YouTube 60 FPS SDR 队列使用官方 1080p 建议码率；混合分辨率请在高级编码中调整。平台仍会转码，300 FPS 以上母版需单独输出。")
        if (platformExportProfile === "douyin30" || platformExportProfile === "douyin60")
            return qsTr("抖音公开上传 API 未列出帧率上限；此处是本地兼容预设，不代表平台保证。")
        if (platformExportProfile === "bilibili60")
            return qsTr("Bilibili 60 FPS 是兼容性预设；码率为本机建议值，当前上传规则需按账号验证。")
        return qsTr("平台会重新编码视频。300 FPS 以上本机母版需单独输出；平台预设会生成兼容性副本。")
    }
    function normalizeEncodingMode() {
        if (![2, 3, 4].includes(encoderBox.currentIndex) && encodingQualityMode.currentIndex > 0) encodingQualityMode.currentIndex = 0
        if ([2, 4].includes(encoderBox.currentIndex) && encodingQuality.value > 51) encodingQuality.value = 51
        if (![2, 4].includes(encoderBox.currentIndex) && encodingQuality.value > 63) encodingQuality.value = 63
    }
    function encodingOptions() {
        let mode = "bitrate"
        if (encodingQualityMode.currentIndex === 1)
            mode = encoderBox.currentIndex === 2 ? "qp" : encoderBox.currentIndex === 3 ? "crf" : "cq"
        return { bitrateKbps: encodingBitrate.value, qualityMode: mode, quality: encodingQuality.value, gopSize: encodingGop.value }
    }
    function localProfileValues() {
        const options = encodingOptions()
        return {rate: rateBox.editText, model: modelFolder, runtime: runtimeFolder,
            output: outputFolder, encoder: encoder, container: containerBox.currentText || "mkv",
            bitrateKbps: options.bitrateKbps, qualityMode: options.qualityMode,
            quality: options.quality, gopSize: options.gopSize, inferenceBackend: inferenceBackend,
            cutMeanDifference: cutMeanDifference, platformExportProfile: platformExportProfile}
    }
    function add(sources) {
        if (!sources || sources.length === 0) return
        selectedSources = sources
        queueController.addItems(sources, outputFolder, rateBox.editText, modelFolder, runtimeFolder,
                                 encoder, container, encodingOptions(), recursive, inferenceBackend, cutMeanDifference, restorationModelFolder)
        selectedSources = []
    }
    function persistDefaults() {
        const codecs = ["auto", "libopenh264", "libkvazaar", "libaom-av1", "h264_nvenc"]
        queueController.saveDefaults(rateBox.editText, modelFolder, runtimeFolder, outputFolder,
            codecs[encoderBox.currentIndex] || "auto", containerBox.currentText || "mkv", encodingOptions(), inferenceBackend, cutMeanDifference, platformExportProfile)
        queueController.setDefaultRestorationModel(restorationModelFolder)
    }
    function statusText(value) {
        const labels = {queued: qsTr("等待处理"), running: qsTr("正在处理"), paused: qsTr("已暂停"),
            interrupted: qsTr("已中断"), failed: qsTr("失败"), cancelled: qsTr("已取消"), completed: qsTr("已完成")}
        return labels[value] || value
    }
    function stageText(value) {
        const labels = {checking_source: qsTr("校验源文件"), waiting_for_engine: qsTr("等待处理引擎"),
            loading_model: qsTr("加载模型"), loading_restoration_model: qsTr("加载修复模型（TensorRT 首次构建可能需要数分钟）"), processing: qsTr("AI 补帧"),
            restoration: qsTr("正在进行视频修复"), resuming: qsTr("读取已完成片段"), encoding: qsTr("编码视频"),
            validating: qsTr("验证输出"), verifying_output: qsTr("校验发布文件"), encoder_fallback: qsTr("切换编码器"), paused: qsTr("安全暂停点")}
        return labels[value] || value
    }
    Component.onCompleted: applyDefaults()
    Connections {
        target: page.queueController
        function onDefaultsChanged() { page.applyDefaults() }
        function onProfileApplied(values) { page.applyDefaults(values) }
    }

    FileDialog {
        id: filesDialog
        title: qsTr("批量选择视频")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.m2ts *.wmv *.flv *.mxf *.ogv *.vob *.3gp *.asf *.gif)"), qsTr("所有文件 (*)")]
        onAccepted: page.add(selectedFiles)
    }
    FileDialog {
        id: queueReportDialog
        title: qsTr("导出队列报告")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("JSON 报告 (*.json)")]
        onAccepted: page.queueController.exportReport(selectedFile)
    }
    FolderDialog { id: sourceFolderDialog; title: qsTr("选择包含视频的文件夹"); onAccepted: page.add([selectedFolder]) }
    FolderDialog { id: outputFolderDialog; title: qsTr("选择输出文件夹"); onAccepted: { page.outputFolder = selectedFolder; page.persistDefaults() } }
    FolderDialog { id: modelDialog; title: qsTr("选择已校验的本地 RIFE 模型包"); onAccepted: { page.modelFolder = selectedFolder; page.persistDefaults() } }
    FolderDialog { id: runtimeDialog; title: qsTr("选择 ONNX Runtime CUDA 运行库目录"); onAccepted: { page.runtimeFolder = selectedFolder; page.persistDefaults() } }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 30
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: qsTr("任务队列"); font.pixelSize: 27; font.bold: true }
                Label { text: qsTr("批量导入并持久保存任务。已完成片段会在本机校验和保存；中断后可继续，也可从头重启。"); color: "#697B8B"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
            ActionButton { text: qsTr("重新连接"); visible: !page.queueController.available; onClicked: page.queueController.reconnect() }
        }
        Rectangle {
            Layout.fillWidth: true
            radius: 10
            color: "#FFFFFF"
            border.color: drop.containsDrag ? "#13836B" : "#CBD6DC"
            implicitHeight: importForm.implicitHeight + 28
            DropArea {
                id: drop
                anchors.fill: parent
                enabled: page.processingAvailable && page.queueController.available && !page.queueController.busy && !page.queueController.importing
                onDropped: function(event) { if (event.hasUrls) page.add(event.urls) }
            }
            ColumnLayout {
                id: importForm
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12
                RowLayout {
                    Label { text: qsTr("导入到队列"); font.bold: true; Layout.fillWidth: true }
                    ActionButton { text: qsTr("选择多个视频"); enabled: page.queueController.available && !page.queueController.busy && !page.queueController.importing; onClicked: filesDialog.open() }
                    ActionButton { text: qsTr("选择文件夹"); enabled: page.queueController.available && !page.queueController.busy && !page.queueController.importing; onClicked: sourceFolderDialog.open() }
                }
                Label { text: qsTr("拖入视频文件或文件夹；自动检查媒体轨道和模型支持的色彩格式。重复文件、已有队列输出和输出目录中的已生成文件会跳过。"); color: "#697B8B"; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12 }
                LocalProfilePicker {
                    objectName: "queueLocalProfiles"
                    Layout.fillWidth: true
                    controller: page.queueController
                    values: page.localProfileValues()
                    available: page.queueController.available && !page.queueController.busy && !page.queueController.importing
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("平台导出预设"); font.bold: true; Layout.preferredWidth: 100 }
                    PlatformExportPicker {
                        objectName: "queuePlatformExportProfile"
                        Layout.fillWidth: true
                        profileId: page.platformExportProfile
                        controlObjectName: "queuePlatformExportProfileCombo"
                        onActivated: function(profileId) { page.applyPlatformExportProfile(profileId) }
                    }
                }
                Label { text: page.platformExportHint(); color: "#697B8B"; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 12 }
                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("目标 FPS"); font.bold: true }
                        ComboBox {
                            id: rateBox
                            objectName: "queueTargetRate"
                            Layout.fillWidth: true
                            editable: true
                            model: ["30", "60", "120", "144", "240", "300", "360", "480", "600", "60000/1001"]
                            onEditTextChanged: { page.targetRate = editText; page.markPlatformExportCustom() }
                            onActivated: { page.markPlatformExportCustom(); page.persistDefaults() }
                            onAccepted: { page.markPlatformExportCustom(); page.persistDefaults() }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("输出容器"); font.bold: true }
                        ComboBox { id: containerBox; objectName: "queueContainer"; Layout.fillWidth: true; model: ["mkv", "mp4", "mov"]; onActivated: { page.markPlatformExportCustom(); page.container = currentText; page.persistDefaults() } }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("编码器"); font.bold: true }
                        ComboBox { id: encoderBox; Layout.fillWidth: true; model: [qsTr("自动编码（优先 NVENC）"), qsTr("H.264 软件"), qsTr("HEVC 软件"), qsTr("AV1 软件"), qsTr("NVIDIA H.264（NVENC）")]; onActivated: { page.markPlatformExportCustom(); page.encoder = ["auto", "libopenh264", "libkvazaar", "libaom-av1", "h264_nvenc"][currentIndex]; page.normalizeEncodingMode(); page.persistDefaults() } }
                    }
                }
                ActionButton { text: page.encodingAdvancedOpen ? qsTr("收起高级编码") : qsTr("高级编码设置"); flat: true; Layout.alignment: Qt.AlignLeft; onClicked: page.encodingAdvancedOpen = !page.encodingAdvancedOpen }
                GroupBox {
                    title: qsTr("高级编码")
                    Layout.fillWidth: true
                    visible: page.encodingAdvancedOpen
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    ColumnLayout {
                        anchors.fill: parent
                        RowLayout {
                            Layout.fillWidth: true
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { text: qsTr("码率控制") }
                                ComboBox {
                                    id: encodingQualityMode
                                    Layout.fillWidth: true
                                    model: encoderBox.currentIndex === 2 ? [qsTr("目标码率"), qsTr("恒定量化 QP")]
                                        : encoderBox.currentIndex === 3 ? [qsTr("目标码率"), qsTr("恒定质量 CRF")]
                                        : encoderBox.currentIndex === 4 ? [qsTr("目标码率"), qsTr("恒定质量 CQ")]
                                        : [qsTr("目标码率")]
                                    onActivated: { page.markPlatformExportCustom(); page.persistDefaults() }
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { text: encodingQualityMode.currentIndex === 1 ? (encoderBox.currentIndex === 2 ? qsTr("QP（0–51）") : encoderBox.currentIndex === 4 ? qsTr("CQ（0–51）") : qsTr("CRF（0–63）")) : qsTr("目标质量") }
                                SpinBox {
                                    id: encodingQuality
                                    objectName: "queueQuality"
                                    from: 0
                                    to: [2, 4].includes(encoderBox.currentIndex) ? 51 : 63
                                    value: 32
                                    enabled: encodingQualityMode.currentIndex === 1
                                    onValueModified: { page.markPlatformExportCustom(); page.persistDefaults() }
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { text: qsTr("目标码率（kbit/s）") }
                                SpinBox { id: encodingBitrate; objectName: "queueBitrateKbps"; from: 250; to: 200000; stepSize: 250; value: 8000; enabled: encodingQualityMode.currentIndex === 0; onValueModified: { page.markPlatformExportCustom(); page.persistDefaults() } }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Label { text: qsTr("关键帧间隔（帧）") }
                                SpinBox { id: encodingGop; objectName: "queueGopSize"; from: 0; to: 1000; value: 120; onValueModified: { page.markPlatformExportCustom(); page.persistDefaults() } }
                            }
                        }
                        Label {
                            text: encoderBox.currentIndex === 2 ? qsTr("HEVC 恒定量化使用 Kvazaar QP；较低 QP 通常会生成更大的文件。")
                                : encoderBox.currentIndex === 3 ? qsTr("AV1 恒定质量使用 CRF；较低 CRF 通常会生成更大的文件。")
                                : encoderBox.currentIndex === 4 ? qsTr("NVENC 恒定质量使用 VBR + CQ（0–51），需要可用的 NVIDIA 视频编码驱动；较低 CQ 通常会生成更大的文件。")
                                : qsTr("自动模式按驱动与源像素格式选择 NVENC、软件 H.264、HEVC 或 AV1，并尽量保留源位深。")
                            color: "#697B8B"
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("输出位置"); Layout.preferredWidth: 75 }
                    TextField { Layout.fillWidth: true; readOnly: true; text: page.outputFolder.toString(); placeholderText: qsTr("留空时使用首个来源文件夹"); selectByMouse: true }
                    ActionButton { text: qsTr("选择"); onClicked: outputFolderDialog.open() }
                    ActionButton { text: qsTr("清除"); enabled: !!page.outputFolder.toString(); onClicked: { page.outputFolder = ""; page.persistDefaults() } }
                }
                ActionButton { objectName: "queueAdvancedModelToggle"; text: page.advancedOpen ? qsTr("收起模型与运行库") : qsTr("高级设置 · 模型与运行库"); flat: true; Layout.alignment: Qt.AlignLeft; onClicked: page.advancedOpen = !page.advancedOpen }
                RowLayout {
                    visible: page.advancedOpen
                    Layout.fillWidth: true
                    Label { text: qsTr("RIFE 模型"); Layout.preferredWidth: 75 }
                    TextField { objectName: "queueModelPath"; Layout.fillWidth: true; readOnly: true; text: page.modelFolder.toString(); placeholderText: qsTr("选择 model.json 所在的已校验目录"); selectByMouse: true }
                    ActionButton { text: qsTr("选择"); enabled: !page.queueController.busy && !page.queueController.importing; onClicked: modelDialog.open() }
                }
                RowLayout {
                    visible: page.advancedOpen
                    Layout.fillWidth: true
                    Label { text: qsTr("CUDA 运行库"); Layout.preferredWidth: 75 }
                    TextField { objectName: "queueRuntimePath"; Layout.fillWidth: true; readOnly: true; text: page.runtimeFolder.toString(); placeholderText: qsTr("选择 ONNX Runtime 与 CUDA DLL 目录"); selectByMouse: true }
                    ActionButton { text: qsTr("选择"); enabled: !page.queueController.busy && !page.queueController.importing; onClicked: runtimeDialog.open() }
                }
                RowLayout {
                    visible: page.advancedOpen
                    Layout.fillWidth: true
                    Label { text: qsTr("推理后端"); Layout.preferredWidth: 75 }
                    ComboBox {
                        objectName: "queueInferenceBackend"
                        Layout.fillWidth: true
                        model: [qsTr("自动：TensorRT 优先，失败时回退 CUDA"), qsTr("仅 ONNX Runtime CUDA"), qsTr("必须使用 TensorRT")]
                        currentIndex: ["auto", "cuda", "tensorrt"].indexOf(page.inferenceBackend)
                        enabled: !page.queueController.busy && !page.queueController.importing
                        onActivated: { page.inferenceBackend = ["auto", "cuda", "tensorrt"][currentIndex]; page.persistDefaults() }
                    }
                    Label { text: qsTr("FP32 固定；FP16 尚未通过数值与画质验证。"); color: "#697B8B"; font.pixelSize: 12; Layout.preferredWidth: 260; wrapMode: Text.WordWrap }
                }
                RowLayout {
                    visible: page.advancedOpen
                    Layout.fillWidth: true
                    Label { text: qsTr("视频修复"); Layout.preferredWidth: 75 }
                    ComboBox {
                        objectName: "queueRestorationModel"
                        Layout.fillWidth: true
                        model: page.restorationModelOptions()
                        textRole: "label"
                        currentIndex: page.selectedRestorationModelIndex()
                        enabled: !page.queueController.busy && !page.queueController.importing && !page.queueController.verifyingModels
                        onActivated: {
                            const choice = page.restorationModelOptions()[currentIndex]
                            page.restorationModelFolder = choice && choice.directory ? page.queueController.localUrl(choice.directory) : ""
                            page.persistDefaults()
                        }
                    }
                    Label {
                        text: qsTr("仅使用已校验的本地 SDR 模型；逐帧去模糊为实验功能，可能闪烁。每个输出帧都会修复并增加处理时间。")
                        color: "#697B8B"
                        font.pixelSize: 12
                        Layout.preferredWidth: 260
                        wrapMode: Text.WordWrap
                    }
                }
                RowLayout {
                    visible: page.advancedOpen
                    Layout.fillWidth: true
                    Label { text: qsTr("镜头切换阈值"); Layout.preferredWidth: 110 }
                    SpinBox {
                        objectName: "queueCutMeanDifference"
                        from: 0
                        to: 100
                        stepSize: 1
                        value: Math.round(page.cutMeanDifference * 100)
                        textFromValue: function(value, locale) { return value + "%" }
                        valueFromText: function(text, locale) { return Number(text.replace("%", "")) }
                        enabled: !page.queueController.busy && !page.queueController.importing
                        onValueModified: { page.cutMeanDifference = value / 100; page.persistDefaults() }
                    }
                    Label { text: qsTr("较低值会把更多画面变化当作切镜，并减少跨切镜生成帧。"); color: "#697B8B"; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                }
                RowLayout {
                    CheckBox { text: qsTr("扫描子文件夹"); checked: page.recursive; onToggled: page.recursive = checked; enabled: !page.queueController.busy && !page.queueController.importing }
                    Item { Layout.fillWidth: true }
                    ActionButton {
                        text: page.queueController.importing ? qsTr("取消导入") : qsTr("加入队列")
                        highlighted: true
                        primary: true
                        enabled: page.queueController.available && !page.queueController.busy &&
                                 (page.queueController.importing || (page.modelFolder.toString() !== "" && page.runtimeFolder.toString() !== "" &&
                                  (!page.restorationModelFolder.toString() || page.selectedRestorationModelIndex() > 0)))
                        onClicked: {
                            if (page.queueController.importing) page.queueController.stop()
                            else if (page.selectedSources.length) page.add(page.selectedSources)
                            else filesDialog.open()
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label { text: qsTr("%1 项").arg(page.queueController.counts.total || 0); font.bold: true; Layout.fillWidth: true }
            Label { text: qsTr("等待 %1 · 运行 %2 · 完成 %3 · 失败 %4").arg(page.queueController.counts.queued || 0).arg(page.queueController.counts.running || 0).arg(page.queueController.counts.completed || 0).arg(page.queueController.counts.failed || 0); color: "#697B8B" }
            ActionButton { objectName: "queueReportButton"; text: qsTr("导出报告"); enabled: page.queueController.available; onClicked: queueReportDialog.open() }
            ActionButton { objectName: "queueStartButton"; text: qsTr("开始队列"); primary: true; enabled: page.queueController.available && !page.queueController.busy && !page.queueController.importing && (page.queueController.counts.queued || 0) > 0; onClicked: page.queueController.start() }
            ActionButton { objectName: "queuePauseButton"; text: qsTr("暂停"); visible: page.queueController.busy && ["running", "pausing"].indexOf(page.queueController.state) >= 0; enabled: page.queueController.state === "running"; onClicked: page.queueController.pause() }
            ActionButton { objectName: "queueResumeButton"; text: qsTr("继续"); primary: true; visible: page.queueController.busy && ["paused", "resuming"].indexOf(page.queueController.state) >= 0; enabled: page.queueController.state === "paused"; onClicked: page.queueController.resume() }
            ActionButton { text: page.queueController.importing ? qsTr("取消导入") : qsTr("停止队列"); visible: page.queueController.busy || page.queueController.importing; onClicked: page.queueController.stop() }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: page.queueController.detail !== ""
            radius: 8
            color: page.queueController.state === "error" ? "#F7E5E5" : "#DEE6EB"
            implicitHeight: queueDetail.implicitHeight + 20
            Label { id: queueDetail; anchors.fill: parent; anchors.margins: 10; text: page.queueController.detail; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: page.queueController.state === "error" ? "#B33B44" : "#52697A"; font.pixelSize: 12 }
        }
        ListView {
            id: tasksView
            objectName: "queueTaskList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: page.queueController.tasks
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property string taskId
                required property string sourcePath
                required property string outputPath
                required property string taskState
                required property string targetRate
                required property string modelLabel
                required property string stage
                required property string errorText
                required property var frames
                required property var aiFrames
                required property var estimatedFrames
                required property real processingFps
                required property string fileName
                width: tasksView.width
                implicitHeight: taskCard.implicitHeight + 22
                radius: 8
                color: "#FFFFFF"
                border.color: taskState === "failed" ? "#D58A8D" : "#DFE7EB"
                ColumnLayout {
                    id: taskCard
                    anchors.fill: parent
                    anchors.margins: 11
                    spacing: 5
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: fileName; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true; textFormat: Text.PlainText }
                        Label { text: page.statusText(taskState); color: taskState === "failed" ? "#B33B44" : taskState === "completed" ? "#13836B" : "#52697A"; font.bold: true }
                        Label { text: targetRate + qsTr(" FPS"); color: "#697B8B" }
                    }
                    Label {
                        text: {
                            if (!stage)
                                return qsTr("模型：") + modelLabel
                            const expected = Number(estimatedFrames || 0)
                            const speed = Number(processingFps || 0)
                            let value = page.stageText(stage) + " · "
                            value += expected > 0 ? qsTr("%1 / 约 %2 帧").arg(frames).arg(expected) : qsTr("%1 帧").arg(frames)
                            value += qsTr(" · AI %1 帧 · %2 FPS").arg(aiFrames).arg(speed.toFixed(1))
                            if (taskState === "running") {
                                if (expected > 0 && speed > 0)
                                    value += qsTr(" · 剩余约 %1").arg(ProgressEstimate.formatDuration(Math.max(0,expected - Number(frames)) / speed))
                                else if (expected > 0)
                                    value += qsTr(" · 预计剩余时间计算中")
                            }
                            return value
                        }
                        color: "#697B8B"
                        font.pixelSize: 12
                        textFormat: Text.PlainText
                    }
                    Label { visible: taskState === "failed"; text: errorText; color: "#B33B44"; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; font.pixelSize: 12; textFormat: Text.PlainText }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: outputPath; elide: Text.ElideMiddle; color: "#6D8193"; font.pixelSize: 11; Layout.fillWidth: true; textFormat: Text.PlainText }
                        ActionButton { objectName: "cancelTaskButton"; text: qsTr("取消任务"); visible: taskState === "queued" || taskState === "running" || taskState === "paused"; enabled: taskState !== "running" || page.queueController.busy; onClicked: page.queueController.cancelTask(taskId) }
                        ActionButton { objectName: "retryTaskButton"; text: qsTr("重试"); visible: taskState === "failed" && !page.queueController.busy && !page.queueController.importing; onClicked: page.queueController.retryTask(taskId) }
                        ActionButton { text: qsTr("从检查点继续"); visible: taskState === "interrupted" && !page.queueController.busy && !page.queueController.importing; onClicked: page.queueController.resumeTask(taskId) }
                        ActionButton { text: qsTr("从头重启"); visible: taskState === "interrupted" && !page.queueController.busy && !page.queueController.importing; onClicked: page.queueController.restartTask(taskId) }
                        ActionButton { text: qsTr("打开输出位置"); visible: taskState === "completed"; onClicked: page.queueController.openOutputFolder(taskId) }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: (page.queueController.counts.total || 0) === 0
                text: qsTr("队列为空\n导入多个视频或整个文件夹开始处理")
                color: "#70818F"
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.5
            }
        }
    }
}
