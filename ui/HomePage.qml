import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: page
    objectName: "homePage"
    required property var queueController
    required property bool processingAvailable
    required property var deviceInfo
    required property var languageSettings
    required property string modelName
    required property bool modelVerified
    required property string encoderHint
    required property string processingStatus
    required property bool processing
    signal chooseVideo()
    signal openStudio()
    signal openQueue()
    signal openModelCenter()

    readonly property var counts: queueController ? queueController.counts : ({})
    readonly property var recentTasks: queueController ? queueController.recentTasks : []
    property string languageSaveError: ""

    function taskState(value) {
        const labels = {queued: qsTr("等待处理"), running: qsTr("正在处理"), paused: qsTr("已暂停"),
            interrupted: qsTr("已中断"), failed: qsTr("失败"), cancelled: qsTr("已取消"), completed: qsTr("已完成")}
        return labels[value] || value || qsTr("未知状态")
    }
    function taskStateColor(value) {
        if (value === "failed" || value === "interrupted") return "#B33B44"
        if (value === "running" || value === "completed") return "#16836B"
        return "#A66C13"
    }

    ScrollView {
        id: scroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: scroll.availableWidth
            spacing: 22

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 32
                spacing: 22

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 246
                    radius: 20
                    border.color: "#D5E4E2"
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#E7F3EF" }
                        GradientStop { position: 1; color: "#F5F8F9" }
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 28
                        spacing: 24
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 11
                            Label { text: qsTr("RIFELOVE · 本机工作区"); color: "#16836B"; font.pixelSize: 12; font.bold: true }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("让每一帧运动都更连贯")
                                font.pixelSize: 30
                                font.bold: true
                                wrapMode: Text.WordWrap
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("选择视频开始处理，或继续查看本机任务。原片保持不变，模型和输出都留在本机。")
                                color: "#667889"
                                wrapMode: Text.WordWrap
                            }
                            RowLayout {
                                Layout.topMargin: 7
                                spacing: 10
                                ActionButton { objectName: "homeImportButton"; text: qsTr("导入视频"); primary: true; enabled: page.processingAvailable && !page.processing; onClicked: page.chooseVideo() }
                                ActionButton { objectName: "homeStudioButton"; text: qsTr("打开工作室"); enabled: page.processingAvailable && !page.processing; onClicked: page.openStudio() }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 255
                            Layout.fillHeight: true
                            radius: 16
                            color: "#F7F9FA"
                            border.color: "#D5E0E3"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 11
                                Label { text: qsTr("当前工作状态"); color: "#647685"; font.pixelSize: 12 }
                                Label { text: page.processingStatus; font.pixelSize: 18; font.bold: true; color: page.processing ? "#16836B" : "#1D2C3A"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                Rectangle { Layout.fillWidth: true; height: 1; color: "#DEE6EA"; Layout.topMargin: 3; Layout.bottomMargin: 3 }
                                Label { text: qsTr("本机处理"); color: "#16836B"; font.bold: true }
                            Label { text: qsTr("视频仅在本机处理；自动模式会按驱动和源格式选择兼容的本地编码器。") ; color: "#647685"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            }
                        }
                    }
                }

                RowLayout {
                    objectName: "homeStatsRow"
                    Layout.fillWidth: true
                    spacing: 14
                    Rectangle {
                        objectName: "homeGpuCard"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 106
                        radius: 14
                        color: "#FFFFFF"
                        border.color: "#DEE6EA"
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 6
                            Label { text: qsTr("NVIDIA GPU"); color: "#647685"; font.pixelSize: 11 }
                            Label { objectName: "homeGpuNameValue"; text: page.deviceInfo && page.deviceInfo.available ? page.deviceInfo.name : qsTr("尚未检测到 GPU"); font.bold: true; font.pixelSize: 15; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label { text: page.deviceInfo && page.deviceInfo.available ? qsTr("显存 %1 GB · CUDA %2").arg((Number(page.deviceInfo.totalBytes) / 1073741824).toFixed(1)).arg(page.deviceInfo.computeCapability) : qsTr("需要可用的 NVIDIA 驱动"); color: "#647685"; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                    }
                    Rectangle {
                        objectName: "homeModelCard"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 106
                        radius: 14
                        color: "#FFFFFF"
                        border.color: "#DEE6EA"
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 6
                            Label { text: qsTr("本地模型"); color: "#647685"; font.pixelSize: 11 }
                            Label { objectName: "homeModelNameValue"; text: page.modelName || qsTr("尚未选择模型"); font.bold: true; font.pixelSize: 15; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label {
                                text: page.modelVerified ? qsTr("已通过本机文件校验")
                                    : page.queueController && page.queueController.verifyingModels ? qsTr("正在校验本地模型…")
                                    : qsTr("请在模型中心检查此模型")
                                color: page.modelVerified ? "#16836B" : "#647685"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                    }
                    Rectangle {
                        objectName: "homeQueueCard"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 106
                        radius: 14
                        color: "#FFFFFF"
                        border.color: "#DEE6EA"
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 6
                            Label { text: qsTr("任务队列"); color: "#647685"; font.pixelSize: 11 }
                            Label { objectName: "homeQueueCountValue"; text: qsTr("%1 项任务").arg(Number(page.counts.total || 0)); font.bold: true; font.pixelSize: 15; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label { text: qsTr("等待 %1 · 处理中 %2 · 已完成 %3").arg(Number(page.counts.queued || 0)).arg(Number(page.counts.running || 0)).arg(Number(page.counts.completed || 0)); color: "#647685"; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    radius: 16
                    color: "#FFFFFF"
                    border.color: "#DEE6EA"
                    implicitHeight: recentContent.implicitHeight + 32
                    ColumnLayout {
                        id: recentContent
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 12
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("最近任务"); font.pixelSize: 17; font.bold: true; Layout.fillWidth: true }
                            ActionButton { objectName: "homeQueueButton"; text: qsTr("查看全部"); flat: true; onClicked: page.openQueue() }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            visible: page.recentTasks.length === 0
                            Label { text: qsTr("这里会显示最近加入队列的视频。"); color: "#647685"; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; Layout.topMargin: 16; Layout.bottomMargin: 16 }
                        }
                        Repeater {
                            model: page.recentTasks
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: 57
                                radius: 10
                                color: "#F3F6F8"
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 14
                                    spacing: 12
                                    Rectangle { Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4; color: page.taskStateColor(modelData.taskState) }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label { text: modelData.fileName || qsTr("本地视频"); font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                        Label {
                                            text: modelData.taskState === "running" ? qsTr("%1 · %2 / 约 %3 帧").arg(page.taskState(modelData.taskState)).arg(Number(modelData.frames || 0)).arg(Number(modelData.estimatedFrames || 0)) : page.taskState(modelData.taskState)
                                            color: "#647685"
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                    Label { text: modelData.targetRate ? qsTr("目标 %1 FPS").arg(modelData.targetRate) : ""; color: "#697B8B"; font.pixelSize: 11 }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 3
                            spacing: 10
                            Label { text: page.encoderHint; color: "#647685"; font.pixelSize: 11; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            ActionButton { text: qsTr("模型中心"); flat: true; onClicked: page.openModelCenter() }
                        }
                    }
                }

                Rectangle {
                    objectName: "homeLanguageCard"
                    Layout.fillWidth: true
                    radius: 14
                    color: "#FFFFFF"
                    border.color: "#DEE6EA"
                    implicitHeight: languageContent.implicitHeight + 28
                    ColumnLayout {
                        id: languageContent
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 8
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("界面语言"); font.bold: true; Layout.fillWidth: true }
                            ComboBox {
                                id: languageSelector
                                objectName: "languageSelector"
                                textRole: "label"
                                valueRole: "code"
                                model: [
                                    {code: "zh_CN", label: "简体中文"},
                                    {code: "zh_TW", label: "繁體中文"},
                                    {code: "en", label: "English"},
                                    {code: "ja", label: "日本語"}
                                ]
                                currentIndex: {
                                    const index = indexOfValue(page.languageSettings.language)
                                    return index < 0 ? 0 : index
                                }
                                onActivated: function(index) {
                                    page.languageSaveError = page.languageSettings.selectLanguage(currentValue)
                                        ? "" : qsTr("无法保存界面语言设置。")
                                }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: page.languageSaveError || (page.languageSettings.restartRequired
                                ? qsTr("界面语言已保存；关闭并重新启动 RifeLove 后生效。")
                                : qsTr("界面语言设置只保存在本机。"))
                            color: page.languageSaveError ? "#B33B44" : "#647685"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }
}
