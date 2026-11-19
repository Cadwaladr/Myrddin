import QtQuick
import QtQuick.Layouts

// What the game is doing, in a word, with a dot that breathes while something runs.
Rectangle {
    id: pill
    property string state: backend.state
    readonly property color tone: state === "running" ? Theme.good : state === "idle" ? Theme.faint : Theme.warn
    readonly property string words: state === "running" ? (backend.layerSession ? (backend.generating ? "In game, generating" : "In game, generation off") : "In game, not steered from here")
                                  : state === "launcher" ? "RSI Launcher open" : state === "starting" ? "Starting" : "Not running"
    implicitWidth: row.implicitWidth + 26
    implicitHeight: 30
    radius: 15
    color: Qt.rgba(tone.r, tone.g, tone.b, 0.12)
    border.width: 1
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.35)
    Behavior on color { ColorAnimation { duration: Theme.slow } }
    Behavior on implicitWidth { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 8
        Rectangle {
            width: 8
            height: 8
            radius: 4
            color: pill.tone
            SequentialAnimation on opacity {
                running: pill.state !== "idle" && Qt.application.state === Qt.ApplicationActive && !picture
                loops: Animation.Infinite
                NumberAnimation { from: 1; to: 0.35; duration: 900; easing.type: Easing.InOutSine }
                NumberAnimation { from: 0.35; to: 1; duration: 900; easing.type: Easing.InOutSine }
            }
        }
        Text {
            text: pill.words
            color: pill.tone
            font.pixelSize: 12
            font.weight: Font.DemiBold
        }
    }
}
