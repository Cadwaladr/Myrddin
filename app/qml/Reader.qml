import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What a report, the last launch or a session's log says, over the page that asked for it.
Item {
    id: reader
    readonly property bool up: backend.toolTitle !== ""
    readonly property bool log: backend.toolTitle.startsWith("Session log")
    visible: opacity > 0.01
    opacity: up ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
    onUpChanged: if (up) sheet.forceActiveFocus()

    Rectangle { anchors.fill: parent; color: Theme.scrim }
    // (a click beside the sheet puts it away; nothing behind it is reached meanwhile)
    MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: backend.closeTool(); onWheel: (wheel) => wheel.accepted = true }

    Rectangle {
        id: sheet
        width: Math.min(parent.width - 72, 900)
        height: parent.height - 60
        x: (parent.width - width) / 2
        y: reader.up ? 30 : 46
        radius: Theme.radius
        color: Theme.card
        border.width: 1
        border.color: Theme.line
        clip: true
        Behavior on y { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }
        Keys.onEscapePressed: backend.closeTool()
        MouseArea { anchors.fill: parent }

        RowLayout {
            id: bar
            x: 20
            y: 12
            width: parent.width - 32
            spacing: 8
            Text { text: backend.toolTitle; color: Theme.text; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideMiddle; Layout.maximumWidth: bar.width - 260 }
            Icon {
                visible: backend.toolBusy
                path: Theme.iconRefresh
                size: 14
                RotationAnimator on rotation { running: backend.toolBusy; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
            }
            Item { Layout.fillWidth: true }
            ActionButton { text: "Delete"; icon: Theme.iconTrash; visible: reader.log && !backend.toolBusy; onClicked: backend.deleteSessionLog() }
            ActionButton { text: "Copy"; icon: Theme.iconCopy; visible: backend.toolOutput !== "" && !backend.toolBusy; onClicked: reader.log ? backend.copySessionLog() : backend.copy(backend.toolOutput) }
            ActionButton { kind: "quiet"; icon: Theme.iconCross; onClicked: backend.closeTool() }
        }
        Rectangle { x: 0; y: 60; width: parent.width; height: 1; color: Theme.lineSoft }
        Flickable {
            anchors.fill: parent
            anchors.topMargin: 72
            anchors.leftMargin: 20
            anchors.rightMargin: 12
            anchors.bottomMargin: 14
            contentWidth: output.implicitWidth
            contentHeight: output.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
            Text {
                id: output
                text: backend.toolBusy ? "…" : backend.toolOutput
                color: Theme.dim
                font.family: Theme.mono
                font.pixelSize: 12
                lineHeight: 1.2
            }
        }
    }
}
