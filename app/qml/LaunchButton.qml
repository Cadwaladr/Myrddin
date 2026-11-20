import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes

// The launch button with a switch hanging from it ("Run with logs"): a tab half as high and seven
// tenths as wide as the button, flush with its lower edge. Where a smaller copy of the button would
// meet that edge with two inward corners, the tab's outline leaves the button's in a curve the other
// way round, so the two read as one piece: with the pointer over the button, both light up.
Item {
    id: root
    property string text: ""
    property string icon: ""
    property bool primary: true       // filled with the accent; otherwise outlined
    property bool active: true        // the button can be pressed
    property bool busy: false
    property bool checked: false      // the tab's switch
    property string tabText: "Run with logs"
    property bool danger: false       // outlined in the warning colour ("Kill")
    property string reserve: ""       // the longest text it will show: its width does not change with the text
    readonly property bool pointed: faceHover.hovered || faceArea.activeFocus   // the pointer or the keyboard is on the button
    signal clicked()
    signal toggled(bool value)

    readonly property real bw: width
    readonly property real bh: 54
    readonly property real tw: Math.round(bw * 0.7)
    readonly property real th: Math.round(bh * 0.5)
    implicitWidth: Math.max(190, Math.ceil(Math.max(row.implicitWidth, measure.implicitWidth + 28)) + 56)
    Text { id: measure; visible: false; text: root.reserve; font.pixelSize: 17; font.weight: Font.Bold }
    implicitHeight: bh + th

    // The tab's outline, along the middle of a line one pixel wide. Its lower corners are a pill's,
    // like the button's. At the top each side ends in a circle that touches the side and the
    // button's outline: the button's lower edge where that is straight, its round end where the tab
    // reaches under it (f: that circle's centre, t: where it leaves the button's outline).
    readonly property real rb: bh / 2 - 0.5
    readonly property real xl: Math.round((bw - tw) / 2) + 0.5
    readonly property real xr: bw - xl
    readonly property real yb: bh + th - 0.5
    readonly property real r2: th / 2
    readonly property real r1: th / 2 - 1
    readonly property real fx: xl - r1
    readonly property bool underEnd: fx < bh / 2
    readonly property real fy: underEnd ? bh / 2 + Math.sqrt(Math.max(0, (rb + r1) * (rb + r1) - (fx - bh / 2) * (fx - bh / 2))) : bh - 0.5 + r1
    readonly property real tx: underEnd ? bh / 2 + rb * (fx - bh / 2) / (rb + r1) : fx
    readonly property real ty: underEnd ? bh / 2 + rb * (fy - bh / 2) / (rb + r1) : bh - 0.5

    // Colours: all of them opaque blends, so that the two shapes can lie over one another.
    readonly property real fade: active ? 0 : 0.6
    readonly property color faceA: Theme.mix(Theme.accent, Theme.card, fade)
    readonly property color faceB: Theme.mix(Theme.accent2, Theme.card, fade)
    // (the pointer over the button: the button and its tab light up alike, by the same amount and
    // in the same time; the pointer over the tab lights up the tab alone, which is a switch of its own)
    readonly property bool lit: faceHover.hovered && active
    readonly property real glow: lit ? 0.14 : 0
    readonly property color plainBase: Theme.mix(Theme.card, Theme.cardHigh, lit ? 1 : 0.55)
    readonly property color plain: danger ? Theme.mix(plainBase, Theme.bad, lit ? 0.24 : 0.1) : plainBase
    readonly property color frame: danger ? Theme.mix(Theme.line, Theme.bad, 0.75) : Theme.line
    readonly property color tabPlain: Theme.mix(plain, Theme.accent, checked ? 0.2 : 0)
    readonly property real lift: tabHover.hovered ? 0.07 : 0
    // (the switch on: the tab is of the button's colour, part of the launch; off: it stands back)
    property color tabA: Theme.mix(primary ? Theme.mix(Theme.mix(faceA, Theme.card, checked ? 0 : 0.62), Theme.onAccent, glow) : tabPlain, Theme.text, lift)
    property color tabB: Theme.mix(primary ? Theme.mix(Theme.mix(faceB, Theme.card, checked ? 0 : 0.62), Theme.onAccent, glow) : tabPlain, Theme.text, lift)
    readonly property color tabInk: primary && checked ? Theme.mix(Theme.onAccent, faceA, fade * 0.5) : (checked ? Theme.text : Theme.dim)
    Behavior on tabA { ColorAnimation { duration: Theme.quick } }
    Behavior on tabB { ColorAnimation { duration: Theme.quick } }

    scale: faceTap.pressed && active ? 0.97 : 1
    Behavior on scale { NumberAnimation { duration: Theme.quick; easing.type: Easing.OutCubic } }

    // the tab, under the button: its fill goes on behind the button's lower half
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: root.bw; y2: 0
                GradientStop { position: 0; color: root.tabA }
                GradientStop { position: 1; color: root.tabB }
            }
            startX: root.tx; startY: root.ty
            PathArc { x: root.xl; y: root.fy; radiusX: root.r1; radiusY: root.r1; direction: PathArc.Clockwise }
            PathLine { x: root.xl; y: root.yb - root.r2 }
            PathArc { x: root.xl + root.r2; y: root.yb; radiusX: root.r2; radiusY: root.r2; direction: PathArc.Counterclockwise }
            PathLine { x: root.xr - root.r2; y: root.yb }
            PathArc { x: root.xr; y: root.yb - root.r2; radiusX: root.r2; radiusY: root.r2; direction: PathArc.Counterclockwise }
            PathLine { x: root.xr; y: root.fy }
            PathArc { x: root.bw - root.tx; y: root.ty; radiusX: root.r1; radiusY: root.r1; direction: PathArc.Clockwise }
            PathLine { x: root.xr; y: root.bh / 2 }
            PathLine { x: root.xl; y: root.bh / 2 }
            PathLine { x: root.tx; y: root.ty }
        }
        // its frame, where the button has one: out of the button's, round the tab and back into it
        ShapePath {
            strokeColor: root.primary ? "transparent" : (tabArea.activeFocus ? Theme.accent2 : root.frame)
            strokeWidth: 1
            fillColor: "transparent"
            capStyle: ShapePath.FlatCap
            startX: root.tx; startY: root.ty
            PathArc { x: root.xl; y: root.fy; radiusX: root.r1; radiusY: root.r1; direction: PathArc.Clockwise }
            PathLine { x: root.xl; y: root.yb - root.r2 }
            PathArc { x: root.xl + root.r2; y: root.yb; radiusX: root.r2; radiusY: root.r2; direction: PathArc.Counterclockwise }
            PathLine { x: root.xr - root.r2; y: root.yb }
            PathArc { x: root.xr; y: root.yb - root.r2; radiusX: root.r2; radiusY: root.r2; direction: PathArc.Counterclockwise }
            PathLine { x: root.xr; y: root.fy }
            PathArc { x: root.bw - root.tx; y: root.ty; radiusX: root.r1; radiusY: root.r1; direction: PathArc.Clockwise }
        }
    }

    // the button
    Rectangle {
        width: root.bw
        height: root.bh
        radius: height / 2
        visible: root.primary
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: root.faceA }
            GradientStop { position: 1; color: root.faceB }
        }
    }
    Rectangle {
        width: root.bw
        height: root.bh
        radius: height / 2
        color: root.primary ? Theme.faded(Theme.onAccent, root.glow) : root.plain
        border.width: root.primary ? 0 : 1
        border.color: faceArea.activeFocus ? Theme.accent2 : root.frame
        Behavior on color { ColorAnimation { duration: Theme.quick } }
    }
    Item {
        id: faceArea
        width: root.bw
        height: root.bh
        activeFocusOnTab: root.active
        Keys.onReturnPressed: if (root.active && !root.busy) root.clicked()
        Keys.onSpacePressed: if (root.active && !root.busy) root.clicked()
        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: 8
            readonly property color ink: root.primary ? Theme.mix(Theme.onAccent, root.faceA, root.fade * 0.5)
                                       : Theme.mix(root.danger ? Theme.mix(Theme.text, Theme.bad, 0.7) : Theme.text, Theme.card, root.active ? 0 : 0.45)
            Icon {
                visible: root.icon !== "" && !root.busy
                path: root.icon
                size: 20
                filled: root.icon === Theme.iconPlay
                color: row.ink
            }
            Icon {
                visible: root.busy
                path: Theme.iconRefresh
                size: 20
                color: row.ink
                RotationAnimator on rotation { running: root.busy; from: 0; to: 360; duration: 900; loops: Animation.Infinite }
            }
            Text {
                text: root.text
                color: row.ink
                font.pixelSize: 17
                font.weight: root.primary ? Font.Bold : Font.Medium
            }
        }
        HoverHandler { id: faceHover; cursorShape: root.active ? Qt.PointingHandCursor : Qt.ArrowCursor }
        TapHandler { id: faceTap; onTapped: if (root.active && !root.busy) root.clicked() }
    }

    // the tab's switch
    Item {
        id: tabArea
        x: root.xl
        y: root.bh
        width: root.tw
        height: root.th
        activeFocusOnTab: true
        Keys.onSpacePressed: root.toggled(!root.checked)
        Keys.onReturnPressed: root.toggled(!root.checked)
        Row {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -1
            spacing: 6
            scale: tabTap.pressed ? 0.94 : 1
            Behavior on scale { NumberAnimation { duration: Theme.quick } }
            Item {
                width: 13
                height: 13
                anchors.verticalCenter: parent.verticalCenter
                Rectangle {
                    anchors.centerIn: parent
                    width: 10
                    height: 10
                    radius: 5
                    color: "transparent"
                    border.width: 1.5
                    border.color: root.tabInk
                    opacity: root.checked ? 0 : 1
                    Behavior on opacity { NumberAnimation { duration: Theme.quick } }
                }
                Icon {
                    anchors.centerIn: parent
                    path: Theme.iconCheck
                    size: 14
                    stroke: 2.6
                    color: root.tabInk
                    opacity: root.checked ? 1 : 0
                    scale: root.checked ? 1 : 0.5
                    Behavior on opacity { NumberAnimation { duration: Theme.quick } }
                    Behavior on scale { NumberAnimation { duration: Theme.smooth; easing.type: Easing.OutBack } }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.tabText
                color: root.tabInk
                font.pixelSize: 12
                font.weight: Font.Medium
                Behavior on color { ColorAnimation { duration: Theme.smooth } }
            }
        }
        HoverHandler { id: tabHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { id: tabTap; onTapped: root.toggled(!root.checked) }
    }
}
