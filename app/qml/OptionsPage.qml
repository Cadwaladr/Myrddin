import QtQuick
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "Options"
    subtitle: "Kept for every launch. The multiplier, the limit on the game's frame rate and the HUD and performance switches also act at once in a running game."

    readonly property bool live: backend.state === "running" && backend.layerSession

    function displayModel() {
        var list = [{ text: backend.displayChoice !== "" ? "Ask the desktop: " + backend.displayChoice.split(" ")[0] : "Ask the desktop", value: "" }]
        for (var i = 0; i < backend.displays.length; i++) {
            var name = backend.displays[i].split(" ")[0]
            list.push({ text: backend.displays[i], value: name })
        }
        var mine = backend.conf.monitor
        if (mine !== "" && mine !== "off" && !list.some((e) => e.value === mine)) list.push({ text: mine + " (not connected)", value: mine })
        list.push({ text: "Leave it to the runner", value: "off" })
        return list
    }
    function rateModel() {
        var list = [{ text: "The display's own", value: "" }]
        var rates = ["60", "75", "100", "120", "144", "165", "240"]
        var mine = backend.conf.targetRate
        if (mine !== "" && rates.indexOf(mine) < 0) rates.push(mine)
        for (var i = 0; i < rates.length; i++) list.push({ text: rates[i] + " Hz", value: rates[i] })
        return list
    }
    function schedModel() {
        var list = [{ text: "The kernel's own", value: "" }]
        for (var i = 0; i < backend.schedulers.length; i++) list.push({ text: "scx_" + backend.schedulers[i], value: backend.schedulers[i] })
        var mine = backend.conf.scheduler
        if (mine !== "" && !list.some((e) => e.value === mine)) list.push({ text: "scx_" + mine, value: mine })
        return list
    }

    Card {
        heading: "Display and frame generation"
        note: "The display the game takes as its main one, its refresh rate, and how many frames Myrddin generates. On KDE Plasma and COSMIC the display and its rate are asked of the desktop at every launch."
        SettingRow {
            title: "Display"
            Choice { implicitWidth: 330; model: page.displayModel(); value: backend.conf.monitor; onPicked: (v) => backend.conf.monitor = v }
        }
        SettingRow {
            title: "Refresh rate to fill"
            detail: "No more frames are shown than this. Elsewhere than on those two desktops 120 Hz is assumed: name the rate here."
            Choice { implicitWidth: 200; model: page.rateModel(); value: backend.conf.targetRate; onPicked: (v) => backend.conf.targetRate = v }
        }
        Divider {}
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            Text { text: "Multiplier"; color: Theme.text; font.pixelSize: 14; font.weight: Font.Medium }
            ModePicker { Layout.fillWidth: true; live: page.live }
        }
        SettingRow {
            title: "Highest multiplier"
            detail: "The most that Auto picks. 4× allows up to three generated frames between every two of the game's; with a lower limit the shown rate may stay under the display's."
            dimmed: backend.prefs.mode !== "auto"
            Segmented {
                cell: backend.conf.experimental ? 44 : 54
                options: backend.conf.experimental ? ["2×", "3×", "4×", "5×", "6×", "7×", "8×"] : ["2×", "3×", "4×"]
                index: backend.prefs.limit - 2
                onChosen: (i) => backend.setLimit(i + 2)
            }
        }
        SettingRow {
            title: "Experimental: 5× to 8×"
            detail: "Lets Auto go up to 8×, for places where the game's own rate falls below 30 frames a second. "
                  + "Every generated frame is a guess between two of the game's, and what you do still takes one of the game's frames to show, "
                  + "so more of them make a smoother picture, not a quicker one. Off by default; acts from the next launch."
            link: backend.multipliersPage
            linkText: "Experimental multipliers on GitHub"
            Toggle { checked: backend.conf.experimental; onToggled: (v) => backend.setOption("experimental", v) }
        }
        SettingRow {
            title: "Limit the game's own frame rate"
            detail: "For Auto. On: the game's own frames are limited to an even part of the display's rate (a half, a third or a quarter; down to an eighth with 5× to 8×), so that every refresh gets a frame. "
                  + "That adds latency: the game makes fewer frames of its own. Off: they are never limited, so the game answers as fast as it can; the shown rate may stay under the display's. "
                  + "At 120 Hz, a game that could do 50 frames a second shows 100 with this off (2×, the game at 50) and 120 with it on (3×, the game held at 40). Off by default."
            dimmed: backend.prefs.mode !== "auto"
            Toggle { checked: backend.conf.fill; onToggled: (v) => backend.setOption("fill", v) }
        }
    }

    Card {
        heading: "Performance overlays"
        note: "What is drawn over the game to show how it runs: one of them, or none. It is chosen at launch."
        SettingRow {
            title: "Overlay"
            detail: overlay.which === "mangohud" ? (!backend.mangohudInstalled ? "MangoHud is not installed on this system: the game starts without it."
                                                    : overlay.generating ? "MangoHud sits below Myrddin's layer, so its frame rate is the shown one, generated frames included. Its own frame limit must stay off."
                                                    : "With frame generation Off, MangoHud shows the game's own frame rate.")
                  : overlay.which === "panel" ? "Myrddin's own readout, there from the start of the game. Its key steps through it in the game: the shown rate, both rates, both with a pacing graph, off."
                                                + (overlay.generating ? "" : " With frame generation Off the layer is loaded to draw it and generates nothing; generation can be switched on in the running game.")
                  : backend.mangohudInstalled ? "Nothing is drawn over the game." : "Nothing is drawn over the game. MangoHud is not installed on this system, so it is not listed."
            OverlayChoice { id: overlay }
        }
        SettingRow {
            title: "Panel corner and key"
            detail: "For Myrddin's panel. The key is read inside the game, while the game has the keyboard: nothing is set up on the desktop, and it is the same on every desktop. The game gets the key as well, so pick one it does not use. For another key, click the key here and press it, with Ctrl, Shift or Alt if you like. Keys go by their place on a US keyboard."
            dimmed: overlay.which !== "panel"
            RowLayout {
                spacing: 10
                Segmented {
                    cell: 44
                    enabled: overlay.which === "panel"
                    options: ["↖", "↗", "↙", "↘"]
                    index: Math.max(0, ["tl", "tr", "bl", "br"].indexOf(backend.prefs.panelCorner))
                    onChosen: (i) => { var c = ["tl", "tr", "bl", "br"][i]; backend.prefs.panelCorner = c; backend.panel(c) }
                }
                KeyField {
                    enabled: overlay.which === "panel"
                    value: backend.conf.panelKey
                    onPicked: (keys) => backend.setPanelKey(keys)
                }
            }
        }
    }

    Card {
        heading: "HUD"
        note: "How the game's HUD is treated in the frames Myrddin generates."
        SettingRow {
            title: "Move HUD markers"
            detail: "On: target markers, the compass and a ship's flight display move in every frame that is shown, as smoothly as the rest of the picture. Off: they move only when the game draws a new frame of its own, in steps."
            Toggle { checked: backend.conf.markers; onToggled: (v) => backend.setOption("markers", v) }
        }
        SettingRow {
            title: "Keep the HUD sharp"
            detail: "On: the HUD is taken out before frames are generated and put back over them, so its letters stay sharp. Off: it is generated along with the picture and smears when the view moves."
            Toggle { checked: backend.conf.keepHud; onToggled: (v) => backend.setOption("keepHud", v) }
        }
        SettingRow {
            title: "Bridge frames that come without the HUD"
            detail: "Now and then the game hands over a frame without its HUD layer. On: the picture under the HUD is worked out for it and generation goes on. Off: nothing is generated from such a frame."
            Toggle { checked: backend.conf.gapfill; onToggled: (v) => backend.setOption("gapfill", v) }
        }
        SettingRow {
            title: "Leave the map at the game's rate"
            detail: "On: the map at the top left changes only with the game's own frames. Off: it is moved like the markers, which doubles its lines."
            Toggle { checked: backend.conf.pictures; onToggled: (v) => backend.setOption("pictures", v) }
        }
    }

    Card {
        heading: "Performance"
        SettingRow {
            title: "Hold the GPU's speed up"
            detail: "Generated frames are ready sooner when the game leaves the GPU half idle. Off unless you switch it on."
            Toggle { checked: backend.conf.boost; onToggled: (v) => backend.setOption("boost", v) }
        }
    }

    Card {
        heading: "NVIDIA settings from the LUG wiki"
        note: "Placed in Myrddin's copy of the launch script, never in the script itself, and only where the script does not set them already. Each acts from the next launch."
        SettingRow {
            title: "DLSS 4"
            detail: "The game uses the newest DLSS, with its newest preset, in place of the one it came with: NVIDIA's updater fetches it into the game's folder and dxvk-nvapi makes the game use it. It is for the picture, not for the frame rate. On by default."
            Toggle { checked: backend.conf.dlss4; onToggled: (v) => backend.setOption("dlss4", v) }
        }
        SettingRow {
            title: "Graphics memory limit"
            detail: "The game is told the card has less memory than it has (9 GB on a 12 GB card, 8 on 10, 6 on 8), so that it leaves room for the desktop, DLSS and Myrddin: the wiki's remedy for frame drops when the card's memory runs out. Off: the game is told all of it. On by default."
            Toggle { checked: backend.conf.vramLimit; onToggled: (v) => backend.setOption("vramLimit", v) }
        }
    }

    Card {
        heading: "Advanced performance tweaks"
        note: "These switches work once the matching tweak has been set up on this computer, which needs root once; Myrddin never asks for a password. Until then a launch leaves the computer as it is. Each takes effect at the next launch and is undone when the game has closed."
        ActionButton {
            text: "Advanced performance tweaking on GitHub"
            icon: Theme.iconLink
            onClicked: backend.openLink(backend.tweaksPage)
        }
        Divider {}
        SettingRow {
            title: "CPU scheduler"
            detail: "A sched_ext scheduler for as long as the game runs; the kernel's own comes back afterwards. On the test machine scx_lavd gave about 3 % more frames, which is within what runs differ by, and shorter slow frames."
            Choice { implicitWidth: 240; model: page.schedModel(); value: backend.conf.scheduler; onPicked: (v) => backend.conf.scheduler = v }
        }
        Divider {}
        SettingRow {
            title: "CPU prefetchers off while playing"
            detail: "The CPU stops fetching memory it only guesses will be read, which leaves more of the memory's time to what the game asks for. On the test machine the game ran 4 to 7 % faster at two busy spots. For Intel Core CPUs of the 12th to 14th generation."
            Toggle { checked: backend.conf.prefetchOff; onToggled: (v) => backend.setOption("prefetchOff", v) }
        }
        Divider {}
        SettingRow {
            title: "Huge pages while playing"
            detail: "The game's memory goes into pages of 2 MiB instead of 4 KiB, so the CPU spends less time finding its way in it. On the test machine about half of the game's memory got there; the frame rate stayed within what runs differ by, and no hitches were seen."
            Toggle { checked: backend.conf.hugePages; onToggled: (v) => backend.setOption("hugePages", v) }
        }
    }

    Card {
        heading: "Known problems"
        note: backend.problems.length === 0 ? "Nothing on this desktop is known to get in the way of the game." : "Settings of this desktop that are known to get in the way of the game. A fix changes the desktop's own setting; Undo puts it back as it was."
        Repeater {
            model: backend.problems
            SettingRow {
                title: modelData.title
                detail: modelData.detail
                ActionButton {
                    text: modelData.present ? "Fix" : "Undo"
                    kind: modelData.present ? "plain" : "quiet"
                    onClicked: modelData.present ? backend.fixProblem(modelData.id) : backend.undoProblem(modelData.id)
                }
            }
        }
    }

    Card {
        heading: "Logs and reports"
        SettingRow {
            title: "Report a bug"
            detail: "A short report from the layer's log of the last session: what it says went wrong, the settings and the system's versions, without the names of your home folder, the game's folder or this computer. It is copied, and the window says where to send it."
            ActionButton { text: "Report a bug"; icon: Theme.iconLog; onClicked: backend.makeBugReport() }
        }
        Divider {}
        SettingRow {
            title: "Run with logs"
            detail: "Writes a log of each session: what the launch said, the frame rates, and what the game used of the disk, memory and graphics memory. It is read from outside the game. When the game closes, the window offers the log: read it, copy it or delete it."
            Toggle { checked: backend.prefs.watch; onToggled: (v) => backend.prefs.watch = v }
        }
        Divider {}
        SettingRow {
            title: "Reports"
            detail: "Each shows what an sc-turbo command says. Most need the game to be running."
        }
        Flow {
            Layout.fillWidth: true
            spacing: 10
            ActionButton { text: "The last launch"; onClicked: backend.showLaunchLog() }
            ActionButton { text: "Session log"; icon: Theme.iconLog; enabled: !backend.toolBusy; onClicked: backend.readSessionLog() }
            ActionButton { text: "Last ten minutes"; enabled: !backend.toolBusy; onClicked: backend.runTool("Last ten minutes", ["fg-watch", "--last", "60"]) }
            ActionButton { text: "Latency"; enabled: !backend.toolBusy; onClicked: backend.runTool("Latency", ["latency"]) }
            ActionButton { text: "Session check"; enabled: !backend.toolBusy; onClicked: backend.runTool("Session check", ["check"]) }
            ActionButton { text: "Displays"; enabled: !backend.toolBusy; onClicked: backend.runTool("Displays", ["displays"]) }
            ActionButton { text: "Threads"; enabled: !backend.toolBusy; onClicked: backend.runTool("Threads", ["threads"]) }
            ActionButton { text: "Where the layer stands"; enabled: !backend.toolBusy; onClicked: backend.runTool("Where the layer stands", ["fg-where"]) }
            ActionButton { text: "Open the log folder"; icon: Theme.iconFolder; onClicked: backend.openFolder("logs") }
        }
    }
}
