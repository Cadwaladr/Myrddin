import QtQuick
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "Star Citizen"
    subtitle: "Frames generated between the game's own, from its motion vectors and depth."
    headerExtra: StatePill {}

    readonly property bool live: backend.state === "running" && backend.layerSession
    readonly property var words: [
        "The game as it is, with none of Myrddin's layers loaded.",
        "One generated frame between every two of the game's.",
        "Two generated frames between every two of the game's.",
        "Three generated frames between every two of the game's."
    ]

    // what stands in the way of a launch
    Card {
        visible: !backend.ready
        color: Qt.rgba(Theme.bad.r, Theme.bad.g, Theme.bad.b, 0.07)
        border.color: Qt.rgba(Theme.bad.r, Theme.bad.g, Theme.bad.b, 0.4)
        padding: 16
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Icon { path: Theme.iconWarn; color: Theme.bad; size: 20 }
            Text {
                text: backend.blocker
                color: Theme.text
                font.pixelSize: 14
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            ActionButton { text: "Open Setup"; icon: Theme.iconArrow; onClicked: page.go("setup") }
        }
    }
    Card {
        visible: backend.runtimeNote !== ""
        padding: 16
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Icon { path: Theme.iconInfo; color: Theme.accent2; size: 20 }
            Text { text: backend.runtimeNote; color: Theme.text; font.pixelSize: 14; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
    }

    // the launch, or the running game's figures
    Card {
        id: hero
        padding: 28
        spacing: 0
        clip: true

        Item {
            Layout.fillWidth: true
            implicitHeight: page.live ? figures.implicitHeight : controls.implicitHeight
            Behavior on implicitHeight { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }

            RowLayout {
                id: controls
                width: parent.width
                spacing: 28
                opacity: page.live ? 0 : 1
                visible: opacity > 0.01
                Behavior on opacity { NumberAnimation { duration: Theme.smooth } }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 14
                    Text {
                        text: "Frames shown per game frame"
                        color: Theme.dim
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                    }
                    Segmented {
                        large: true
                        cell: 86
                        options: ["Off", "×2", "×3", "×4"]
                        index: backend.prefs.multiplier - 1
                        onChosen: (i) => backend.setMultiplier(i + 1)
                    }
                    Text {
                        id: explain
                        text: page.words[Math.max(0, Math.min(3, backend.prefs.multiplier - 1))]
                        color: Theme.dim
                        font.pixelSize: 14
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        onTextChanged: swap.restart()
                        SequentialAnimation {
                            id: swap
                            NumberAnimation { target: explain; property: "opacity"; from: 0.2; to: 1; duration: Theme.smooth }
                        }
                    }
                }
                ColumnLayout {
                    spacing: 10
                    Layout.alignment: Qt.AlignVCenter
                    ActionButton {
                        id: launchButton
                        kind: backend.state === "idle" ? "primary" : "plain"
                        large: true
                        Layout.alignment: Qt.AlignRight
                        Layout.minimumWidth: 190
                        enabled: backend.ready && backend.state === "idle"
                        busy: backend.state === "starting"
                        icon: backend.state === "idle" ? Theme.iconPlay : ""
                        text: backend.state === "idle" ? "Launch" : backend.state === "starting" ? "Starting" : backend.state === "launcher" ? "Launcher open" : "Running"
                        onClicked: backend.launch()
                    }
                    Text {
                        Layout.alignment: Qt.AlignRight
                        Layout.maximumWidth: 230
                        horizontalAlignment: Text.AlignRight
                        wrapMode: Text.WordWrap
                        color: Theme.faint
                        font.pixelSize: 12
                        text: backend.state === "launcher" ? "Start the game from the RSI Launcher."
                            : backend.state === "running" ? "This session does not write to Myrddin's log: it was started without the layer, or not from here. Generated frames come with a launch from this window."
                            : "Opens the RSI Launcher."
                    }
                }
            }

            ColumnLayout {
                id: figures
                width: parent.width
                spacing: 18
                opacity: page.live ? 1 : 0
                visible: opacity > 0.01
                Behavior on opacity { NumberAnimation { duration: Theme.smooth } }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 34
                    Figure { value: backend.shownRate; label: "shown"; unit: "fps"; tone: Theme.accent; size: 54 }
                    Figure { value: backend.baseRate; label: "the game's own"; unit: "fps"; size: 54 }
                    Figure { value: backend.frameP99; label: "frame time, 99th"; unit: "ms"; decimals: 1; size: 30; Layout.alignment: Qt.AlignBottom }
                    Item { Layout.fillWidth: true }
                    ColumnLayout {
                        spacing: 10
                        Layout.alignment: Qt.AlignVCenter
                        Segmented {
                            Layout.alignment: Qt.AlignRight
                            cell: 54
                            options: ["Off", "×2", "×3", "×4"]
                            index: backend.generating ? Math.max(1, backend.prefs.multiplier - 1) : 0
                            onChosen: (i) => backend.setMultiplier(i + 1)
                        }
                        Text {
                            Layout.alignment: Qt.AlignRight
                            color: Theme.faint
                            font.pixelSize: 12
                            text: backend.generating ? "Changes at once, in the running game." : "Generation is off; the game shows its own frames."
                        }
                    }
                }
                Sparkline {
                    Layout.fillWidth: true
                    implicitHeight: 76
                    shown: backend.shownHistory
                    base: backend.baseHistory
                }
            }
        }
    }

    // the switches most people touch
    Card {
        heading: backend.state === "idle" ? "At launch" : "At the next launch"
        GridLayout {
            Layout.fillWidth: true
            columns: page.width > 820 ? 2 : 1
            columnSpacing: 36
            rowSpacing: 18
            SettingRow {
                title: "MangoHud"
                detail: backend.mangohudInstalled ? "Its frame rate counts what reaches the display, generated frames included." : "Not installed on this system."
                dimmed: !backend.mangohudInstalled
                Toggle { checked: backend.conf.mangohud; enabled: backend.mangohudInstalled; onToggled: (v) => backend.conf.mangohud = v }
            }
            SettingRow {
                title: "Performance panel"
                detail: "Myrddin's own readout in the game: the shown rate, the game's rate, pacing."
                Toggle { checked: backend.prefs.panel; onToggled: (v) => backend.prefs.panel = v }
            }
            SettingRow {
                title: "Move HUD markers"
                detail: "Markers, the compass and flight displays move in every shown frame, not in steps."
                Toggle { checked: backend.conf.markers; onToggled: (v) => backend.conf.markers = v }
            }
            SettingRow {
                title: "Run with logs"
                detail: "Keeps a log of the session. When the game closes you can read it, copy it or delete it."
                Toggle { checked: backend.prefs.watch; onToggled: (v) => backend.prefs.watch = v }
            }
        }
    }

    // the same thing as a command
    Card {
        padding: 14
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Icon { path: Theme.iconTerminal; size: 18; color: Theme.faint; Layout.leftMargin: 6 }
            Text {
                text: backend.command
                color: Theme.dim
                font.family: Theme.mono
                font.pixelSize: 13
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
            Text { text: "the same from a terminal"; color: Theme.faint; font.pixelSize: 12 }
            ActionButton { kind: "quiet"; icon: Theme.iconCopy; onClicked: backend.copy(backend.command) }
        }
    }
}
