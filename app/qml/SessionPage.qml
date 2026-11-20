import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "Session"
    subtitle: "What the layer reports while the game runs, and the reports sc-turbo can make."
    headerExtra: StatePill {}

    readonly property bool live: backend.state === "running" && backend.layerSession

    Card {
        heading: "Frame rates"
        note: page.live ? "A point every ten seconds, the last fifteen minutes." : "Figures appear here while the game runs with the layer."
        RowLayout {
            Layout.fillWidth: true
            spacing: 34
            opacity: page.live ? 1 : 0.35
            Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
            Figure { value: backend.shownRate; label: "shown"; unit: "fps"; tone: Theme.accent }
            Figure { value: backend.baseRate; label: "the game's own"; unit: "fps" }
            Figure { value: backend.liveMultiplier; label: "multiplier"; unit: "×" }
            Figure { value: backend.frameP50; label: "frame time, median"; unit: "ms"; decimals: 1; size: 30; Layout.alignment: Qt.AlignBottom }
            Figure { value: backend.frameP99; label: "99th"; unit: "ms"; decimals: 1; size: 30; Layout.alignment: Qt.AlignBottom }
            Item { Layout.fillWidth: true }
        }
        Sparkline {
            Layout.fillWidth: true
            implicitHeight: 120
            opacity: page.live ? 1 : 0.3
            shown: backend.shownHistory
            base: backend.baseHistory
        }
    }

    Card {
        heading: "Reports"
        note: "Each runs an sc-turbo command and shows what it prints. Most need the game to be running. Session log shows the newest log of a session that ran with logs."
        Flow {
            Layout.fillWidth: true
            spacing: 10
            ActionButton { text: "Last ten minutes"; enabled: !backend.toolBusy; onClicked: backend.runTool("Last ten minutes", ["fg-watch", "--last", "60"]) }
            ActionButton { text: "Latency"; enabled: !backend.toolBusy; onClicked: backend.runTool("Latency", ["latency"]) }
            ActionButton { text: "Session check"; enabled: !backend.toolBusy; onClicked: backend.runTool("Session check", ["check"]) }
            ActionButton { text: "Displays"; enabled: !backend.toolBusy; onClicked: backend.runTool("Displays", ["displays"]) }
            ActionButton { text: "Threads"; enabled: !backend.toolBusy; onClicked: backend.runTool("Threads", ["threads"]) }
            ActionButton { text: "Session log"; icon: Theme.iconLog; enabled: !backend.toolBusy; onClicked: backend.readSessionLog() }
            ActionButton { text: "Where the layer stands"; enabled: !backend.toolBusy; onClicked: backend.runTool("Where the layer stands", ["fg-where"]) }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: backend.toolTitle !== ""
            implicitHeight: Math.min(Math.max(output.implicitHeight + 54, 90), 380)
            radius: Theme.radiusSmall
            color: Theme.field
            border.width: 1
            border.color: Theme.line
            clip: true
            Behavior on implicitHeight { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }

            RowLayout {
                id: bar
                x: 14
                y: 10
                width: parent.width - 22
                spacing: 8
                Text { text: backend.toolTitle; color: Theme.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                Icon {
                    visible: backend.toolBusy
                    path: Theme.iconRefresh
                    size: 14
                    RotationAnimator on rotation { running: backend.toolBusy; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
                }
                Item { Layout.fillWidth: true }
                ActionButton { kind: "quiet"; icon: Theme.iconTrash; visible: backend.toolTitle.startsWith("Session log") && !backend.toolBusy; onClicked: backend.deleteSessionLog() }
                ActionButton { kind: "quiet"; icon: Theme.iconCopy; visible: backend.toolOutput !== ""; onClicked: backend.toolTitle.startsWith("Session log") ? backend.copySessionLog() : backend.copy(backend.toolOutput) }
            }
            Flickable {
                anchors.fill: parent
                anchors.topMargin: 44
                anchors.margins: 14
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

    Card {
        heading: "The last launch"
        note: backend.launchLog === "" ? "What sc-turbo says when it starts the game shows up here: the display it found, the runner, anything in the way." : ""
        Text {
            visible: backend.launchLog !== ""
            text: backend.launchLog
            color: Theme.dim
            font.family: Theme.mono
            font.pixelSize: 12
            lineHeight: 1.2
            wrapMode: Text.WrapAnywhere
            Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ActionButton { text: "Open the log folder"; icon: Theme.iconFolder; onClicked: backend.openFolder("logs") }
        }
    }
}
