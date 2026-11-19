// Not part of the app. The deploy step scans this file next to app/qml so that the QML modules
// the app is going to use are carried even before its own files import them (see build-appimage.sh).
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Templates
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Shapes
import QtQuick.Dialogs

Window {
    width: 320; height: 200; visible: true; color: "#0e1116"
    ColumnLayout {
        anchors.fill: parent
        Button { text: "Button" }
        Shape { ShapePath { strokeColor: "#e6e9ef"; startX: 0; startY: 0; PathLine { x: 40; y: 20 } } }
        Rectangle { id: r; width: 40; height: 20; color: "#3a7"; layer.enabled: true }
        MultiEffect { source: r; width: 40; height: 20; blurEnabled: true; blur: 0.5 }
    }
}
