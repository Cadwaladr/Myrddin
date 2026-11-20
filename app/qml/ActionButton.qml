import QtQuick
import QtQuick.Layouts

// A button. kind: "primary" (filled with the accent), "plain" (outlined), "quiet" (text only).
Item {
    id: root
    property string text: ""
    property string icon: ""
    property string kind: "plain"
    property bool busy: false
    property bool large: false
    signal clicked()
    implicitWidth: row.implicitWidth + (large ? 56 : 30)
    implicitHeight: large ? 54 : 36
    opacity: enabled ? 1 : 0.45
    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()
    scale: tap.pressed ? 0.97 : 1
    Behavior on scale { NumberAnimation { duration: Theme.quick; easing.type: Easing.OutCubic } }
    Behavior on opacity { NumberAnimation { duration: Theme.smooth } }

    Rectangle {
        anchors.fill: parent
        radius: root.large ? height / 2 : Theme.radiusSmall
        visible: root.kind === "primary"
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Theme.accent }
            GradientStop { position: 1; color: Theme.accent2 }
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: root.large ? height / 2 : Theme.radiusSmall
        color: root.kind === "primary" ? Theme.onAccent : Theme.cardHigh
        opacity: root.kind === "primary" ? (hover.hovered ? 0.14 : 0) : (root.kind === "quiet" ? (hover.hovered ? 1 : 0) : (hover.hovered ? 1 : 0.55))
        border.width: root.kind === "plain" ? 1 : 0
        border.color: root.activeFocus ? Theme.accent2 : Theme.line
        Behavior on opacity { NumberAnimation { duration: Theme.quick } }
    }
    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 8
        Icon {
            visible: root.icon !== "" && !root.busy
            path: root.icon
            size: root.large ? 20 : 16
            filled: root.icon === Theme.iconPlay
            color: root.kind === "primary" ? Theme.onAccent : Theme.text
        }
        Icon {
            visible: root.busy
            path: Theme.iconRefresh
            size: root.large ? 20 : 16
            color: root.kind === "primary" ? Theme.onAccent : Theme.text
            RotationAnimator on rotation { running: root.busy; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
        }
        Text {
            text: root.text
            visible: text !== ""
            color: root.kind === "primary" ? Theme.onAccent : Theme.text
            font.pixelSize: root.large ? 17 : 13
            font.weight: root.kind === "primary" ? Font.Bold : Font.Medium
        }
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: if (!root.busy) root.clicked() }
}
