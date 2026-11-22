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

    property string page: ["play", "console", "setup", "options", "about"].indexOf(startPage) >= 0 ? startPage : "play"
    readonly property var pages: [
        { key: "play", label: "Play", icon: Theme.iconPlay },
        { key: "console", label: "Console", icon: Theme.iconTerminal },
        { key: "setup", label: "Setup", icon: Theme.iconSetup },
        { key: "options", label: "Options", icon: Theme.iconOptions },
        { key: "about", label: "About", icon: Theme.iconAbout }
    ]
    function indexOf(key) { for (var i = 0; i < pages.length; i++) if (pages[i].key === key) return i; return 0 }

    Shortcut { sequence: "Ctrl+1"; onActivated: window.page = "play" }
    Shortcut { sequence: "Ctrl+2"; onActivated: window.page = "setup" }
    Shortcut { sequence: "Ctrl+3"; onActivated: window.page = "options" }
    Shortcut { sequence: "Ctrl+4"; onActivated: window.page = "about" }
    Shortcut { sequences: [StandardKey.Quit]; onActivated: Qt.quit() }

    // a little light behind everything: still, so the window costs nothing while it is not touched
    Shape {
        anchors.fill: parent
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: window.width * 0.78; centerY: -40; focalX: centerX; focalY: centerY
                centerRadius: window.width * 0.62
                GradientStop { position: 0; color: Theme.faded(Theme.accent2, 0.11) }
                GradientStop { position: 1; color: Theme.faded(Theme.accent2, 0.0) }
            }
            PathRectangle { width: window.width; height: window.height }
        }
        ShapePath {
            strokeColor: "transparent"
            fillGradient: RadialGradient {
                centerX: 120; centerY: window.height + 60; focalX: centerX; focalY: centerY
                centerRadius: window.width * 0.5
                GradientStop { position: 0; color: Theme.faded(Theme.accent, 0.08) }
                GradientStop { position: 1; color: Theme.faded(Theme.accent, 0.0) }
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
        ConsolePage { active: window.page === "console"; onGo: (name) => window.page = name }
        SetupPage { active: window.page === "setup"; onGo: (name) => window.page = name }
        OptionsPage { active: window.page === "options"; onGo: (name) => window.page = name }
        AboutPage { active: window.page === "about"; onGo: (name) => window.page = name }
    }

    // the log of a session that ran with logs, once the game has closed: read, copy, delete or keep
    Rectangle {
        id: logOffer
        // (out of the way while the log is being read, where the same choices are)
        readonly property bool up: backend.sessionLog !== "" && !backend.toolTitle.startsWith("Session log")
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.leftMargin: 36
        anchors.rightMargin: 36
        y: parent.height - (up ? height + 24 : 0)
        height: 68
        radius: Theme.radius
        visible: opacity > 0.01
        opacity: up ? 1 : 0
        color: Theme.cardHigh
        border.width: 1
        border.color: Theme.accent
        Behavior on y { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 12
            spacing: 10
            Icon { path: Theme.iconLog; color: Theme.accent; size: 22 }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { text: "The log of your last session is ready"; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }
                Text { text: backend.sessionLogNote; color: Theme.dim; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true }
            }
            ActionButton { text: "Read"; onClicked: backend.readSessionLog() }
            ActionButton { text: "Copy"; icon: Theme.iconCopy; onClicked: backend.copySessionLog() }
            ActionButton { text: "Delete"; icon: Theme.iconTrash; onClicked: backend.deleteSessionLog() }
            ActionButton { kind: "quiet"; text: "Keep"; onClicked: backend.keepSessionLog() }
        }
    }

    // a report, the last launch or a session's log, over the page that asked for it
    Reader {
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }

    // a line that comes up and goes
    Rectangle {
        id: toast
        property string words: ""
        property bool up: false
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.horizontalCenterOffset: rail.width / 2
        y: parent.height - (up ? 62 : 20) - (logOffer.up ? logOffer.height + 14 : 0)
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

    // Known problems, at the start: settings of this desktop that are known to get in the way, each with its fix
    // (Options, "Known problems", has the same and the undo)
    Rectangle {
        id: problemsNote
        anchors.fill: parent
        color: Theme.scrim
        visible: backend.problemsOffer && !welcome.shown && backend.bugReport === ""
        MouseArea { anchors.fill: parent }   // (the page behind takes no clicks while it is up)
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(640, parent.width - 80)
            height: problemsColumn.implicitHeight + 44
            radius: Theme.radius
            color: Theme.card
            border.width: 1
            border.color: Theme.accent
            ColumnLayout {
                id: problemsColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 22
                spacing: 14
                Text { text: "Before you play"; color: Theme.text; font.pixelSize: 18; font.weight: Font.DemiBold }
                Text {
                    text: "Desktop settings known to cause trouble in the game. Each can be fixed here, and Options, under \"Known problems\", can undo every fix."
                    color: Theme.dim
                    font.pixelSize: 13
                    lineHeight: 1.25
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Repeater {
                    model: backend.problems.filter((p) => p.present)
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Text { text: modelData.title; color: Theme.text; font.pixelSize: 14; font.weight: Font.Medium; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            Text { text: modelData.detail; color: Theme.dim; font.pixelSize: 13; lineHeight: 1.2; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        ActionButton { text: "Fix"; onClicked: backend.fixProblem(modelData.id) }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    ActionButton { kind: "quiet"; text: "Leave them as they are"; onClicked: backend.leaveProblems() }
                }
            }
        }
    }

    // "Report a bug": the report is made and copied; where it goes
    Rectangle {
        id: reportNote
        anchors.fill: parent
        color: Theme.scrim
        // (out of the way while the report is being read)
        visible: backend.bugReport !== "" && backend.toolTitle !== "Bug report"
        MouseArea { anchors.fill: parent }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(600, parent.width - 80)
            height: reportColumn.implicitHeight + 44
            radius: Theme.radius
            color: Theme.card
            border.width: 1
            border.color: Theme.accent
            ColumnLayout {
                id: reportColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 22
                spacing: 12
                Text { text: "Report a bug"; color: Theme.text; font.pixelSize: 18; font.weight: Font.DemiBold }
                Text {
                    text: "The report is copied. It says what went wrong in your last session, with your settings and system versions; your folder and computer names are left out.\n\n1. Open a new issue on GitHub (you need a GitHub account).\n2. Paste the report into it.\n3. Say what you were doing and what you saw. A screenshot helps."
                    color: Theme.dim
                    font.pixelSize: 13
                    lineHeight: 1.25
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Text { text: backend.tidyPath(backend.bugReport); color: Theme.faint; font.pixelSize: 12; elide: Text.ElideMiddle; Layout.fillWidth: true }
                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    ActionButton { text: "Open GitHub"; icon: Theme.iconLink; onClicked: backend.openLink(backend.issuesPage) }
                    ActionButton { text: "Read it"; icon: Theme.iconLog; onClicked: backend.readBugReport() }
                    ActionButton { text: "Show the folder"; icon: Theme.iconFolder; onClicked: backend.openFolder("reports") }
                    ActionButton { kind: "quiet"; text: "Close"; onClicked: backend.closeBugReport() }
                }
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
