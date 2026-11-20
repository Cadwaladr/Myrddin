import QtQuick

// What is drawn over the game to show how it runs: nothing, MangoHud, or Myrddin's own panel.
// One of them at a time; it is chosen at launch.
Choice {
    id: box
    readonly property string which: backend.conf.mangohud ? "mangohud" : backend.prefs.panel ? "panel" : "off"
    // (a launch with generation Off is the game as it is: nothing of Myrddin's is loaded, no overlay either)
    readonly property bool unused: which !== "off" && backend.prefs.mode === "off"
    readonly property string words: unused ? "A launch with frame generation Off starts the game as it is: no overlay is drawn."
                                  : which === "mangohud" ? (backend.mangohudInstalled ? "Its frame rate counts every frame that is shown, generated ones too." : "MangoHud is not installed on this system: the game starts without it.")
                                  : which === "panel" ? "Myrddin's own readout: the shown rate, the game's rate, pacing."
                                  : "Nothing is drawn over the game."
    readonly property string few: unused ? "Not drawn while frame generation is Off."
                                : which === "mangohud" ? (backend.mangohudInstalled ? "Counts every shown frame, generated ones too." : "Not installed: the game starts without it.")
                                : which === "panel" ? "The shown rate, the game's rate and pacing."
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
