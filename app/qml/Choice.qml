import QtQuick
import QtQuick.Controls

// A drop-down list. model: [{ text, value }]; "value" is what is chosen.
ComboBox {
    id: box
    property var value
    signal picked(var value)
    implicitWidth: 240
    implicitHeight: 38
    textRole: "text"
    font.pixelSize: 13
    currentIndex: {
        for (var i = 0; i < model.length; i++) if (model[i].value === value) return i
        return 0
    }
    onActivated: (index) => picked(model[index].value)

    contentItem: Text {
        leftPadding: 12
        rightPadding: 30
        text: box.displayText
        color: Theme.text
        font: box.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Icon {
        x: box.width - width - 10
        y: (box.height - height) / 2
        path: "M7 10l5 5 5-5"
        size: 16
        color: Theme.dim
        rotation: box.popup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutCubic } }
    }
    background: Rectangle {
        radius: Theme.radiusSmall
        color: Theme.field
        border.width: 1
        border.color: box.activeFocus || box.popup.visible ? Theme.accent : Theme.line
        Behavior on border.color { ColorAnimation { duration: Theme.quick } }
    }
    delegate: ItemDelegate {
        id: entry
        required property var modelData
        required property int index
        width: box.width - 8
        height: 34
        contentItem: Text {
            text: entry.modelData.text
            color: entry.index === box.currentIndex ? Theme.accent : Theme.text
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            leftPadding: 4
        }
        background: Rectangle {
            radius: 7
            color: entry.hovered || entry.highlighted ? Theme.cardHigh : "transparent"
        }
    }
    popup: Popup {
        y: box.height + 6
        width: box.width
        padding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 300)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: box.popup.visible ? box.delegateModel : null
            currentIndex: box.highlightedIndex
        }
        background: Rectangle {
            radius: Theme.radiusSmall + 2
            color: Theme.card
            border.width: 1
            border.color: Theme.line
        }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.quick }
            NumberAnimation { property: "y"; from: box.height - 2; to: box.height + 6; duration: Theme.smooth; easing.type: Easing.OutCubic }
        }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.quick } }
    }
}
