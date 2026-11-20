pragma Singleton
import QtQuick

// Colours, sizes and timings of the window, and its icons (24 x 24, drawn as lines).
// Every colour is the desktop's own or a blend of two of them (systemColors, src/systemcolors.h):
// the window follows the colour scheme the system is set to, and changes with it.
QtObject {
    function mix(a, b, t) { return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1) }
    function faded(c, alpha) { return Qt.rgba(c.r, c.g, c.b, alpha) }

    readonly property bool dark: systemColors.dark
    readonly property color bg: systemColors.window
    readonly property color text: systemColors.windowText
    readonly property color card: systemColors.view
    readonly property color cardHigh: systemColors.button
    readonly property color accent: systemColors.highlight
    readonly property color onAccent: systemColors.highlightText   // words and signs on the accent
    readonly property color good: systemColors.positive
    readonly property color warn: systemColors.neutral
    readonly property color bad: systemColors.negative

    readonly property color rail: mix(bg, card, 0.5)
    readonly property color field: mix(card, bg, 0.55)
    readonly property color line: mix(bg, text, 0.2)
    readonly property color lineSoft: mix(bg, text, 0.11)
    readonly property color dim: mix(text, bg, 0.3)
    readonly property color faint: mix(text, bg, 0.52)
    readonly property color accent2: mix(accent, text, 0.3)
    readonly property color scrim: faded(mix(bg, dark ? "black" : "white", 0.4), 0.92)   // over the window, behind the first-start pages

    readonly property int radius: 14
    readonly property int radiusSmall: 9
    readonly property int quick: 130
    readonly property int smooth: 240
    readonly property int slow: 460
    readonly property string mono: "monospace"

    function level(name) {
        return name === "ok" ? good : name === "warn" ? warn : name === "bad" ? bad : accent2
    }

    readonly property string iconPlay: "M8 5.2v13.6a.8.8 0 0 0 1.2.7l11-6.8a.8.8 0 0 0 0-1.4l-11-6.8a.8.8 0 0 0-1.2.7z"
    readonly property string iconSetup: "M3.5 7.5a2 2 0 0 1 2-2h3.6l2 2.2h7.4a2 2 0 0 1 2 2v7.8a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"
    readonly property string iconOptions: "M4 7h9M17 7h3M4 17h3M11 17h9M15 4.8v4.4M9 14.8v4.4"
    readonly property string iconAbout: "M12 3.5a8.5 8.5 0 1 0 0 17a8.5 8.5 0 0 0 0-17zM12 11v5.5M12 7.6v.4"
    readonly property string iconCheck: "M5 12.8l4.3 4.3L19 7.4"
    readonly property string iconWarn: "M12 4.2l8.6 15H3.4zM12 10v4.2M12 16.9v.3"
    readonly property string iconCross: "M6.5 6.5l11 11M17.5 6.5l-11 11"
    readonly property string iconInfo: "M12 11v5.5M12 7.6v.4"
    readonly property string iconCopy: "M9 9h10v10H9zM5.2 15V5.2H15"
    readonly property string iconRefresh: "M19.5 12a7.5 7.5 0 1 1-2.2-5.3M19.6 4.4v4.8h-4.8"
    readonly property string iconFolder: "M3.5 7.5a2 2 0 0 1 2-2h3.6l2 2.2h7.4a2 2 0 0 1 2 2v7.8a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z"
    readonly property string iconMonitor: "M4 5.5h16v10.5H4zM9 19.5h6M12 16v3.5"
    readonly property string iconBolt: "M13 3.5L5.5 13.2h5.3l-1 7.3 8.2-10h-5.5z"
    readonly property string iconArrow: "M5 12h13M13 6.5l5.5 5.5-5.5 5.5"
    readonly property string iconStop: "M7 7h10v10H7z"
    readonly property string iconTerminal: "M4 5.5h16v13H4zM7.5 9.5l3 2.5-3 2.5M12.5 14.5H16"
    readonly property string iconSearch: "M10.5 4.5a6 6 0 1 0 0 12a6 6 0 0 0 0-12zM15 15l4.5 4.5"
    readonly property string iconLog: "M6 3.5h8.5L18.5 7.5V20.5H6zM14 3.5V8h4.5M9 12h6.5M9 15.5h6.5"
    readonly property string iconLink: "M13.5 4.5h6v6M19.5 4.5l-8.5 8.5M18 14v4a1.5 1.5 0 0 1-1.5 1.5h-10A1.5 1.5 0 0 1 5 18V8a1.5 1.5 0 0 1 1.5-1.5h4"
    readonly property string iconTrash: "M5 7h14M9.5 7V4.5h5V7M7 7l.8 12.5h8.4L17 7M10 10.5v6M14 10.5v6"
}
