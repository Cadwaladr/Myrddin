import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// What the game's process is doing, read from outside it (as "top" would: its CPU time, memory and
// threads from /proc, the GPU from nvidia-smi), and the logs as they grow: the layer's, the game's own
// Game.log, and what the last launch said. Nothing here touches the game.
PageView {
    id: page
    title: "Console"
    subtitle: "The game's process and its logs, as they are now. Read from outside the game, the way a task manager does."
    signal go(string name)
    readonly property var info: gameConsole.info
    readonly property bool up: info.up === true
    onActiveChanged: gameConsole.active = active
    Component.onCompleted: gameConsole.active = active

    function size(gb) { return gb >= 10 ? gb.toFixed(1) : gb.toFixed(2) }
    function since(s) {
        if (!(s > 0)) return "–"
        var h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60), sec = Math.floor(s % 60)
        return h > 0 ? h + " h " + m + " min" : m > 0 ? m + " min " + sec + " s" : sec + " s"
    }

    Card {
        heading: "The game's process"
        note: page.up ? "StarCitizen.exe, process " + page.info.pid + ", running for " + page.since(page.info.seconds) + "."
                      : (page.info.launcher === true ? "The RSI Launcher is open; the game is not running yet." : "The game is not running.")
        GridLayout {
            Layout.fillWidth: true
            // four to a row where they fit (the game above, the GPU below), else two. (The card's width:
            // a grid is only as wide as what it holds.)
            columns: parent.width > 680 ? 4 : 2
            columnSpacing: 22
            rowSpacing: 18
            Figure { value: page.up ? page.info.cpu : 0; label: "CPU threads busy"; unit: "of " + (page.info.cpus || 0); decimals: 1; size: 30 }
            Figure { value: page.up ? page.info.rssGb : 0; label: "Memory in use"; unit: "GB"; decimals: page.up && page.info.rssGb >= 10 ? 1 : 2; size: 30 }
            Figure { value: page.up ? page.info.threads : 0; label: "Threads"; size: 30 }
            Figure { value: page.info.ramFreeGb || 0; label: "Free memory (this PC)"; unit: "GB"; decimals: 1; size: 30 }
            Figure { value: page.info.gpuBusy || 0; label: "GPU busy"; unit: "%"; size: 30 }
            Figure { value: page.info.vramGb || 0; label: "GPU memory"; unit: "of " + page.size(page.info.vramTotalGb || 0) + " GB"; decimals: 1; size: 30 }
            Figure { value: page.info.gpuWatts || 0; label: "GPU power"; unit: "W"; size: 30 }
            Figure { value: page.info.swapGb || 0; label: "In swap"; unit: "GB"; decimals: 1; size: 30 }
        }
        Text {
            Layout.fillWidth: true
            text: page.info.tuning || ""
            visible: text !== ""
            color: Theme.dim
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
    }

    Card {
        heading: "Logs"
        note: gameConsole.source === "game" ? "The game's own log (Game.log), as it grows." :
              gameConsole.source === "launch" ? "What the last launch said, and what was switched for the time the game runs." :
              "Myrddin's layer: the multiplier, the frame rates every ten seconds, what it set up."
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Segmented {
                options: ["Layer", "Game", "Launch"]
                cell: 86
                index: ["layer", "game", "launch"].indexOf(gameConsole.source)
                onChosen: (i) => gameConsole.setSource(["layer", "game", "launch"][i])
            }
            Item { Layout.fillWidth: true }
            ActionButton { text: "Copy"; icon: Theme.iconCopy; onClicked: backend.copy(gameConsole.text) }
            ActionButton { text: "Folder"; icon: Theme.iconFolder; onClicked: gameConsole.openFolder() }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 360
            radius: Theme.radius - 4
            color: Theme.field
            border.width: 1
            border.color: Theme.line
            Flickable {
                id: view
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                contentWidth: width
                contentHeight: lines.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                // (it follows the end while it is at the end; scrolled up, it stays where it is)
                property bool atEnd: true
                onContentYChanged: if (moving || dragging) atEnd = contentY >= contentHeight - height - 4
                onContentHeightChanged: if (atEnd) contentY = Math.max(0, contentHeight - height)
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                    contentItem: Rectangle { implicitWidth: 5; radius: 3; color: Theme.line }
                }
                TextEdit {
                    id: lines
                    width: view.width - 8
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    text: gameConsole.text !== "" ? gameConsole.text : "(nothing yet)"
                    color: gameConsole.text !== "" ? Theme.text : Theme.dim
                    selectionColor: Theme.accent
                    font.family: Theme.mono
                    font.pixelSize: 12
                }
            }
        }
    }
}
