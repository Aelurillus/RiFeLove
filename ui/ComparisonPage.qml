import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtMultimedia

Rectangle {
    id: page
    objectName: "comparisonPage"
    color: "#0b1018"

    property url originalSource: ""
    property url processedSource: ""
    property bool synchronize: true
    property real durationMs: Math.max(originalPlayer.duration, processedPlayer.duration)
    property real frameRate: 60
    readonly property bool originalReady: originalPlayer.hasVideo && originalPlayer.duration > 0
    readonly property bool processedReady: processedPlayer.hasVideo && processedPlayer.duration > 0
    readonly property real originalPosition: originalPlayer.position
    readonly property real processedPosition: processedPlayer.position
    readonly property bool playbackActive: originalPlayer.playing
    readonly property bool exactPreviewAvailable: typeof comparisonPreview !== "undefined" && comparisonPreview !== null
    readonly property bool exactPreviewVisible: page.exactPreviewAvailable && comparisonPreview.ready && !page.playbackActive
    property real pendingPreviewPosition: 0

    function clampPosition(value) {
        const maximum = Math.max(0, Math.min(originalPlayer.duration || value, processedPlayer.duration || value))
        return Math.max(0, Math.min(maximum, value))
    }
    function seek(value) {
        if (!originalReady || !processedReady) return
        const target = clampPosition(value)
        originalPlayer.position = target
        processedPlayer.position = target
    }
    function syncProcessed() {
        if (!synchronize || !originalReady || !processedReady) return
        if (Math.abs(processedPlayer.position - originalPlayer.position) > 80)
            processedPlayer.position = clampPosition(originalPlayer.position)
    }
    function updateFrameRate() {
        let value = Number(processedPlayer.metaData.value(MediaMetaData.VideoFrameRate))
        if (!(value > 0)) value = Number(originalPlayer.metaData.value(MediaMetaData.VideoFrameRate))
        frameRate = value > 0 ? value : 60
    }
    function updatePreviewIfPossible() {
        if (!page.exactPreviewAvailable || !page.originalReady || !page.processedReady || page.playbackActive) return
        page.scheduleExactPreview(processedPlayer.position)
    }
    function scheduleExactPreview(value) {
        if (!page.exactPreviewAvailable || !page.originalReady || !page.processedReady) return
        pendingPreviewPosition = clampPosition(value)
        previewDebounce.restart()
    }
    function requestExactPreview(value) {
        if (!page.exactPreviewAvailable || !page.originalReady || !page.processedReady) return
        comparisonPreview.requestFrames(originalSource, processedSource, clampPosition(value))
    }
    function stepFrame(direction) {
        updateFrameRate()
        if (page.exactPreviewAvailable && page.originalReady && page.processedReady) {
            originalPlayer.pause()
            processedPlayer.pause()
            if (comparisonPreview.ready) comparisonPreview.stepFrame(direction)
            else requestExactPreview(originalPlayer.position + direction * 1000 / frameRate)
            return
        }
        seek(originalPlayer.position + direction * 1000 / frameRate)
    }
    function togglePlayback() {
        if (!originalReady || !processedReady) return
        if (originalPlayer.playing) {
            originalPlayer.pause()
            processedPlayer.pause()
            if (page.exactPreviewAvailable) scheduleExactPreview(originalPlayer.position)
        } else {
            if (page.exactPreviewAvailable) comparisonPreview.clear()
            if (originalPlayer.position >= durationMs - 20) seek(0)
            originalPlayer.play()
            processedPlayer.play()
        }
    }
    function formatTime(milliseconds) {
        const total = Math.max(0, Math.floor(milliseconds / 1000))
        const minutes = Math.floor(total / 60)
        const seconds = total % 60
        const millis = Math.max(0, Math.floor(milliseconds) % 1000)
        return minutes.toString().padStart(2, "0") + ":" + seconds.toString().padStart(2, "0") + "." + millis.toString().padStart(3, "0")
    }
    function displayName(url) {
        const value = url.toString()
        if (!value) return qsTr("尚未选择视频")
        const parts = value.split("/")
        return decodeURIComponent(parts[parts.length - 1] || value)
    }

    MediaPlayer {
        id: originalPlayer
        source: page.originalSource
        videoOutput: originalVideo
        audioOutput: AudioOutput { volume: 0.7 }
        onPositionChanged: page.syncProcessed()
        onMetaDataChanged: page.updateFrameRate()
        onDurationChanged: page.syncProcessed()
    }
    MediaPlayer {
        id: processedPlayer
        source: page.processedSource
        videoOutput: processedVideo
        audioOutput: AudioOutput { muted: true }
        onMetaDataChanged: page.updateFrameRate()
        onDurationChanged: page.syncProcessed()
    }
    Timer {
        id: previewDebounce
        interval: 90
        repeat: false
        onTriggered: page.requestExactPreview(page.pendingPreviewPosition)
    }
    Connections {
        target: page.exactPreviewAvailable ? comparisonPreview : null
        function onFrameReady(outputPositionMs) { page.seek(outputPositionMs) }
    }
    Connections {
        target: originalPlayer
        function onHasVideoChanged() { page.updatePreviewIfPossible() }
    }
    Connections {
        target: processedPlayer
        function onHasVideoChanged() { page.updatePreviewIfPossible() }
    }

    FileDialog {
        id: originalDialog
        title: qsTr("选择原视频")
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)"), qsTr("所有文件 (*)")]
        onAccepted: { if (page.exactPreviewAvailable) comparisonPreview.clear(); page.originalSource = selectedFile; page.updatePreviewIfPossible() }
    }
    FileDialog {
        id: processedDialog
        title: qsTr("选择补帧视频")
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts)"), qsTr("所有文件 (*)")]
        onAccepted: { if (page.exactPreviewAvailable) comparisonPreview.clear(); page.processedSource = selectedFile; page.updatePreviewIfPossible() }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 30
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5
                Label { text: qsTr("对比预览"); color: "#9ee6c8"; font.pixelSize: 12; font.bold: true }
                Label { text: qsTr("同步播放、分屏比较，逐帧检查补帧结果"); font.pixelSize: 27; font.bold: true }
                Label { text: qsTr("本地播放，不会上传视频。步进速度根据补帧视频的帧率读取。"); color: "#9aaac0"; wrapMode: Text.WordWrap }
            }
            CheckBox {
                objectName: "comparisonSync"
                text: qsTr("同步播放")
                checked: page.synchronize
                onToggled: page.synchronize = checked
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            Repeater {
                model: [
                    { title: qsTr("原视频"), name: page.displayName(page.originalSource), button: qsTr("选择原片") },
                    { title: qsTr("补帧视频"), name: page.displayName(page.processedSource), button: qsTr("选择补帧结果") }
                ]
                delegate: Rectangle {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 56
                    radius: 10
                    color: "#151f2c"
                    border.color: "#263548"
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        Label { text: modelData.title; font.bold: true }
                        Label { text: modelData.name; color: "#9aaac0"; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        ActionButton {
                            text: modelData.button
                            onClicked: index === 0 ? originalDialog.open() : processedDialog.open()
                        }
                        ActionButton {
                            objectName: index === 0 ? "comparisonOpenOriginalFolder" : "comparisonOpenProcessedFolder"
                            text: qsTr("打开位置")
                            visible: page.exactPreviewAvailable
                            enabled: visible && (index === 0 ? page.originalReady : page.processedReady)
                            onClicked: comparisonPreview.openContainingFolder(index === 0 ? page.originalSource : page.processedSource)
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 14
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 240
                radius: 12
                color: "#05080d"
                border.color: "#263548"
                clip: true
                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 12
                    z: 2
                    text: qsTr("原视频")
                    color: "#eaf0fa"
                    background: Rectangle { color: "#990b1018"; radius: 5 }
                    padding: 6
                }
                VideoOutput { id: originalVideo; anchors.fill: parent; fillMode: VideoOutput.PreserveAspectFit; visible: !page.exactPreviewVisible }
                Image { objectName: "exactOriginalFrameImage"; anchors.fill: parent; visible: page.exactPreviewVisible; source: page.exactPreviewVisible ? comparisonPreview.originalFrameUrl : ""; fillMode: Image.PreserveAspectFit; cache: false; asynchronous: false }
                Label { anchors.centerIn: parent; visible: !originalPlayer.hasVideo; text: qsTr("选择视频以开始预览"); color: "#8493aa" }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 240
                radius: 12
                color: "#05080d"
                border.color: "#263548"
                clip: true
                Label {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 12
                    z: 2
                    text: qsTr("补帧视频")
                    color: "#eaf0fa"
                    background: Rectangle { color: "#990b1018"; radius: 5 }
                    padding: 6
                }
                VideoOutput { id: processedVideo; anchors.fill: parent; fillMode: VideoOutput.PreserveAspectFit; visible: !page.exactPreviewVisible }
                Image { objectName: "exactProcessedFrameImage"; anchors.fill: parent; visible: page.exactPreviewVisible; source: page.exactPreviewVisible ? comparisonPreview.processedFrameUrl : ""; fillMode: Image.PreserveAspectFit; cache: false; asynchronous: false }
                Label { anchors.centerIn: parent; visible: !processedPlayer.hasVideo; text: qsTr("选择视频以开始预览"); color: "#8493aa" }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            ActionButton {
                objectName: "comparisonPreviousFrame"
                text: "◀|"
                enabled: page.originalReady && page.processedReady && (!page.exactPreviewAvailable || !comparisonPreview.busy)
                onClicked: page.stepFrame(-1)
            }
            ActionButton {
                objectName: "comparisonPlayback"
                text: page.playbackActive ? qsTr("暂停") : qsTr("播放")
                primary: true
                enabled: page.originalReady && page.processedReady
                onClicked: page.togglePlayback()
            }
            ActionButton {
                objectName: "comparisonNextFrame"
                text: "|▶"
                enabled: page.originalReady && page.processedReady && (!page.exactPreviewAvailable || !comparisonPreview.busy)
                onClicked: page.stepFrame(1)
            }
            Label { text: page.formatTime(originalPlayer.position); color: "#9aaac0"; font.family: "Consolas" }
            Slider {
                objectName: "comparisonTimeline"
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, page.durationMs)
                value: originalPlayer.position
                enabled: page.originalReady && page.processedReady
                onMoved: { page.seek(value); page.scheduleExactPreview(value) }
            }
            Label { text: page.formatTime(page.durationMs); color: "#9aaac0"; font.family: "Consolas" }
            Label { text: qsTr("步进 1/%1 秒").arg(Number(page.frameRate).toFixed(3)); color: "#9aaac0"; font.pixelSize: 12 }
        }
        Label {
            objectName: "comparisonStatus"
            Layout.fillWidth: true
            text: (page.exactPreviewAvailable && comparisonPreview.navigationErrorString) ||
                  (page.exactPreviewAvailable && comparisonPreview.errorString) || originalPlayer.errorString || processedPlayer.errorString ||
                  (page.exactPreviewVisible ? qsTr("已按 FFmpeg 实际解码帧精确预览 · 输出时间 %1").arg(page.formatTime(comparisonPreview.outputTimestampMs)) :
                   page.exactPreviewAvailable && comparisonPreview.busy ? qsTr("正在精确解码所选时间的原片与补帧画面…") :
                   page.originalReady && page.processedReady ? qsTr("两路视频按时间戳同步；输出帧率 %1 FPS").arg(Number(page.frameRate).toFixed(3)) : qsTr("请选择原视频和补帧结果"))
            color: (page.exactPreviewAvailable && comparisonPreview.navigationErrorString) ||
                   (page.exactPreviewAvailable && comparisonPreview.errorString) || originalPlayer.errorString || processedPlayer.errorString ? "#ffb3ad" : "#91a1b4"
            wrapMode: Text.WrapAnywhere
        }
    }
}
