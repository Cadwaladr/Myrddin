import QtQuick
import QtQuick.Shapes

// Myrddin's mark. "drawn" from 0 to 1 draws its line.
Item {
    id: mark
    property real size: 34
    property real drawn: 1
    implicitWidth: size
    implicitHeight: size

    Rectangle {
        anchors.fill: parent
        radius: width * 0.23
        gradient: Gradient {
            GradientStop { position: 0; color: "#1b2330" }
            GradientStop { position: 1; color: "#0c1016" }
        }
        border.width: 1
        border.color: Theme.line
    }
    Shape {
        width: 256
        height: 256
        scale: mark.size / 256
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 22
            strokeColor: Theme.accent
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            strokeStyle: mark.drawn < 1 ? ShapePath.DashLine : ShapePath.SolidLine
            dashPattern: [20, 20]
            dashOffset: 20 * (1 - mark.drawn) + 0.001
            PathSvg { path: "M70 178 V86 L128 144 L186 86 V178" }
        }
    }
    Rectangle {
        width: mark.size * 0.075
        height: width
        radius: width / 2
        x: mark.size * 0.5 - width / 2
        y: mark.size * 0.375 - width / 2
        color: Theme.text
        opacity: mark.drawn
    }
}
