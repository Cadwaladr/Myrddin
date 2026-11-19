import QtQuick
import QtQuick.Layouts

// A setting: what it is on the left, the control on the right.
RowLayout {
    id: row
    property string title: ""
    property string detail: ""
    property bool dimmed: false
    default property alias control: holder.data
    Layout.fillWidth: true
    spacing: 18
    opacity: dimmed ? 0.45 : 1
    Behavior on opacity { NumberAnimation { duration: Theme.smooth } }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        Text {
            text: row.title
            color: Theme.text
            font.pixelSize: 14
            font.weight: Font.Medium
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        Text {
            visible: row.detail !== ""
            text: row.detail
            color: Theme.dim
            font.pixelSize: 13
            lineHeight: 1.25
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
    Item {
        id: holder
        implicitWidth: childrenRect.width
        implicitHeight: childrenRect.height
        Layout.alignment: Qt.AlignVCenter
    }
}
