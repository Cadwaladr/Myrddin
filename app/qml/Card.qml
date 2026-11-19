import QtQuick
import QtQuick.Layouts

// A panel of the page. "reveal" is driven by the page: cards come in one after the other.
Rectangle {
    id: card
    default property alias content: column.data
    property int padding: 22
    property alias spacing: column.spacing
    property real reveal: 1
    property string heading: ""
    property string note: ""

    Layout.fillWidth: true
    implicitHeight: column.implicitHeight + 2 * padding
    color: Theme.card
    radius: Theme.radius
    border.width: 1
    border.color: Theme.line
    opacity: reveal
    transform: Translate { y: (1 - card.reveal) * 14 }
    Behavior on reveal { NumberAnimation { duration: Theme.slow; easing.type: Easing.OutCubic } }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: card.padding
        spacing: 14

        ColumnLayout {
            visible: card.heading !== ""
            spacing: 3
            Layout.fillWidth: true
            Layout.bottomMargin: 2
            Text {
                text: card.heading
                color: Theme.text
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Text {
                visible: card.note !== ""
                text: card.note
                color: Theme.dim
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                lineHeight: 1.25
                Layout.fillWidth: true
            }
        }
    }
}
