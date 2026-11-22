import QtQuick

// What is drawn over the game to show how it runs: nothing, MangoHud, or Myrddin's own panel.
// One of them at a time; it is chosen at launch.
Choice {
    id: box
    readonly property string which: backend.conf.mangohud ? "mangohud" : backend.prefs.panel ? "panel" : "off"
    // (what is chosen is drawn, whatever the multiplier: with frame generation Off, MangoHud shows the
    // game's own rate, and Myrddin's panel comes with the layer loaded and nothing generated)
    readonly property bool generating: backend.prefs.mode !== "off"
    readonly property string words: which === "mangohud" ? (!backend.mangohudInstalled ? "MangoHud is not installed on this system: the game starts without it."
                                                           : generating ? "Counts every frame you see, generated ones included."
                                                           : "With frame generation Off it shows the game's own frame rate.")
                                  : which === "panel" ? (generating ? "Myrddin's own readout: the frame rate you see, the game's own rate and frame timing."
                                                        : "Myrddin's own readout. With frame generation Off it shows the game's own rate, and generation can still be switched on while you play.")
                                  : "Nothing is drawn over the game."
    readonly property string few: which === "mangohud" ? (!backend.mangohudInstalled ? "Not installed: the game starts without it."
                                                         : generating ? "Counts every frame you see, generated ones too." : "The game's own frame rate.")
                                : which === "panel" ? (generating ? "Both frame rates and frame timing." : "The game's rate and frame timing; no frames added.")
                                : "Nothing is drawn over the game."
    implicitWidth: 220
    model: {
        var list = [{ text: "Off", value: "off" }]
        // (MangoHud is listed where it is installed, and where it is what the settings say)
        if (backend.mangohudInstalled || which === "mangohud") list.push({ text: backend.mangohudInstalled ? "MangoHud" : "MangoHud (not installed)", value: "mangohud" })
        list.push({ text: "Myrddin's panel", value: "panel" })
        return list
    }
    value: which
    onPicked: (v) => backend.setOverlay(v)
}
