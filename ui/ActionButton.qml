import QtQuick
import QtQuick.Controls

Button {
    id: control
    property bool primary: highlighted
    implicitHeight: 42
    implicitWidth: Math.max(92, label.implicitWidth + leftPadding + rightPadding)
    leftPadding: 18
    rightPadding: 18
    topPadding: 9
    bottomPadding: 9

    background: Rectangle {
        radius: control.height / 2
        color: !control.enabled ? "#1b2430"
            : control.primary ? (control.down ? "#80d8ba" : control.hovered ? "#c1f4df" : "#9ee6c8")
            : control.flat ? "transparent"
            : control.down ? "#354b61" : control.hovered ? "#304258" : "#26364a"
        border.width: control.flat || control.primary ? 0 : 1
        border.color: "#3a5069"
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Text {
        id: label
        text: control.text
        color: control.primary && control.enabled ? "#10241f" : control.enabled ? "#e6edf7" : "#718096"
        font.family: control.font.family
        font.pixelSize: 13
        font.weight: control.primary ? Font.DemiBold : Font.Medium
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
