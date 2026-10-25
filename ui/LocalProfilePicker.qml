import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: picker
    required property var controller
    property var values: ({})
    property bool available: true
    property string selectedId: ""
    property string message: ""
    property bool messageIsError: false
    implicitHeight: content.implicitHeight

    function refreshSelection() {
        const profiles = picker.controller ? picker.controller.profiles || [] : []
        for (let index = 0; index < profiles.length; ++index) {
            if (String(profiles[index].id) === picker.selectedId) {
                profileBox.currentIndex = index
                return
            }
        }
        picker.selectedId = ""
        profileBox.currentIndex = -1
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 4
        RowLayout {
            Layout.fillWidth: true
            Label { text: qsTr("本机配置"); Layout.preferredWidth: 82 }
            ComboBox {
                id: profileBox
                objectName: "localProfileCombo"
                Layout.fillWidth: true
                model: picker.controller ? picker.controller.profiles || [] : []
                textRole: "name"
                valueRole: "id"
                enabled: picker.available
                displayText: currentIndex >= 0 ? currentText : qsTr("选择要复用的设置")
                onActivated: picker.selectedId = String(currentValue)
            }
            ActionButton {
                text: qsTr("使用")
                enabled: picker.available && profileBox.currentIndex >= 0
                onClicked: picker.controller.applyProfile(String(profileBox.currentValue))
            }
            ActionButton {
                text: qsTr("保存当前设置")
                enabled: picker.available && !!picker.values
                onClicked: { profileName.text = ""; saveDialog.open() }
            }
            ActionButton {
                text: qsTr("删除")
                enabled: picker.available && profileBox.currentIndex >= 0
                onClicked: deleteDialog.open()
            }
        }
        Label {
            visible: picker.message.length > 0
            text: picker.message
            color: picker.messageIsError ? "#f0c98a" : "#91d9ba"
            font.pixelSize: 11
            Layout.leftMargin: 82
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }

    Dialog {
        id: saveDialog
        title: qsTr("保存本机配置")
        modal: true
        standardButtons: Dialog.Save | Dialog.Cancel
        anchors.centerIn: parent
        contentItem: ColumnLayout {
            implicitWidth: 360
            Label { text: qsTr("给这组视频设置取一个容易辨认的名称。"); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            TextField {
                id: profileName
                objectName: "localProfileName"
                Layout.fillWidth: true
                placeholderText: qsTr("例如：手机视频 60 FPS")
                maximumLength: 64
                selectByMouse: true
            }
        }
        onAccepted: picker.controller.saveProfile(profileName.text, picker.values)
    }

    Dialog {
        id: deleteDialog
        title: qsTr("删除本机配置")
        modal: true
        standardButtons: Dialog.Yes | Dialog.Cancel
        anchors.centerIn: parent
        Label {
            padding: 20
            text: qsTr("删除后，这组配置将从本机列表中移除。")
            wrapMode: Text.WordWrap
        }
        onAccepted: picker.controller.removeProfile(String(profileBox.currentValue))
    }

    Connections {
        target: picker.controller
        function onProfilesChanged() { picker.refreshSelection() }
        function onProfileOperationFinished(success, operation) {
            if (!success) {
                picker.messageIsError = true
                picker.message = qsTr("配置操作未完成，请检查名称和设置后重试！")
                return
            }
            picker.messageIsError = false
            if (operation === "save") picker.message = qsTr("配置已保存在本机。")
            else if (operation === "remove") picker.message = qsTr("本机配置已删除。")
            else picker.message = qsTr("已应用本机配置。")
        }
    }
}
