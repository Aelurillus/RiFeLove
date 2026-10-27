import QtQuick
import QtQuick.Controls

Item {
    id: root
    required property string profileId
    required property string controlObjectName
    signal activated(string profileId)
    readonly property var profileIds: ["custom", "youtube60", "bilibili60", "bilibili120", "douyin30", "douyin60"]
    readonly property var profileNames: [
        qsTr("本机高帧率母版 / 自定义"),
        qsTr("YouTube · 60 FPS 上传副本"),
        qsTr("Bilibili · 60 FPS 上传副本"),
        qsTr("Bilibili · 120 FPS 实验"),
        qsTr("抖音 · 30 FPS 上传副本"),
        qsTr("抖音 · 60 FPS 上传副本")
    ]
    implicitHeight: selector.implicitHeight

    function syncSelection() {
        const index = root.profileIds.indexOf(root.profileId)
        selector.currentIndex = index >= 0 ? index : 0
    }

    Component.onCompleted: syncSelection()
    onProfileIdChanged: syncSelection()

    ComboBox {
        id: selector
        objectName: root.controlObjectName
        anchors.fill: parent
        model: root.profileNames
        onActivated: function(index) { root.activated(root.profileIds[index]) }
    }
}
