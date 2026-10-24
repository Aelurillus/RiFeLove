import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: page
    objectName: "modelCenterPage"
    required property var queueController
    signal defaultModelSelected(url directory)

    function currentModelPath() {
        const value = queueController.defaults ? queueController.defaults.model : null
        return value ? normalizePath(queueController.localPath(value)) : ""
    }
    function isDefault(directory) {
        return directory !== "" && normalizePath(directory) === currentModelPath()
    }
    function normalizePath(value) { return String(value).split("\\").join("/").toLowerCase() }
    onVisibleChanged: if (visible && queueController.available) queueController.refreshModels()

    FolderDialog {
        id: addModelDialog
        title: qsTr("选择本地 RIFE 模型包目录")
        onAccepted: page.queueController.addLocalModel(selectedFolder)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 30
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                Label { text: qsTr("本地模型中心"); font.pixelSize: 27; font.bold: true }
                Label { text: qsTr("管理已校验的 RIFE 模型包。模型文件只从本机目录读取；此页面不会联网或下载权重。"); color: "#9aaac0"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
            Button { text: qsTr("重新校验"); enabled: page.queueController.available && !page.queueController.busy && !page.queueController.importing && !page.queueController.verifyingModels; onClicked: page.queueController.refreshModels() }
            Button { text: qsTr("添加本地模型"); highlighted: true; enabled: page.queueController.available && !page.queueController.busy && !page.queueController.importing && !page.queueController.verifyingModels; onClicked: addModelDialog.open() }
        }
        Rectangle {
            Layout.fillWidth: true
            radius: 8
            color: "#202d3d"
            visible: page.queueController.detail !== "" || page.queueController.verifyingModels
            implicitHeight: modelDetail.implicitHeight + 20
            Label {
                id: modelDetail
                anchors.fill: parent
                anchors.margins: 10
                text: page.queueController.verifyingModels ? qsTr("正在后台读取并校验模型包；不会阻塞视频工作流界面。") : page.queueController.detail
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: page.queueController.state === "error" ? "#ffb3ad" : "#c4d1e4"
                font.pixelSize: 12
            }
        }
        ListView {
            id: modelsView
            objectName: "modelCatalogList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 10
            model: page.queueController.models
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                required property var modelData
                readonly property string directory: modelData.directory || ""
                readonly property bool valid: !!modelData.valid
                width: modelsView.width
                implicitHeight: modelCard.implicitHeight + 24
                radius: 9
                color: "#1a2432"
                border.color: page.isDefault(directory) ? "#79d6ba" : valid ? "#324259" : "#73434b"
                ColumnLayout {
                    id: modelCard
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: valid ? (modelData.id || qsTr("RIFE 模型")) + "  ·  " + (modelData.version || "") : qsTr("模型校验失败")
                            font.bold: true
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                        }
                        Label { visible: page.isDefault(directory); text: qsTr("默认模型"); color: "#79d6ba"; font.bold: true }
                        Label { visible: !valid; text: qsTr("不可用"); color: "#ffb3ad"; font.bold: true }
                    }
                    Label {
                        text: directory
                        color: "#9aaac0"
                        font.pixelSize: 11
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: valid
                        text: qsTr("ONNX SHA-256  ") + (modelData.sha256 || "")
                        color: "#8493aa"
                        font.pixelSize: 11
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: !valid
                        text: modelData.error || qsTr("模型包无法读取。")
                        color: "#ffb3ad"
                        font.pixelSize: 12
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Button {
                            objectName: "setDefaultModelButton"
                            text: qsTr("设为默认模型")
                            visible: valid && !page.isDefault(directory)
                            enabled: !page.queueController.busy && !page.queueController.importing && !page.queueController.verifyingModels
                            onClicked: {
                                const selected = page.queueController.localUrl(directory)
                                page.queueController.setDefaultModel(selected)
                                page.defaultModelSelected(selected)
                            }
                        }
                        Button {
                            text: qsTr("从目录列表移除")
                            visible: !page.isDefault(directory)
                            enabled: !page.queueController.busy && !page.queueController.importing && !page.queueController.verifyingModels
                            onClicked: page.queueController.removeLocalModel(page.queueController.localUrl(directory))
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                visible: !page.queueController.verifyingModels && (!page.queueController.models || page.queueController.models.length === 0)
                text: qsTr("尚未登记本地模型\n添加包含 model.json 的已校验模型包")
                color: "#8493aa"
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.5
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("目录校验会检查模型张量契约、随包许可证和权重 SHA-256。当前插帧适配器仅接受 RIFE FP32；是否适合当前视频仍由处理流水线进一步验证。")
            color: "#8493aa"
            wrapMode: Text.WordWrap
            font.pixelSize: 12
        }
    }
}
