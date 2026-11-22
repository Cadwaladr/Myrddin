import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

// The first start: three short steps over the window.
Rectangle {
    id: welcome
    property bool shown: false
    property int step: 0
    signal finished()
    anchors.fill: parent
    color: Theme.scrim
    visible: opacity > 0.01
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }
    MouseArea { anchors.fill: parent; hoverEnabled: true; onWheel: (wheel) => wheel.accepted = true }

    onShownChanged: if (shown) { step = 0; draw.restart() }
    FolderDialog {
        id: folders
        title: "The folder Star Citizen is installed in"
        onAccepted: backend.conf.prefix = backend.fullPath(selectedFolder.toString())
    }

    Rectangle {
        id: sheet
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 600)
        height: steps.children[welcome.step].implicitHeight + 128
        radius: 22
        color: Theme.card
        border.width: 1
        border.color: Theme.line
        scale: welcome.shown ? 1 : 0.96
        Behavior on scale { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutBack } }
        Behavior on height { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }
        clip: true

        Item {
            id: steps
            x: 40
            y: 40
            width: parent.width - 80

            // 1: what this is
            ColumnLayout {
                width: parent.width
                spacing: 16
                opacity: welcome.step === 0 ? 1 : 0
                visible: opacity > 0.01
                Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
                Mark {
                    id: logo
                    size: 72
                    NumberAnimation { id: draw; target: logo; property: "drawn"; from: 0; to: 1; duration: 1100; easing.type: Easing.InOutCubic }
                }
                Text { text: "Welcome to Myrddin"; color: Theme.text; font.pixelSize: 26; font.weight: Font.DemiBold; font.letterSpacing: -0.4 }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: 1.35
                    color: Theme.dim
                    font.pixelSize: 14
                    text: "Myrddin starts Star Citizen and adds generated frames between the game's own. It needs an NVIDIA graphics card, DLSS chosen as the upscaler in the game's settings, and one of the LUG's experimental runners."
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: 1.35
                    color: Theme.faint
                    font.pixelSize: 13
                    text: "It does not change the game's files or memory. The game is protected by Easy Anti-Cheat, Myrddin is not affiliated with Cloud Imperium Games, and you use it at your own risk."
                }
            }

            // 2: where the game is
            ColumnLayout {
                width: parent.width
                spacing: 16
                opacity: welcome.step === 1 ? 1 : 0
                visible: opacity > 0.01
                Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
                Text { text: "Where is the game?"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold; font.letterSpacing: -0.3 }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    lineHeight: 1.35
                    color: Theme.dim
                    font.pixelSize: 14
                    text: found ? "Found it where the LUG Helper installed it. Change the folder if this is not the one you play." : "The folder Star Citizen is installed in, the one that holds sc-launch.sh, was not found in the usual places."
                    readonly property bool found: backend.conf.prefix !== "" && backend.looksLikePrefix(backend.conf.prefix)
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Field {
                        id: where
                        Layout.fillWidth: true
                        placeholderText: "~/Games/star-citizen"
                        text: backend.tidyPath(backend.conf.prefix)
                        valid: text === "" || backend.looksLikePrefix(text)
                        onEditingFinished: {
                    backend.conf.prefix = backend.fullPath(text)
                    text = Qt.binding(() => backend.tidyPath(backend.conf.prefix))
                }
                    }
                    ActionButton { text: "Browse"; icon: Theme.iconFolder; onClicked: folders.open() }
                }
                RowLayout {
                    spacing: 8
                    visible: where.text !== ""
                    Icon { path: where.valid ? Theme.iconCheck : Theme.iconCross; color: where.valid ? Theme.good : Theme.bad; size: 16; stroke: 2.2 }
                    Text { text: where.valid ? "This looks like the game's folder." : "This folder has neither sc-launch.sh nor a drive_c in it."; color: where.valid ? Theme.good : Theme.bad; font.pixelSize: 13 }
                }
            }

            // 3: what the system has
            ColumnLayout {
                width: parent.width
                spacing: 14
                opacity: welcome.step === 2 ? 1 : 0
                visible: opacity > 0.01
                Behavior on opacity { NumberAnimation { duration: Theme.smooth } }
                Text { text: "Your system"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold; font.letterSpacing: -0.3 }
                Repeater {
                    model: backend.checks
                    CheckRow {
                        required property var modelData
                        item: modelData
                        visible: modelData.level !== "info"
                        onFix: (key) => backend.applyFix(key)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.faint
                    font.pixelSize: 13
                    text: "Setup shows this list again at any time."
                }
            }
        }

        RowLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 28
            spacing: 10
            Row {
                spacing: 7
                Layout.leftMargin: 12
                Repeater {
                    model: 3
                    Rectangle {
                        required property int index
                        width: index === welcome.step ? 22 : 7
                        height: 7
                        radius: 3.5
                        color: index === welcome.step ? Theme.accent : Theme.line
                        Behavior on width { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }
                        Behavior on color { ColorAnimation { duration: Theme.smooth } }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            ActionButton { text: "Back"; kind: "quiet"; visible: welcome.step > 0; onClicked: welcome.step-- }
            ActionButton {
                kind: "primary"
                text: welcome.step === 2 ? "Done" : "Continue"
                icon: welcome.step === 2 ? Theme.iconCheck : Theme.iconArrow
                onClicked: {
                    if (welcome.step < 2) { welcome.step++; if (welcome.step === 2) backend.refreshChecks() }
                    else welcome.finished()
                }
            }
        }
    }
}
