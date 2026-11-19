import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One page: a title, then cards in a column that scrolls. When the page comes up its cards
// come in one after the other.
Flickable {
    id: page
    property bool active: false
    property string title: ""
    property string subtitle: ""
    default property alias content: column.data
    property alias headerExtra: extra.data

    anchors.fill: parent
    contentWidth: width
    contentHeight: column.implicitHeight + 72
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    visible: opacity > 0.01
    opacity: active ? 1 : 0
    enabled: active
    Behavior on opacity { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }
    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
        contentItem: Rectangle { implicitWidth: 5; radius: 3; color: Theme.line; opacity: parent.pressed ? 1 : 0.7 }
    }

    function replay() {
        var n = 0
        for (var i = 0; i < column.children.length; i++) {
            var item = column.children[i]
            if (item.reveal !== undefined) {
                item.reveal = 0
                n++
            }
        }
        stagger.index = 0
        stagger.restart()
    }
    onActiveChanged: if (active) replay()
    Component.onCompleted: if (active) replay()

    Timer {
        id: stagger
        property int index: 0
        interval: 55
        repeat: true
        triggeredOnStart: true
        onTriggered: {
            while (index < column.children.length && column.children[index].reveal === undefined) index++
            if (index >= column.children.length) { stop(); return }
            column.children[index].reveal = 1
            index++
        }
    }

    ColumnLayout {
        id: column
        x: Math.max(36, (page.width - width) / 2)
        y: 30
        width: Math.min(page.width - 72, 900)
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            Layout.bottomMargin: 6
            spacing: 12
            ColumnLayout {
                spacing: 4
                Layout.fillWidth: true
                Text {
                    text: page.title
                    color: Theme.text
                    font.pixelSize: 26
                    font.weight: Font.DemiBold
                    font.letterSpacing: -0.4
                }
                Text {
                    visible: page.subtitle !== ""
                    text: page.subtitle
                    color: Theme.dim
                    font.pixelSize: 14
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
            Item { id: extra; implicitWidth: childrenRect.width; implicitHeight: childrenRect.height; Layout.alignment: Qt.AlignVCenter }
        }
    }
}
