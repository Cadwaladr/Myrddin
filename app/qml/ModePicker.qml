import QtQuick
import QtQuick.Layouts

// The multiplier: off, one that is held (2×, 3×, 4×), or Auto, and a line that says what the choice
// does. The same on Play and in Options. A change is kept for the next launch and goes to a game
// that runs with the layer at once.
ColumnLayout {
    id: root
    property bool live: false     // a game runs with the layer
    property bool brief: false    // one line under the choices (Play); the longer words are Options'
    readonly property var modes: ["off", "2", "3", "4", "auto"]
    readonly property string mode: backend.prefs.mode
    readonly property int limit: backend.prefs.limit
    readonly property var held: ({
        "2": "One generated frame between every two of the game's, whatever its frame rate. The game's own rate is kept to half the display's at the most.",
        "3": "Two generated frames between every two of the game's, whatever its frame rate. The game's own rate is kept to a third of the display's at the most.",
        "4": "Three generated frames between every two of the game's, whatever its frame rate. The game's own rate is kept to a quarter of the display's at the most."
    })
    readonly property var count: ({ "2": "one generated frame", "3": "two generated frames", "4": "three generated frames" })
    readonly property string few: mode === "off" ? (live ? "Generation is off: the game shows its own frames." : "The game as it is, without Myrddin's layers.")
        : mode === "auto" ? (limit === 2 ? "2×, and none while the game alone is near the display's rate."
                                         : "Follows the game's frame rate, from 2× up to " + limit + "×.")
        : "Always " + mode + "×: " + count[mode] + " for each of the game's."
    readonly property string words: mode === "off" ? (live ? "Generation is off: the game shows its own frames." : "The game as it is, with none of Myrddin's layers loaded.")
        : mode === "auto" ? (limit === 2 ? "Myrddin generates one frame between every two of the game's, and none while the game alone is near the display's rate."
                                         : "Myrddin picks the multiplier as the game's own rate changes, from 2× up to " + limit + "×.")
        : held[mode]
    spacing: brief ? 10 : 12

    // (the row of choices is as wide as there is room for, down to what five words need; it stands in
    // a holder so that the layout can make it narrower than it would like to be)
    Item {
        Layout.fillWidth: true
        Layout.minimumWidth: 5 * 50 + 8
        implicitWidth: 5 * 84 + 8
        implicitHeight: choices.implicitHeight
        Segmented {
            id: choices
            large: true
            cell: Math.max(50, Math.min(84, Math.floor((parent.width - 8) / 5)))
            options: ["Off", "2×", "3×", "4×", "Auto"]
            index: Math.max(0, root.modes.indexOf(root.mode))
            onChosen: (i) => backend.setMode(root.modes[i])
        }
    }
    Text {
        id: explain
        text: root.brief ? root.few : root.words + (root.live ? " A change acts at once in the running game." : "")
        color: Theme.dim
        font.pixelSize: 14
        lineHeight: 1.2
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        onTextChanged: swap.restart()
        NumberAnimation { id: swap; target: explain; property: "opacity"; from: 0.2; to: 1; duration: Theme.smooth }
    }
}
