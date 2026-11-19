import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes

ApplicationWindow {
    id: window
    width: 1080
    height: 720
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    title: "Myrddin"
    color: Theme.bg

    property string page: ["play", "setup", "options", "session", "about"].indexOf(startPage) >= 0 ? startPage : "play"
    readonly property var pages: [
        { key: "play", label: "Play", icon: Theme.iconPlay },
        { key: "setup", label: "Setup", icon: Theme.iconSetup },
        { key: "options", label: "Options", icon: Theme.iconOptions },
        { key: "session", label: "Session", icon: Theme.iconSession },
        { key: "about", label: "About", icon: Theme.iconAbout }
    ]
    function indexOf(key) { for (var i = 0; i < pages.length; i++) if (pages[i].key === key) return i; return 0 }

    Shortcut { sequence: "Ctrl+1"; onActivated: window.page = "play" }
    Shortcut { sequence: "Ctrl+2"; onActivated: window.page = "setup" }
    Shortcut { sequence: "Ctrl+3"; onActivated: window.page = "options" }
    Shortcut { sequence: "Ctrl+4"; onActivated: window.page = "session" }
    Shortcut { sequence: "Ctrl+5"; onActivated: window.page = "about" }
    Shortcut { sequences: [StandardKey.Quit]; onActivated: Qt.quit() }

    // a little light behind everything: still, so the window costs nothing while it is not touched
    Shape {
        anchors.fill: parent
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: window.width * 0.78; centerY: -40; focalX: centerX; focalY: centerY
                centerRadius: window.width * 0.62
                GradientStop { position: 0; color: Qt.rgba(0.35, 0.64, 0.97, 0.13) }
                GradientStop { position: 1; color: Qt.rgba(0.35, 0.64, 0.97, 0.0) }
            }
            PathRectangle { width: window.width; height: window.height }
        }
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: 120; centerY: window.height + 60; focalX: centerX; focalY: centerY
                centerRadius: window.width * 0.5
                GradientStop { position: 0; color: Qt.rgba(0.34, 0.88, 0.78, 0.09) }
                GradientStop { position: 1; color: Qt.rgba(0.34, 0.88, 0.78, 0.0) }
            }
            PathRectangle { width: window.width; height: window.height }
        }
    }

    // the rail
    Rectangle {
        id: rail
        width: 214
        height: parent.height
        color: Qt.rgba(Theme.rail.r, Theme.rail.g, Theme.rail.b, 0.82)
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.lineSoft }

        RowLayout {
            x: 22
            y: 24
            spacing: 12
            Mark { size: 34 }
            Text { text: "Myrddin"; color: Theme.text; font.pixelSize: 19; font.weight: Font.DemiBold; font.letterSpacing: -0.3 }
        }

        Item {
            id: nav
            x: 12
            y: 92
            width: parent.width - 24
            height: window.pages.length * 46

            Rectangle {
                width: parent.width
                height: 40
                radius: 10
                y: window.indexOf(window.page) * 46
                color: Theme.cardHigh
                border.width: 1
                border.color: Theme.line
                Behavior on y { NumberAnimation { duration: 320; easing.type: Easing.OutBack; easing.overshoot: 0.9 } }
                Rectangle {
                    width: 3
                    height: 18
                    radius: 1.5
                    x: 0
                    anchors.verticalCenter: parent.verticalCenter
                    gradient: Gradient {
                        GradientStop { position: 0; color: Theme.accent }
                        GradientStop { position: 1; color: Theme.accent2 }
                    }
                }
            }
            Repeater {
                model: window.pages
                Item {
                    id: entry
                    required property var modelData
                    required property int index
                    readonly property bool current: window.page === modelData.key
                    y: index * 46
                    width: nav.width
                    height: 40
                    RowLayout {
                        x: 16
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 12
                        Icon {
                            path: entry.modelData.icon
                            size: 18
                            color: entry.current ? Theme.accent : (hover.hovered ? Theme.text : Theme.dim)
                            Behavior on color { ColorAnimation { duration: Theme.quick } }
                        }
                        Text {
                            text: entry.modelData.label
                            color: entry.current || hover.hovered ? Theme.text : Theme.dim
                            font.pixelSize: 14
                            font.weight: entry.current ? Font.DemiBold : Font.Medium
                            Behavior on color { ColorAnimation { duration: Theme.quick } }
                        }
                    }
                    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: window.page = entry.modelData.key }
                }
            }
        }

        ColumnLayout {
            x: 22
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20
            width: parent.width - 44
            spacing: 6
            RowLayout {
                spacing: 8
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: backend.state === "running" ? Theme.good : backend.state === "idle" ? Theme.faint : Theme.warn
                    Behavior on color { ColorAnimation { duration: Theme.slow } }
                }
                Text {
                    text: backend.state === "running" ? "Game running" : backend.state === "launcher" ? "Launcher open" : backend.state === "starting" ? "Starting" : "Game not running"
                    color: Theme.dim
                    font.pixelSize: 12
                }
            }
            Text { text: "Version " + backend.version; color: Theme.faint; font.pixelSize: 12 }
        }
    }

    // the pages
    Item {
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        PlayPage { active: window.page === "play"; onGo: (name) => window.page = name }
        SetupPage { active: window.page === "setup"; onGo: (name) => window.page = name }
        OptionsPage { active: window.page === "options"; onGo: (name) => window.page = name }
        SessionPage { active: window.page === "session"; onGo: (name) => window.page = name }
        AboutPage { active: window.page === "about"; onGo: (name) => window.page = name }
    }

    // a line that comes up and goes
    Rectangle {
        id: toast
        property string words: ""
        property bool up: false
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: rail.width / 2
        y: parent.height - (up ? 62 : 20)
        opacity: up ? 1 : 0
        width: Math.min(toastText.implicitWidth + 36, parent.width - rail.width - 80)
        height: 38
        radius: 19
        color: Theme.cardHigh
        border.width: 1
        border.color: Theme.line
        Behavior on y { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
        Text {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - 30)
            text: toast.words
            color: Theme.text
            font.pixelSize: 13
            elide: Text.ElideRight
        }
        Timer { id: toastTimer; interval: 3200; onTriggered: toast.up = false }
        Connections {
            target: backend
            function onSaid(text) {
                if (text === "") return
                toast.words = text
                toast.up = true
                toastTimer.restart()
            }
        }
    }

    Welcome {
        id: welcome
        shown: startPage === "welcome" || (startPage === "" && !backend.prefs.welcomed)
        onFinished: {
            backend.prefs.welcomed = true
            shown = false
        }
    }
}
