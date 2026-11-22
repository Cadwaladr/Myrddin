import QtQuick
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "Star Citizen"
    subtitle: "Adds frames between the game's own, worked out from how the picture moves."
    headerExtra: StatePill {}

    readonly property bool live: backend.state === "running" && backend.layerSession
    readonly property bool roomy: width > 820     // (a window at its smallest is narrower)

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

    // 1. what the game is launched with, and the launch. (This card and the session's below it are
    // both in view in a window of the size it opens with: what is added here has to fit.)
    Card {
        padding: 24

        RowLayout {
            Layout.fillWidth: true
            spacing: 28

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: 10
                Caption { text: "Frame generation" }
                ModePicker { Layout.fillWidth: true; live: page.live; brief: true }

                // (Whether Auto limits the game's own frames is in Options: "Limit the game's own frame rate")
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 10
                    Caption { text: "Performance overlay" }
                    OverlayChoice { id: overlay }
                    Text {
                        text: overlay.few + (backend.state === "idle" || overlay.which === "off" ? "" : " From the next launch.")
                        color: Theme.dim
                        font.pixelSize: 13
                        lineHeight: 1.2
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            ColumnLayout {
                spacing: 12
                Layout.alignment: Qt.AlignVCenter
                // Launch. While the RSI Launcher or the game is up, the button says so, and "Stop" with
                // the pointer on it: that asks the session to end. Should it still be there two seconds
                // later, the button, which says "Stopping" meanwhile, offers "Kill" under the pointer.
                LaunchButton {
                    id: launch
                    readonly property bool up: backend.state === "launcher" || backend.state === "running"
                    readonly property bool kill: backend.stopping && backend.canKill && pointed
                    Layout.alignment: Qt.AlignRight
                    reserve: "Launcher open"
                    primary: backend.state === "idle"
                    active: backend.state === "idle" ? backend.ready : backend.stopping ? backend.canKill : up
                    busy: backend.state === "starting" || (backend.stopping && !kill)
                    danger: kill
                    icon: backend.state === "idle" ? Theme.iconPlay : kill ? Theme.iconCross : up && !backend.stopping && pointed ? Theme.iconStop : ""
                    text: backend.state === "idle" ? "Launch" : backend.state === "starting" ? "Starting"
                        : kill ? "Kill" : backend.stopping ? "Stopping" : pointed ? "Stop"
                        : backend.state === "launcher" ? "Launcher open" : "Running"
                    checked: backend.prefs.watch
                    onClicked: backend.state === "idle" ? backend.launch() : backend.stopping ? backend.kill() : backend.stop()
                    onToggled: (v) => backend.prefs.watch = v
                }
                Text {
                    // (as wide and at least as high whatever it says: the button above it stays where
                    // it is when its words change under the pointer)
                    Layout.alignment: Qt.AlignRight
                    Layout.preferredWidth: 230
                    Layout.minimumHeight: 30
                    horizontalAlignment: Text.AlignRight
                    wrapMode: Text.WordWrap
                    color: Theme.faint
                    font.pixelSize: 12
                    lineHeight: 1.2
                    text: backend.stopping ? (backend.canKill ? "Part of the session is still running. Kill ends it at once." : "Asking the game and the RSI Launcher to close.")
                        : backend.state === "launcher" ? "Start the game from the RSI Launcher."
                        : backend.state === "running" ? (page.live ? "Myrddin is running with the game: changes here apply straight away." : "This window cannot follow this game: it was started without Myrddin, or from somewhere else. Launch from here to add frames.")
                        : backend.prefs.watch ? "Opens the RSI Launcher. The session's log is offered when the game closes." : "Opens the RSI Launcher."
                }
            }
        }
    }

    // 2. the session: frame rates while the game runs with the layer
    Card {
        heading: "Session"
        note: page.live ? "Frame rates: a point every ten seconds, the last fifteen minutes."
                        : backend.layerProblem !== "" ? "" : "Frame rates appear here while the game runs with Myrddin."
        // (the last session's layer log says something went wrong: a report of it, in place of the line above)
        RowLayout {
            visible: !page.live && backend.layerProblem !== ""
            Layout.fillWidth: true
            spacing: 10
            Icon { path: Theme.iconWarn; color: Theme.warn; size: 18 }
            Text { text: backend.layerProblem; color: Theme.text; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true }
            ActionButton { text: "Report a bug"; onClicked: backend.makeBugReport() }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: page.roomy ? 34 : 18
            opacity: page.live ? 1 : 0.35
            Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
            Figure { value: backend.shownRate; label: "shown"; unit: "fps"; tone: Theme.accent }
            Figure { value: backend.baseRate; label: page.roomy ? "the game's own" : "game's own"; unit: "fps" }
            Figure { value: backend.liveMultiplier; label: "multiplier"; unit: "×" }
            Figure { value: backend.frameP50; label: page.roomy ? "frame time, median" : "median"; unit: "ms"; decimals: 1; size: 30; Layout.alignment: Qt.AlignBottom }
            Figure { value: backend.frameP99; label: "99th"; unit: "ms"; decimals: 1; size: 30; Layout.alignment: Qt.AlignBottom }
            Item { Layout.fillWidth: true }
        }
        Sparkline {
            Layout.fillWidth: true
            implicitHeight: 92
            opacity: page.live ? 1 : 0.3
            shown: backend.shownHistory
            base: backend.baseHistory
        }
    }
}
