import QtQuick
import QtQuick.Layouts

// A number that counts to its new value, with what it is below it.
ColumnLayout {
    id: root
    property real value: 0
    property string label: ""
    property string unit: ""
    property int decimals: 0
    property color tone: Theme.text
    property int size: 44
    property real displayed: value
    spacing: 0
    Behavior on displayed { NumberAnimation { duration: 700; easing.type: Easing.OutCubic } }

    RowLayout {
        spacing: 5
        Text {
            text: root.displayed.toFixed(root.decimals)
            color: root.tone
            font.pixelSize: root.size
            font.weight: Font.DemiBold
            font.letterSpacing: -1
            font.features: { "tnum": 1 }
        }
        Text {
            visible: root.unit !== ""
            text: root.unit
            color: Theme.dim
            font.pixelSize: Math.round(root.size * 0.34)
            font.weight: Font.Medium
            Layout.alignment: Qt.AlignBottom
            Layout.bottomMargin: Math.round(root.size * 0.17)
        }
    }
    Text {
        text: root.label
        color: Theme.dim
        font.pixelSize: 12
        font.weight: Font.Medium
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
    }
}
