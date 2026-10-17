import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    visible: true
    width: 1040
    height: 680
    minimumWidth: 800
    minimumHeight: 540
    title: qsTr("RifeLove")
    color: "#10131a"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 220
            color: "#171b24"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 22
                spacing: 18
                Label { text: "RifeLove"; color: "#f3f5fa"; font.pixelSize: 25; font.bold: true }
                Label { text: qsTr("Offline frame interpolation"); color: "#929bac"; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                Item { Layout.fillHeight: true }
                Label { text: qsTr("Development foundation"); color: "#929bac" }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 42
            spacing: 24

            Label { text: qsTr("Workspace"); color: "#f3f5fa"; font.pixelSize: 30; font.bold: true }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#171b24"
                radius: 16
                border.color: "#303746"
                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 16
                    Label { text: qsTr("The processing engine is being built"); color: "#f3f5fa"; font.pixelSize: 21; Layout.alignment: Qt.AlignHCenter }
                    Label { text: qsTr("Timeline planning and persistent tasks are available in the development CLI. Video import and AI interpolation are not ready yet."); color: "#aab2c0"; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; Layout.maximumWidth: 470; Layout.alignment: Qt.AlignHCenter }
                }
            }
        }
    }
}
