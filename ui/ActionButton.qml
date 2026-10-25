import QtQuick
import QtQuick.Controls

Button {
    id: control
    property bool primary: highlighted
    property bool navigation: false
    property string iconText: ""
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
        color: !control.enabled ? (control.navigation ? "transparent" : "#EDF1F4")
            : control.navigation ? (control.highlighted ? "#E3F2EC" : control.hovered ? "#EAF1F3" : "transparent")
            : control.primary ? (control.down ? "#0E765F" : control.hovered ? "#147C66" : "#1C8D73")
            : control.flat ? (control.hovered ? "#EDF2F5" : "transparent")
            : control.down ? "#DCE4E9" : control.hovered ? "#DCE4E9" : "#EDF2F5"
        border.width: control.activeFocus ? 2 : control.navigation || control.flat || control.primary ? 0 : 1
        border.color: control.activeFocus ? "#12816A" : "#D0DDDF"
        Rectangle {
            visible: control.navigation && control.highlighted
            x: 0
            y: 9
            width: 3
            height: parent.height - 18
            radius: 2
            color: "#1C8D73"
        }
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Item {
        Text {
            visible: control.navigation && control.iconText.length > 0
            x: 15
            width: 22
            height: parent.height
            text: control.iconText
            color: control.highlighted ? "#0C6C57" : "#70818F"
            font.family: "Segoe UI Symbol"
            font.pixelSize: 16
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        Text {
            id: label
            x: control.navigation ? 47 : 0
            width: Math.max(0, parent.width - x)
            height: parent.height
            text: control.text
            color: control.navigation && control.highlighted ? "#0C6C57"
                : control.primary && control.enabled ? "#FFFFFF"
                : control.enabled ? "#304353" : "#758692"
            font.family: control.font.family
            font.pixelSize: 13
            font.weight: control.primary || (control.navigation && control.highlighted) ? Font.DemiBold : Font.Medium
            horizontalAlignment: control.navigation ? Text.AlignLeft : Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
}
