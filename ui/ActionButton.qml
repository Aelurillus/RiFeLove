import QtQuick
import QtQuick.Controls

Button {
    id: control
    property bool primary: highlighted
    property bool navigation: false
    implicitHeight: navigation ? 46 : 40
    implicitWidth: Math.max(navigation ? 168 : 96, label.implicitWidth + leftPadding + rightPadding)
    focusPolicy: Qt.StrongFocus
    Accessible.name: text
    Keys.onReturnPressed: function(event) {
        if (control.enabled) control.click()
        event.accepted = true
    }
    Keys.onEnterPressed: function(event) {
        if (control.enabled) control.click()
        event.accepted = true
    }
    leftPadding: 18
    rightPadding: 18
    topPadding: 8
    bottomPadding: 8

    background: Rectangle {
        radius: control.navigation ? 10 : 9
        color: !control.enabled ? (control.navigation ? "transparent" : "#171f29")
            : control.navigation ? (control.highlighted ? "#17352f" : control.hovered ? "#192633" : "transparent")
            : control.primary ? (control.down ? "#41bd9b" : control.hovered ? "#92efd0" : "#73dfbd")
            : control.flat ? (control.hovered ? "#182431" : "transparent")
            : control.down ? "#263746" : control.hovered ? "#202f3d" : "#172330"
        border.width: control.activeFocus ? 2 : control.navigation || control.flat || control.primary ? 0 : 1
        border.color: control.activeFocus ? "#a8efd5" : "#2a3d4e"
        Rectangle {
            visible: control.navigation && control.highlighted
            x: 0
            y: 9
            width: 3
            height: parent.height - 18
            radius: 2
            color: "#73dfbd"
        }
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Text {
        id: label
        text: control.text
        color: control.navigation && control.highlighted ? "#b4f2dc"
            : control.primary && control.enabled ? "#09251e"
            : control.enabled ? "#d9e3ee" : "#657487"
        font.family: control.font.family
        font.pixelSize: 13
        font.weight: control.primary || (control.navigation && control.highlighted) ? Font.DemiBold : Font.Medium
        horizontalAlignment: control.navigation ? Text.AlignLeft : Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        leftPadding: control.navigation ? 7 : 0
        elide: Text.ElideRight
    }
}
