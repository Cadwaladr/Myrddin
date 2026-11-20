import QtQuick

// A switch.
Item {
    id: root
    property bool checked: false
    signal toggled(bool value)
    implicitWidth: 44
    implicitHeight: 24
    opacity: enabled ? 1 : 0.4
    activeFocusOnTab: true
    Keys.onSpacePressed: root.toggled(!root.checked)
    Keys.onReturnPressed: root.toggled(!root.checked)

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.checked ? Theme.accent : Theme.line
        border.width: root.activeFocus ? 2 : 0
        border.color: Theme.accent2
        Behavior on color { ColorAnimation { duration: Theme.smooth } }
    }
    Rectangle {
        width: 18
        height: 18
        radius: 9
        y: 3
        x: root.checked ? parent.width - width - 3 : 3
        color: root.checked ? Theme.onAccent : Theme.dim
        scale: area.pressed ? 0.88 : 1
        Behavior on x { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutBack; easing.overshoot: 1.4 } }
        Behavior on color { ColorAnimation { duration: Theme.smooth } }
        Behavior on scale { NumberAnimation { duration: Theme.quick } }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        anchors.margins: -6
        cursorShape: Qt.PointingHandCursor
        onClicked: root.toggled(!root.checked)
    }
}
