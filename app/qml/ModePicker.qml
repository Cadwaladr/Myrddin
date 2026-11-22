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
        "2": "Adds one frame between each two of the game's, at any frame rate. The game's own rate is held to half your display's at most.",
        "3": "Adds two frames between each two of the game's, at any frame rate. The game's own rate is held to a third of your display's at most.",
        "4": "Adds three frames between each two of the game's, at any frame rate. The game's own rate is held to a quarter of your display's at most."
    })
    readonly property var count: ({ "2": "one generated frame", "3": "two generated frames", "4": "three generated frames" })
    readonly property string few: mode === "off" ? (live ? "Generation is off: the game shows its own frames." : "The game as it is, without Myrddin.")
        : mode === "auto" ? (limit === 2 ? "2×, or none when the game is close to your display's rate."
                                         : "Follows the game's frame rate, from 2× up to " + limit + "×.")
        : "Always " + mode + "×: " + count[mode] + " for each of the game's."
    readonly property string words: mode === "off" ? (live ? "Generation is off: the game shows its own frames." : "The game as it is, without Myrddin.")
        : mode === "auto" ? (limit === 2 ? "Myrddin adds one frame between each two of the game's, or none when the game is close to your display's rate."
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
        text: root.brief ? root.few : root.words + (root.live ? " A change applies straight away in the running game." : "")
        color: Theme.dim
        font.pixelSize: 14
        lineHeight: 1.2
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        onTextChanged: swap.restart()
        NumberAnimation { id: swap; target: explain; property: "opacity"; from: 0.2; to: 1; duration: Theme.smooth }
    }
}
