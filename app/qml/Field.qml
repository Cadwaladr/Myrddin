import QtQuick
import QtQuick.Controls

// A line of text to type in.
TextField {
    id: field
    property bool valid: true
    implicitHeight: 38
    color: Theme.text
    placeholderTextColor: Theme.faint
    selectionColor: Theme.accent2
    selectedTextColor: Theme.onAccent
    font.pixelSize: 13
    leftPadding: 12
    rightPadding: 12
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true
    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.field
        border.width: 1
        border.color: !field.valid ? Theme.bad : field.activeFocus ? Theme.accent : Theme.line
        Behavior on border.color { ColorAnimation { duration: Theme.quick } }
    }
}
