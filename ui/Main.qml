import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Dialogs
import "ProgressEstimate.js" as ProgressEstimate

ApplicationWindow {
    id: window
    visible: true
    width: 1360
    height: 860
    minimumWidth: 1080
    minimumHeight: 720
    title: "RifeLove"
    color: "#0b1018"
    Material.theme: Material.Dark
    Material.accent: "#9ee6c8"
    Material.primary: "#26394a"
    font.family: "Microsoft YaHei UI"
    font.pixelSize: 14
    palette.text: "#eaf0fa"
    palette.windowText: "#eaf0fa"
    palette.buttonText: "#eaf0fa"
    palette.base: "#111923"
    palette.placeholderText: "#8493aa"
    palette.button: "#283447"
    palette.highlight: "#9ee6c8"
    palette.highlightedText: "#10241f"
    property url modelFolder: ""
    property url runtimeFolder: ""
    property bool advancedOpen: false
    property bool encodingAdvancedOpen: false
    property int currentPage: 0
    readonly property bool queueActive: processingAvailable && (taskQueue.busy || taskQueue.importing)
    readonly property bool active: processingAvailable && (studio.busy || queueActive)
    readonly property bool backendAvailable: processingAvailable
    property string selectedModelDisplayName: ""
    property bool selectedModelVerified: false
    property bool startupModelValidationStarted: false
    function resolveSelectedModelDisplayName() {
        selectedModelVerified = false
        if (!processingAvailable || !taskQueue || !taskQueue.defaults) return ""
        const selected = taskQueue.defaults.model
        const directory = selected ? taskQueue.localPath(selected) : ""
        if (!directory) return ""
        const models = taskQueue.models || []
        const normalizePath = value => String(value).replace(/\\/g, "/").replace(/\/+$/, "").toLowerCase()
        const selectedPath = normalizePath(directory)
        for (let index = 0; index < models.length; ++index) {
            if (normalizePath(models[index].directory) === selectedPath) {
                selectedModelVerified = !!models[index].valid
                return models[index].valid ? models[index].displayName : qsTr("模型校验失败")
            }
        }
        return taskQueue.verifyingModels ? qsTr("正在校验本地模型…") : qsTr("本地模型未校验")
    }
    function refreshSelectedModelDisplayName() {
        selectedModelDisplayName = resolveSelectedModelDisplayName()
    }
    readonly property var info: processingAvailable ? studio.media : ({})
    readonly property var progress: processingAvailable ? studio.progress : ({})
    readonly property string state: processingAvailable ? studio.status : "unavailable"
    readonly property bool selectedEncoderReady: {
        if (!processingAvailable || !window.info.encoders) return false
        const probes = window.info.encoders
        if (encoder.currentIndex === 0) {
            const nvenc = probes.h264_nvenc
            const software = probes.libopenh264
            return (!!nvenc && !!nvenc.contextOpened) || (!!software && !!software.contextOpened)
        }
        const names = ["libopenh264", "libkvazaar", "libaom-av1"]
        const probe = probes[names[encoder.currentIndex - 1]]
        return !!probe && !!probe.contextOpened
    }
    readonly property bool canStart: processingAvailable && !window.active && !!window.info.width && !!window.info.supportedColor &&
        !!window.info.gpuAvailable && window.selectedEncoderReady && window.modelFolder.toString() !== "" && window.runtimeFolder.toString() !== ""

    function applyLocalDefaults() {
        if (!processingAvailable) return
        const values = taskQueue.defaults || ({})
        if (!modelFolder.toString() && values.model && values.model.toString()) modelFolder = values.model
        if (!runtimeFolder.toString() && values.runtime && values.runtime.toString()) runtimeFolder = values.runtime
        if (values.rate) rate.editText = values.rate
        if (values.encoder) encoder.currentIndex = ["auto", "libopenh264", "libkvazaar", "libaom-av1"].indexOf(values.encoder)
        studioBitrate.value = Number(values.bitrateKbps || 8000)
        studioQuality.value = Number(values.quality || 32)
        studioGop.value = Number(values.gopSize === undefined ? 120 : values.gopSize)
        studioQualityMode.currentIndex = values.qualityMode === "qp" || values.qualityMode === "crf" ? 1 : 0
        normalizeEncodingMode()
    }
    function normalizeEncodingMode() {
        if (encoder.currentIndex < 2 && studioQualityMode.currentIndex > 0) studioQualityMode.currentIndex = 0
        if (encoder.currentIndex === 2 && studioQuality.value > 51) studioQuality.value = 51
        if (encoder.currentIndex !== 2 && studioQuality.value > 63) studioQuality.value = 63
    }
    function encodingOptions() {
        let mode = "bitrate"
        if (studioQualityMode.currentIndex === 1) mode = encoder.currentIndex === 2 ? "qp" : "crf"
        return { bitrateKbps: studioBitrate.value, qualityMode: mode, quality: studioQuality.value, gopSize: studioGop.value }
    }
    function persistLocalDefaults() {
        if (!processingAvailable) return
        const codecs = ["auto", "libopenh264", "libkvazaar", "libaom-av1"]
        taskQueue.saveDefaults(rate.editText, modelFolder, runtimeFolder, taskQueue.defaults.output || "",
            codecs[encoder.currentIndex] || "auto", taskQueue.defaults.container || "mkv", encodingOptions())
    }
    function encoderHint() {
        if (!window.info.width) return qsTr("导入后会检查本机的 GPU 与视频编码能力。")
        if (!window.info.nvencContextOpened && window.info.softwareH264ContextOpened)
            return qsTr("NVENC 当前不可用；自动模式会用软件 H.264 编码，RIFE 仍由 CUDA 推理。")
        if (window.info.nvencContextOpened) return qsTr("NVENC 可以初始化；最终以实际导出结果为准。")
        return qsTr("H.264 编码器暂不可用，请检查本机视频运行环境。")
    }
    Component.onCompleted: { applyLocalDefaults(); refreshSelectedModelDisplayName() }
    Connections {
        target: taskQueue
        function onDefaultsChanged() { window.applyLocalDefaults(); window.refreshSelectedModelDisplayName() }
        function onModelsChanged() { window.refreshSelectedModelDisplayName() }
        function onChanged() {
            if (taskQueue.available && !window.startupModelValidationStarted) {
                window.startupModelValidationStarted = true
                taskQueue.refreshModels()
            }
        }
    }

    function stateText(value) {
        if (value === "inspected" && window.info.width) {
            if (!window.info.supportedColor) return qsTr("色彩格式暂不支持")
            if (!window.info.gpuAvailable) return qsTr("NVIDIA GPU 不可用")
            if (!window.selectedEncoderReady) return qsTr("所选编码器不可用")
            if (!window.modelFolder.toString() || !window.runtimeFolder.toString()) return qsTr("请配置模型与运行库")
            return qsTr("可以开始")
        }
        const labels = {ready: qsTr("等待导入"), inspecting: qsTr("读取视频信息"), inspected: qsTr("可以开始"),
            checking_source: qsTr("校验源文件"), waiting_for_engine: qsTr("等待处理引擎"),
            loading_model: qsTr("加载本地模型"), processing: qsTr("正在补帧"), encoding: qsTr("正在编码"),
            validating: qsTr("验证输出视频"), encoder_fallback: qsTr("切换编码器"), completed: qsTr("处理完成"),
            failed: qsTr("处理失败"), cancelling: qsTr("正在安全取消"), cancelled: qsTr("已取消"), unavailable: qsTr("处理组件未启用")}
        return labels[value] || qsTr("正在处理")
    }
    function stateColor(value) {
        if (value === "failed" || (value === "inspected" && window.info.width && !window.info.supportedColor)) return "#ffb3ad"
        if (value === "inspected" && !window.canStart) return "#f0c98a"
        return "#9ee6c8"
    }
    FileDialog {
        id: sourceDialog
        title: qsTr("选择视频")
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)"), qsTr("所有文件 (*)")]
        onAccepted: { window.currentPage = 1; studio.openSource(selectedFile) }
    }
    FileDialog {
        id: outputDialog
        title: qsTr("保存补帧视频")
        fileMode: FileDialog.SaveFile
        nameFilters: ["Matroska (*.mkv)", "MP4 (*.mp4)", "QuickTime (*.mov)"]
        onAccepted: studio.setOutput(selectedFile)
    }
    FolderDialog { id: modelDialog; title: qsTr("选择已校验的本地模型包"); onAccepted: { window.modelFolder = selectedFolder; window.persistLocalDefaults() } }
    FolderDialog { id: runtimeDialog; title: qsTr("选择 ONNX CUDA 运行库目录"); onAccepted: { window.runtimeFolder = selectedFolder; window.persistLocalDefaults() } }
    onClosing: function(close) {
        if (window.active) { close.accepted = false; closeDialog.open() }
    }
    Dialog {
        id: closeDialog
        anchors.centerIn: parent
        modal: true
        title: qsTr("正在处理视频")
        standardButtons: Dialog.Cancel | Dialog.Ok
        Label { text: qsTr("取消导入或当前任务，并等待资源清理后，可关闭窗口。"); wrapMode: Text.WordWrap }
        onAccepted: {
            if (processingAvailable && studio.busy) studio.cancel()
            if (processingAvailable && (taskQueue.busy || taskQueue.importing)) taskQueue.stop()
        }
    }
    RowLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 232
            color: "#101722"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    Rectangle {
                        Layout.preferredWidth: 46
                        Layout.preferredHeight: 46
                        radius: 14
                        gradient: Gradient { GradientStop { position: 0; color: "#b2f0d6" } GradientStop { position: 1; color: "#62c6aa" } }
                        Label { anchors.centerIn: parent; text: "R"; color: "#10241f"; font.pixelSize: 28; font.bold: true }
                    }
                    ColumnLayout {
                        spacing: 1
                        Label { text: "RifeLove"; font.pixelSize: 23; font.bold: true }
                        Label { text: qsTr("离线视频工作台"); color: "#91a1b4"; font.pixelSize: 12 }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#253243"; Layout.topMargin: 12; Layout.bottomMargin: 8 }
                Label { text: qsTr("工作区"); color: "#74849a"; font.pixelSize: 11; font.bold: true; Layout.leftMargin: 8 }
                ActionButton { objectName: "homeNavigationButton"; text: qsTr("⌂     工作区"); Layout.fillWidth: true; highlighted: window.currentPage === 0; onClicked: window.currentPage = 0 }
                ActionButton { text: qsTr("▶     插帧工作室"); Layout.fillWidth: true; highlighted: window.currentPage === 1; onClicked: window.currentPage = 1 }
                ActionButton { text: qsTr("≋     任务队列"); Layout.fillWidth: true; highlighted: window.currentPage === 2; onClicked: window.currentPage = 2 }
                ActionButton { text: qsTr("◈     模型中心"); Layout.fillWidth: true; highlighted: window.currentPage === 3; onClicked: window.currentPage = 3 }
                ActionButton { objectName: "comparisonButton"; text: qsTr("▣     对比预览"); Layout.fillWidth: true; visible: processingAvailable; highlighted: window.currentPage === 4; onClicked: window.currentPage = 4 }
                Item { Layout.fillHeight: true }
                Rectangle {
                    Layout.fillWidth: true
                    radius: 14
                    color: "#151f2c"
                    border.color: "#263548"
                    implicitHeight: deviceInfo.implicitHeight + 28
                    ColumnLayout {
                        id: deviceInfo
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 7
                        Label { text: qsTr("本机加速器"); color: "#91a1b4"; font.pixelSize: 11 }
                        Label {
                            text: window.info.gpu || (window.backendAvailable && studio.deviceInfo.available
                                ? studio.deviceInfo.name : qsTr("未检测到 NVIDIA GPU"))
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            font.bold: true
                        }
                        Label {
                            visible: !window.info.width && window.backendAvailable && studio.deviceInfo.available
                            text: qsTr("显存 %1 GB · CUDA %2").arg((Number(studio.deviceInfo.totalBytes) / 1073741824).toFixed(1)).arg(studio.deviceInfo.computeCapability)
                            color: "#91a1b4"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Label {
                            visible: !!window.info.width
                            text: !window.info.nvencContextOpened && window.info.softwareH264ContextOpened
                                ? qsTr("NVENC 不可用 · 自动使用软件 H.264")
                                : window.info.nvencContextOpened ? qsTr("NVENC 编码器可初始化") : qsTr("H.264 编码器不可用")
                            color: window.info.nvencContextOpened ? "#9ee6c8" : "#f0c98a"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        Label { text: qsTr("源文件不会被修改"); color: "#91a1b4"; font.pixelSize: 11 }
                    }
                }
            }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            id: pages
            currentIndex: window.currentPage
            HomePage {
                queueController: window.backendAvailable ? taskQueue : null
                processingAvailable: window.backendAvailable
                deviceInfo: window.backendAvailable ? studio.deviceInfo : ({})
                modelName: window.selectedModelDisplayName
                modelVerified: window.selectedModelVerified
                encoderHint: window.encoderHint()
                processingStatus: window.stateText(window.state)
                processing: window.active
                onChooseVideo: sourceDialog.open()
                onOpenStudio: window.currentPage = 1
                onOpenQueue: window.currentPage = 2
                onOpenModelCenter: window.currentPage = 3
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                spacing: 20
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: 32
                    spacing: 20
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 5
                            Label { text: qsTr("插帧工作室"); color: "#9ee6c8"; font.pixelSize: 12; font.bold: true }
                            Label { text: qsTr("让每一段运动都更流畅"); font.pixelSize: 30; font.bold: true }
                            Label { text: qsTr("选择视频与目标帧率，RifeLove 会保留原片并生成新文件。"); color: "#9aaac0"; wrapMode: Text.WordWrap }
                        }
                        Rectangle {
                            visible: processingAvailable
                            radius: 20
                            color: "#172a2a"
                            border.color: "#31534b"
                            implicitWidth: engineLabel.implicitWidth + 24
                            implicitHeight: engineLabel.implicitHeight + 16
                            Label { id: engineLabel; anchors.centerIn: parent; text: qsTr("●  RIFE 4.25 · CUDA"); color: "#a5e2c8"; font.pixelSize: 12 }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: importContent.implicitHeight + 36
                        color: "#151f2c"
                        radius: 12
                        border.color: drop.containsDrag ? "#9ee6c8" : "#263548"
                        DropArea {
                            id: drop
                            anchors.fill: parent
                            enabled: processingAvailable && !window.active
                            onDropped: function(event) { if (event.hasUrls && event.urls.length > 0) studio.openSource(event.urls[0]) }
                        }
                        ColumnLayout {
                            id: importContent
                            anchors.fill: parent
                            anchors.margins: 18
                            spacing: 10
                            RowLayout {
                            Label { text: qsTr("导入视频"); font.bold: true; font.pixelSize: 16; Layout.fillWidth: true }
                                ActionButton { text: qsTr("选择文件"); primary: true; enabled: processingAvailable && !window.active; onClicked: sourceDialog.open() }
                            }
                            Label { text: processingAvailable && studio.source ? studio.source : qsTr("将一个视频拖到这里，或选择文件"); wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; color: "#c4d1e4" }
                            Label { visible: !!window.info.width; text: (window.info.width || 0) + " × " + (window.info.height || 0) + "   ·   " + (window.info.rate || "—") + " FPS   ·   " + (window.info.codec || "") + "   ·   " + (window.info.depth || 0) + "-bit"; color: "#9aaac0" }
                            Label { visible: !!window.info.color; text: window.info.color || ""; color: "#9aaac0"; font.pixelSize: 12 }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("目标帧率"); font.bold: true }
                            ComboBox { id: rate; objectName: "targetRate"; Layout.fillWidth: true; editable: true; model: ["60", "120", "144", "240", "60000/1001"]; enabled: !window.active; onActivated: window.persistLocalDefaults(); onAccepted: window.persistLocalDefaults() }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("编码器"); font.bold: true }
                            ComboBox { id: encoder; objectName: "studioEncoder"; Layout.fillWidth: true; model: [qsTr("自动 H.264（优先 GPU）"), qsTr("H.264 软件"), qsTr("HEVC 软件"), qsTr("AV1 软件")]; enabled: !window.active; onActivated: { window.normalizeEncodingMode(); window.persistLocalDefaults() } }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                            Label { text: qsTr("输出文件"); font.bold: true }
                        Item { Layout.fillWidth: true }
                        ActionButton { text: qsTr("选择位置"); enabled: processingAvailable && !window.active; onClicked: { outputDialog.selectedFile = studio.localUrl(studio.output); outputDialog.open() } }
                    }
                    TextField { Layout.fillWidth: true; readOnly: true; text: processingAvailable ? studio.output : ""; placeholderText: qsTr("导入视频后自动生成新文件名"); selectByMouse: true }
                    ActionButton {
                        objectName: "studioAdvancedEncodingToggle"
                        text: window.encodingAdvancedOpen ? qsTr("收起高级编码") : qsTr("高级编码设置")
                        flat: true
                        Layout.alignment: Qt.AlignLeft
                        onClicked: window.encodingAdvancedOpen = !window.encodingAdvancedOpen
                    }
                    GroupBox {
                        title: qsTr("高级编码")
                        Layout.fillWidth: true
                        visible: window.encodingAdvancedOpen
                        Layout.preferredHeight: visible ? implicitHeight : 0
                        ColumnLayout {
                            anchors.fill: parent
                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("码率控制") }
                                    ComboBox {
                                        id: studioQualityMode
                                        objectName: "studioQualityMode"
                                        Layout.fillWidth: true
                                        model: encoder.currentIndex === 2 ? [qsTr("目标码率"), qsTr("恒定量化 QP")]
                                            : encoder.currentIndex === 3 ? [qsTr("目标码率"), qsTr("恒定质量 CRF")]
                                            : [qsTr("目标码率")]
                                        enabled: !window.active
                                        onActivated: window.persistLocalDefaults()
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: studioQualityMode.currentIndex === 1 ? (encoder.currentIndex === 2 ? qsTr("QP（0–51）") : qsTr("CRF（0–63）")) : qsTr("目标质量") }
                                    SpinBox {
                                        id: studioQuality
                                        objectName: "studioQuality"
                                        from: 0
                                        to: encoder.currentIndex === 2 ? 51 : 63
                                        value: 32
                                        enabled: !window.active && studioQualityMode.currentIndex === 1
                                        onValueModified: window.persistLocalDefaults()
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("目标码率（kbit/s）") }
                                    SpinBox {
                                        id: studioBitrate
                                        objectName: "studioBitrateKbps"
                                        from: 250
                                        to: 200000
                                        stepSize: 250
                                        value: 8000
                                        enabled: !window.active && studioQualityMode.currentIndex === 0
                                        onValueModified: window.persistLocalDefaults()
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Label { text: qsTr("关键帧间隔（帧）") }
                                    SpinBox {
                                        id: studioGop
                                        objectName: "studioGopSize"
                                        from: 0
                                        to: 1000
                                        value: 120
                                        enabled: !window.active
                                        onValueModified: window.persistLocalDefaults()
                                    }
                                }
                            }
                            Label {
                                text: encoder.currentIndex === 2 ? qsTr("HEVC 恒定量化使用 Kvazaar QP；较低 QP 通常会生成更大的文件。")
                                    : encoder.currentIndex === 3 ? qsTr("AV1 恒定质量使用 CRF；较低 CRF 通常会生成更大的文件。")
                                    : qsTr("当前 H.264 编码器使用目标码率。自动模式会在 NVENC 不可用时回退到软件 H.264。")
                                color: "#9aaac0"
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                    ActionButton {
                        text: window.advancedOpen ? qsTr("收起模型与运行库设置") : qsTr("高级设置 · 模型与运行库")
                        flat: true
                        Layout.alignment: Qt.AlignLeft
                        onClicked: window.advancedOpen = !window.advancedOpen
                    }
                    GroupBox {
                        title: qsTr("本地模型与运行库")
                        Layout.fillWidth: true
                        visible: window.advancedOpen
                        Layout.preferredHeight: visible ? implicitHeight : 0
                        ColumnLayout {
                            anchors.fill: parent
                            RowLayout {
                                Label { text: qsTr("模型包"); Layout.preferredWidth: 70 }
                                TextField { Layout.fillWidth: true; readOnly: true; text: window.modelFolder.toString(); placeholderText: qsTr("包含 model.json 的已校验模型包"); selectByMouse: true }
                                ActionButton { text: qsTr("浏览"); enabled: !window.active; onClicked: modelDialog.open() }
                            }
                            RowLayout {
                                Label { text: qsTr("运行库"); Layout.preferredWidth: 70 }
                                TextField { Layout.fillWidth: true; readOnly: true; text: window.runtimeFolder.toString(); placeholderText: qsTr("包含 ONNX Runtime / CUDA DLL 的目录"); selectByMouse: true }
                                ActionButton { text: qsTr("浏览"); enabled: !window.active; onClicked: runtimeDialog.open() }
                            }
                            Label { text: qsTr("当前已验证：RIFE 4.25、BT.709 及 BT.470BG/SMPTE 170M 8-bit SDR。HDR、10-bit 和未标记色彩的视频暂不支持。模型和运行库仅从所选本地目录加载。"); wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9aaac0"; font.pixelSize: 12 }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { objectName: "studioStatusLabel"; text: window.stateText(window.state); color: window.stateColor(window.state); font.bold: true; Layout.fillWidth: true }
                        Label {
                            objectName: "studioProgressLabel"
                            text: {
                                const frames = Number(window.progress.frames || 0)
                                const expected = Number(window.progress.estimatedFrames || 0)
                                const speed = Number(window.progress.fps || 0)
                                let value = expected > 0 ? qsTr("%1 / 约 %2 帧").arg(frames).arg(expected) : qsTr("%1 帧").arg(frames)
                                value += " · " + speed.toFixed(1) + qsTr(" 处理 FPS")
                                if (studio.busy) {
                                    if (expected > 0 && speed > 0)
                                        value += " · " + qsTr("剩余约 %1").arg(ProgressEstimate.formatDuration(Math.max(0,expected - frames) / speed))
                                    else
                                        value += " · " + qsTr("预计剩余时间计算中")
                                }
                                return value
                            }
                            color: "#9aaac0"
                        }
                        ActionButton { text: qsTr("取消"); visible: processingAvailable && studio.busy; enabled: window.state !== "cancelling"; onClicked: studio.cancel() }
                        ActionButton {
                            objectName: "startButton"
                            text: qsTr("开始补帧")
                            enabled: window.canStart
                            highlighted: true
                            primary: true
                            onClicked: studio.start(rate.editText, window.modelFolder, window.runtimeFolder,
                                ["auto", "libopenh264", "libkvazaar", "libaom-av1"][encoder.currentIndex], window.encodingOptions())
                        }
                    }
                    ProgressBar { Layout.fillWidth: true; indeterminate: studio.busy; visible: processingAvailable && studio.busy }
                    Label { text: processingAvailable ? studio.detail : qsTr("请使用启用媒体和 ONNX CUDA 的桌面构建。"); wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; color: window.state === "failed" ? "#ffb3ad" : "#b9c9dd"; font.pixelSize: 12 }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        visible: !window.info.width
                        Repeater {
                            model: [
                                {icon: "◉", title: qsTr("完全本地"), detail: qsTr("视频与模型只在本机处理")},
                                {icon: "↗", title: qsTr("安全输出"), detail: qsTr("保留原片，生成独立文件")},
                                {icon: "✦", title: qsTr("NVIDIA 加速"), detail: qsTr("RIFE 推理由 CUDA 驱动")}
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: 82
                                radius: 12
                                color: "#121b27"
                                border.color: "#202d3d"
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 14
                                    spacing: 12
                                    Label { text: modelData.icon; color: "#9ee6c8"; font.pixelSize: 20 }
                                    ColumnLayout {
                                        spacing: 4
                                        Layout.fillWidth: true
                                        Label { text: modelData.title; font.bold: true }
                                        Label { text: modelData.detail; color: "#91a1b4"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            }
            Loader {
                id: queuePageLoader
                Layout.fillWidth: true
                Layout.fillHeight: true
                active: processingAvailable
                sourceComponent: queuePageComponent
            }
            Loader {
                id: modelPageLoader
                Layout.fillWidth: true
                Layout.fillHeight: true
                active: processingAvailable
                sourceComponent: modelCenterComponent
            }
            Loader {
                id: comparisonPageLoader
                Layout.fillWidth: true
                Layout.fillHeight: true
                active: processingAvailable && window.currentPage === 4
                sourceComponent: comparisonPageComponent
            }
        }
    }
    Component {
        id: queuePageComponent
        QueuePage {
            queueController: taskQueue
            processingAvailable: true
        }
    }
    Component {
        id: modelCenterComponent
        ModelCenterPage { queueController: taskQueue }
    }
    Component {
        id: comparisonPageComponent
        ComparisonPage {
            originalSource: studio.source ? studio.localUrl(studio.source) : ""
            processedSource: window.state === "completed" && studio.output ? studio.localUrl(studio.output) : ""
        }
    }
    Connections {
        target: modelPageLoader.item
        function onDefaultModelSelected(directory) {
            window.modelFolder = directory
            if (queuePageLoader.item) queuePageLoader.item.modelFolder = directory
        }
    }
}
