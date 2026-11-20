import QtQuick

// A key to press in the game, shown and chosen: a click, then the key itself (with Ctrl, Shift or
// Alt held, if wanted) is the new one. Esc leaves it as it was. Which keys can be used is the
// backend's to say (the layer's own list).
Rectangle {
    id: root
    property string value: ""
    property bool listening: false
    property string refused: ""
    signal picked(string keys)

    implicitWidth: Math.max(132, Math.ceil(label.implicitWidth) + 30)
    implicitHeight: 36
    radius: Theme.radiusSmall
    color: listening ? Theme.mix(Theme.card, Theme.accent, 0.16) : Theme.mix(Theme.card, Theme.cardHigh, hover.hovered ? 1 : 0.55)
    border.width: 1
    border.color: listening || activeFocus ? Theme.accent2 : Theme.line
    opacity: enabled ? 1 : 0.45
    activeFocusOnTab: true
    Behavior on color { ColorAnimation { duration: Theme.quick } }

    Text {
        id: label
        anchors.centerIn: parent
        text: root.listening ? (root.refused !== "" ? root.refused : "Press a key") : root.value
        color: root.listening ? Theme.dim : Theme.text
        font.pixelSize: 14
        font.weight: root.listening ? Font.Normal : Font.Medium
    }
    Keys.onPressed: (event) => {
        if (!root.listening) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                root.refused = ""
                root.listening = true
                event.accepted = true
            }
            return
        }
        event.accepted = true
        if (event.key === Qt.Key_Escape) { root.listening = false; return }
        if ([Qt.Key_Shift, Qt.Key_Control, Qt.Key_Alt, Qt.Key_AltGr, Qt.Key_Meta, Qt.Key_Super_L, Qt.Key_Super_R].indexOf(event.key) >= 0) return
        var keys = backend.keyName(event.nativeScanCode, event.modifiers)
        if (keys === "") { root.refused = "Not that key"; return }
        root.listening = false
        root.picked(keys)
    }
    onActiveFocusChanged: if (!activeFocus) listening = false
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler {
        onTapped: {
            root.forceActiveFocus()
            root.refused = ""
            root.listening = !root.listening
        }
    }
}
