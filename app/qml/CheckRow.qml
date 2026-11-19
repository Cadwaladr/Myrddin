import QtQuick
import QtQuick.Layouts

// One line of the system check: a mark, what was looked at, what was found, and a way to fix it.
RowLayout {
    id: row
    property var item: ({})
    signal fix(string key)
    signal go(string page)
    Layout.fillWidth: true
    spacing: 14

    Rectangle {
        Layout.alignment: Qt.AlignTop
        Layout.topMargin: 1
        width: 26
        height: 26
        radius: 13
        readonly property color tone: Theme.level(row.item.level)
        color: Qt.rgba(tone.r, tone.g, tone.b, 0.14)
        Icon {
            anchors.centerIn: parent
            size: 15
            stroke: 2.2
            color: parent.tone
            path: row.item.level === "ok" ? Theme.iconCheck : row.item.level === "bad" ? Theme.iconCross : row.item.level === "warn" ? Theme.iconWarn : Theme.iconInfo
        }
    }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Text {
            text: row.item.title || ""
            color: Theme.text
            font.pixelSize: 14
            font.weight: Font.Medium
        }
        Text {
            text: row.item.detail || ""
            color: Theme.dim
            font.pixelSize: 13
            lineHeight: 1.25
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
    }
    ActionButton {
        visible: (row.item.fix || "") !== ""
        text: row.item.fix || ""
        Layout.alignment: Qt.AlignVCenter
        onClicked: row.fix(row.item.key)
    }
    ActionButton {
        visible: (row.item.fix || "") === "" && (row.item.page || "") !== "" && row.item.level !== "ok"
        kind: "quiet"
        icon: Theme.iconArrow
        Layout.alignment: Qt.AlignVCenter
        onClicked: row.go(row.item.page)
    }
}
