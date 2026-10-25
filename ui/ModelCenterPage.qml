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
                Label { text: qsTr("管理已校验的 RIFE 模型包。模型文件只从本机目录读取；此页面不会联网或下载权重。"); color: "#697B8B"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
            ActionButton { text: qsTr("重新校验"); enabled: page.queueController.available && !page.queueController.busy && !page.queueController.importing && !page.queueController.verifyingModels; onClicked: page.queueController.refreshModels() }
            ActionButton { text: qsTr("添加本地模型"); primary: true; highlighted: true; enabled: page.queueController.available && !page.queueController.busy && !page.queueController.importing && !page.queueController.verifyingModels; onClicked: addModelDialog.open() }
        }
        Rectangle {
            Layout.fillWidth: true
            radius: 8
            color: "#DEE6EB"
            visible: page.queueController.detail !== "" || page.queueController.verifyingModels
            implicitHeight: modelDetail.implicitHeight + 20
            Label {
                id: modelDetail
                anchors.fill: parent
                anchors.margins: 10
                text: page.queueController.verifyingModels ? qsTr("正在后台读取并校验模型包；不会阻塞视频工作流界面。") : page.queueController.detail
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: page.queueController.state === "error" ? "#B33B44" : "#52697A"
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
                color: "#FFFFFF"
                border.color: page.isDefault(directory) ? "#13836B" : valid ? "#CBD6DC" : "#D58A8D"
                ColumnLayout {
                    id: modelCard
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: valid ? (modelData.displayName || modelData.id || qsTr("RIFE 模型")) + "  ·  " + (modelData.version || "") : qsTr("模型校验失败")
                            font.bold: true
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                        }
                        Label { visible: page.isDefault(directory); text: qsTr("默认模型"); color: "#13836B"; font.bold: true }
                        Label { visible: !valid; text: qsTr("不可用"); color: "#B33B44"; font.bold: true }
                    }
                    Label {
                        text: directory
                        color: "#697B8B"
                        font.pixelSize: 11
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: valid
                        text: qsTr("ONNX SHA-256  ") + (modelData.sha256 || "")
                        color: "#70818F"
                        font.pixelSize: 11
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: valid
                        text: qsTr("许可  ") + (modelData.licenseId || qsTr("未声明")) + "     " +
                              qsTr("数值档案  ") + (modelData.numericalProfile || qsTr("未记录")) + "     " +
                              qsTr("HDR  ") + (modelData.hdrQualified ? qsTr("已验证") : qsTr("未验证"))
                        color: "#52697A"
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: valid && (modelData.validatedBackends || []).length > 0
                        text: qsTr("已记录的验证后端  ") + (modelData.validatedBackends || []).join("、")
                        color: "#697B8B"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: valid && (modelData.sourceRepository || modelData.upstreamCommit)
                        text: qsTr("上游来源声明  ") + (modelData.sourceRepository || qsTr("未记录")) +
                              (modelData.upstreamCommit ? "  ·  " + modelData.upstreamCommit : "")
                        color: "#697B8B"
                        font.pixelSize: 11
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: valid && modelData.licenseScopeEvidence
                        text: qsTr("许可范围依据  ") + modelData.licenseScopeEvidence
                        color: "#697B8B"
                        font.pixelSize: 11
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: valid
                        text: modelData.metadataIntegrityVerified
                            ? qsTr("完整性校验通过：权重、许可证、来源清单和验证报告的 SHA-256 均匹配。此校验不代表作者签名或画质保证。")
                            : qsTr("旧版清单：权重 SHA-256 已校验；许可证、来源清单和验证报告未绑定 SHA-256。")
                        color: modelData.metadataIntegrityVerified ? "#13836B" : "#A46A12"
                        font.pixelSize: 11
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    Label {
                        visible: !valid
                        text: modelData.error || qsTr("模型包无法读取。")
                        color: "#B33B44"
                        font.pixelSize: 12
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        ActionButton {
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
                        ActionButton {
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
                color: "#70818F"
                horizontalAlignment: Text.AlignHCenter
                lineHeight: 1.5
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("目录校验会检查模型张量契约与所声明的文件哈希。后端列表和数值档案描述已有验证范围；它们不代表感知画质排名，HDR 支持只会在独立色彩与模型测试完成后标记。")
            color: "#70818F"
            wrapMode: Text.WordWrap
            font.pixelSize: 12
        }
    }
}
