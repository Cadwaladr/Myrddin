pragma Singleton
import QtQuick

// Colours, sizes and timings of the window, and its icons (24 x 24, drawn as lines).
QtObject {
    readonly property color bg: "#0a0d12"
    readonly property color rail: "#0d1117"
    readonly property color card: "#121821"
    readonly property color cardHigh: "#18202b"
    readonly property color field: "#0d1219"
    readonly property color line: "#232c39"
    readonly property color lineSoft: "#1a222d"
    readonly property color text: "#e9edf3"
    readonly property color dim: "#9aa5b4"
    readonly property color faint: "#657080"
    readonly property color accent: "#56e0c8"
    readonly property color accent2: "#5aa2f7"
    readonly property color good: "#4fd98a"
    readonly property color warn: "#f2b84b"
    readonly property color bad: "#f2716b"

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
    readonly property string iconSession: "M3 12.5h3.6l2.7-7 4.2 13.5 2.7-6.5H21"
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
}
