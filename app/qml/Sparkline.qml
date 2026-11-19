import QtQuick
import QtQuick.Shapes

// Two lines over time: the game's own frame rate (dim) and the shown one (bright).
Item {
    id: root
    property var shown: []
    property var base: []
    property real ceiling: {
        var m = 30
        for (var i = 0; i < shown.length; i++) m = Math.max(m, shown[i])
        return Math.ceil(m * 1.12 / 10) * 10
    }

    function points(values) {
        var out = []
        var n = values.length
        if (n < 2) return [Qt.point(0, height), Qt.point(width, height)]
        for (var i = 0; i < n; i++)
            out.push(Qt.point(width * i / (n - 1), height - 2 - (height - 6) * Math.min(values[i] / ceiling, 1)))
        return out
    }
    function closed(values) {
        var p = points(values)
        p.push(Qt.point(width, height))
        p.push(Qt.point(0, height))
        return p
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: 0; y2: root.height
                GradientStop { position: 0; color: Qt.rgba(0.34, 0.88, 0.78, 0.22) }
                GradientStop { position: 1; color: Qt.rgba(0.34, 0.88, 0.78, 0.0) }
            }
            PathPolyline { path: root.closed(root.shown) }
        }
        ShapePath {
            strokeColor: Theme.faint
            strokeWidth: 1.5
            fillColor: "transparent"
            joinStyle: ShapePath.RoundJoin
            PathPolyline { path: root.points(root.base) }
        }
        ShapePath {
            strokeColor: Theme.accent
            strokeWidth: 2
            fillColor: "transparent"
            joinStyle: ShapePath.RoundJoin
            capStyle: ShapePath.RoundCap
            PathPolyline { path: root.points(root.shown) }
        }
    }
}
