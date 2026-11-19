import QtQuick
import QtQuick.Shapes

// One of Theme's icons, drawn as a line in the given colour.
Item {
    id: root
    property string path
    property color color: Theme.dim
    property real size: 20
    property real stroke: 1.8
    property bool filled: false
    implicitWidth: size
    implicitHeight: size

    Shape {
        width: 24
        height: 24
        scale: root.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: root.color
            strokeWidth: root.stroke
            fillColor: root.filled ? root.color : "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.path }
        }
    }
}
