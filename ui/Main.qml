import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: window
    visible: true
    width: 1180
    height: 800
    minimumWidth: 960
    minimumHeight: 680
    title: "RifeLove"
    color: "#10151e"
    font.family: "Microsoft YaHei UI"
    font.pixelSize: 14
    palette.text: "#eaf0fa"
    palette.windowText: "#eaf0fa"
    palette.buttonText: "#eaf0fa"
    palette.base: "#10151e"
    palette.placeholderText: "#8493aa"
    palette.button: "#283447"
    palette.highlight: "#79d6ba"
    palette.highlightedText: "#10241f"
    property url modelFolder: ""
    property url runtimeFolder: ""
    property int currentPage: 0
    readonly property bool queueActive: processingAvailable && (taskQueue.busy || taskQueue.importing)
    readonly property bool active: processingAvailable && (studio.busy || queueActive)
    readonly property var info: processingAvailable ? studio.media : ({})
    readonly property var progress: processingAvailable ? studio.progress : ({})
    readonly property string state: processingAvailable ? studio.status : "unavailable"

    function stateText(value) {
        const labels = {ready: qsTr("等待导入"), inspecting: qsTr("读取视频信息"), inspected: qsTr("可以开始"),
            checking_source: qsTr("校验源文件"), waiting_for_engine: qsTr("等待处理引擎"),
            loading_model: qsTr("加载本地模型"), processing: qsTr("正在补帧"), encoding: qsTr("正在编码"),
            validating: qsTr("验证输出视频"), encoder_fallback: qsTr("切换编码器"), completed: qsTr("处理完成"),
            failed: qsTr("处理失败"), cancelling: qsTr("正在安全取消"), cancelled: qsTr("已取消"), unavailable: qsTr("处理组件未启用")}
        return labels[value] || qsTr("正在处理")
    }
    FileDialog {
        id: sourceDialog
        title: qsTr("选择视频")
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)"), qsTr("所有文件 (*)")]
        onAccepted: studio.openSource(selectedFile)
    }
    FileDialog {
        id: outputDialog
        title: qsTr("保存补帧视频")
        fileMode: FileDialog.SaveFile
        nameFilters: ["Matroska (*.mkv)", "MP4 (*.mp4)", "QuickTime (*.mov)"]
        onAccepted: studio.setOutput(selectedFile)
    }
    FolderDialog { id: modelDialog; title: qsTr("选择已校验的本地模型包"); onAccepted: window.modelFolder = selectedFolder }
    FolderDialog { id: runtimeDialog; title: qsTr("选择 ONNX CUDA 运行库目录"); onAccepted: window.runtimeFolder = selectedFolder }
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
            Layout.preferredWidth: 200
            color: "#171f2b"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 16
                Label { text: "RifeLove"; font.pixelSize: 26; font.bold: true }
                Label { text: qsTr("离线 AI 视频补帧"); color: "#9aaac0" }
                Rectangle { Layout.fillWidth: true; height: 1; color: "#2b394e"; Layout.topMargin: 16; Layout.bottomMargin: 10 }
                Button { text: qsTr("插帧工作室"); Layout.fillWidth: true; highlighted: window.currentPage === 0; onClicked: window.currentPage = 0 }
                Button { text: qsTr("任务队列"); Layout.fillWidth: true; highlighted: window.currentPage === 1; onClicked: window.currentPage = 1 }
                Button { text: qsTr("模型中心"); Layout.fillWidth: true; highlighted: window.currentPage === 2; onClicked: window.currentPage = 2 }
                Item { Layout.fillHeight: true }
                Label { text: qsTr("NVIDIA GPU"); color: "#9aaac0"; font.pixelSize: 12 }
                Label { text: window.info.gpu || qsTr("导入后检测"); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Label { text: qsTr("源文件保留\n输出保存为新文件"); color: "#9aaac0"; font.pixelSize: 12; lineHeight: 1.5 }
            }
        }
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            id: pages
            currentIndex: window.currentPage
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
                    Label { text: qsTr("让运动更连贯"); font.pixelSize: 28; font.bold: true }
                    Label { text: qsTr("选择本机视频，设置目标帧率，生成经过验证的补帧文件。"); color: "#9aaac0"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: importContent.implicitHeight + 36
                        color: "#1a2432"
                        radius: 12
                        border.color: drop.containsDrag ? "#79d6ba" : "#324259"
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
                                Label { text: qsTr("01  导入视频"); font.bold: true; Layout.fillWidth: true }
                                Button { text: qsTr("选择文件"); enabled: processingAvailable && !window.active; onClicked: sourceDialog.open() }
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
                            Label { text: qsTr("02  目标帧率"); font.bold: true }
                            ComboBox { id: rate; objectName: "targetRate"; Layout.fillWidth: true; editable: true; model: ["60", "120", "144", "240", "60000/1001"]; enabled: !window.active }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("编码器"); font.bold: true }
                            ComboBox { id: encoder; Layout.fillWidth: true; model: [qsTr("自动 H.264（优先 GPU）"), qsTr("H.264 软件"), qsTr("HEVC 软件"), qsTr("AV1 软件")]; enabled: !window.active }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("03  输出文件"); font.bold: true }
                        Item { Layout.fillWidth: true }
                        Button { text: qsTr("选择位置"); enabled: processingAvailable && !window.active; onClicked: { outputDialog.selectedFile = studio.localUrl(studio.output); outputDialog.open() } }
                    }
                    TextField { Layout.fillWidth: true; readOnly: true; text: processingAvailable ? studio.output : ""; placeholderText: qsTr("导入视频后自动生成新文件名"); selectByMouse: true }
                    GroupBox {
                        title: qsTr("本地模型与运行库")
                        Layout.fillWidth: true
                        ColumnLayout {
                            anchors.fill: parent
                            RowLayout {
                                Label { text: qsTr("模型包"); Layout.preferredWidth: 70 }
                                TextField { Layout.fillWidth: true; readOnly: true; text: window.modelFolder.toString(); placeholderText: qsTr("包含 model.json 的已校验模型包"); selectByMouse: true }
                                Button { text: qsTr("选择目录"); enabled: !window.active; onClicked: modelDialog.open() }
                            }
                            RowLayout {
                                Label { text: qsTr("运行库"); Layout.preferredWidth: 70 }
                                TextField { Layout.fillWidth: true; readOnly: true; text: window.runtimeFolder.toString(); placeholderText: qsTr("包含 ONNX Runtime / CUDA DLL 的目录"); selectByMouse: true }
                                Button { text: qsTr("选择目录"); enabled: !window.active; onClicked: runtimeDialog.open() }
                            }
                            Label { text: qsTr("当前已验证：RIFE 4.25、BT.709 8-bit SDR。HDR、10-bit 和未标记色彩的视频暂不支持。模型和运行库仅从所选本地目录加载。"); wrapMode: Text.WordWrap; Layout.fillWidth: true; color: "#9aaac0"; font.pixelSize: 12 }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: window.stateText(window.state); color: window.state === "failed" ? "#ffb3ad" : "#79d6ba"; font.bold: true; Layout.fillWidth: true }
                        Label { text: (window.progress.frames || 0) + qsTr(" 帧  ·  ") + Number(window.progress.fps || 0).toFixed(1) + qsTr(" 处理 FPS"); color: "#9aaac0" }
                        Button { text: qsTr("取消"); visible: processingAvailable && studio.busy; enabled: window.state !== "cancelling"; onClicked: studio.cancel() }
                        Button {
                            objectName: "startButton"
                            text: qsTr("开始补帧")
                            enabled: processingAvailable && !window.active && !!window.info.width && !!window.info.supportedColor && !!window.info.gpuAvailable && window.modelFolder.toString() !== "" && window.runtimeFolder.toString() !== ""
                            highlighted: true
                            onClicked: studio.start(rate.editText, window.modelFolder, window.runtimeFolder, ["auto", "libopenh264", "libkvazaar", "libaom-av1"][encoder.currentIndex])
                        }
                    }
                    ProgressBar { Layout.fillWidth: true; indeterminate: studio.busy; visible: processingAvailable && studio.busy }
                    Label { text: processingAvailable ? studio.detail : qsTr("请使用启用媒体和 ONNX CUDA 的桌面构建。"); wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; color: window.state === "failed" ? "#ffb3ad" : "#b9c9dd"; font.pixelSize: 12 }
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
    Connections {
        target: modelPageLoader.item
        function onDefaultModelSelected(directory) {
            window.modelFolder = directory
            if (queuePageLoader.item) queuePageLoader.item.modelFolder = directory
        }
    }
}
