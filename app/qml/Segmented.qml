import QtQuick

// A row of choices with a thumb that slides to the chosen one.
Item {
    id: root
    property var options: []          // texts
    property int index: 0
    property real cell: 64
    property bool large: false
    signal chosen(int index)
    implicitWidth: options.length * cell + 8
    implicitHeight: large ? 52 : 36
    opacity: enabled ? 1 : 0.45

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: Theme.field
        border.width: 1
        border.color: Theme.line
    }
    Rectangle {
        id: thumb
        width: root.cell
        height: parent.height - 8
        y: 4
        x: 4 + root.index * root.cell
        radius: height / 2
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Theme.accent }
            GradientStop { position: 1; color: Theme.accent2 }
        }
        Behavior on x { NumberAnimation { duration: 300; easing.type: Easing.OutBack; easing.overshoot: 1.1 } }
    }
    Row {
        x: 4
        height: parent.height
        Repeater {
            model: root.options
            Item {
                id: option
                required property int index
                required property string modelData
                width: root.cell
                height: parent.height
                Text {
                    anchors.centerIn: parent
                    text: option.modelData
                    color: option.index === root.index ? "#06201d" : (hover.hovered ? Theme.text : Theme.dim)
                    font.pixelSize: root.large ? 17 : 13
                    font.weight: option.index === root.index ? Font.Bold : Font.Medium
                    Behavior on color { ColorAnimation { duration: Theme.quick } }
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.chosen(option.index) }
            }
        }
    }
}
