import QtQuick
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "Options"
    subtitle: "Saved for every launch. The multiplier, the frame-rate limit and the HUD and GPU switches also change a running game straight away."

    readonly property bool live: backend.state === "running" && backend.layerSession

    function displayModel() {
        var list = [{ text: backend.displayChoice !== "" ? "Ask the desktop: " + backend.displayChoice.split(" ")[0] : "Ask the desktop", value: "" }]
        for (var i = 0; i < backend.displays.length; i++) {
            var name = backend.displays[i].split(" ")[0]
            list.push({ text: backend.displays[i], value: name })
        }
        var mine = backend.conf.monitor
        if (mine !== "" && mine !== "off" && !list.some((e) => e.value === mine)) list.push({ text: mine + " (not connected)", value: mine })
        list.push({ text: "Let Wine pick", value: "off" })
        return list
    }
    function rateModel() {
        var list = [{ text: "Your display's rate", value: "" }]
        var rates = ["60", "75", "100", "120", "144", "165", "240"]
        var mine = backend.conf.targetRate
        if (mine !== "" && rates.indexOf(mine) < 0) rates.push(mine)
        for (var i = 0; i < rates.length; i++) list.push({ text: rates[i] + " Hz", value: rates[i] })
        return list
    }
    function schedModel() {
        var list = [{ text: "System default", value: "" }]
        for (var i = 0; i < backend.schedulers.length; i++) list.push({ text: "scx_" + backend.schedulers[i], value: backend.schedulers[i] })
        var mine = backend.conf.scheduler
        if (mine !== "" && !list.some((e) => e.value === mine)) list.push({ text: "scx_" + mine, value: mine })
        return list
    }

    Card {
        heading: "Display and frame generation"
        note: "Which display the game uses, its refresh rate, and how many frames Myrddin adds. On KDE Plasma and COSMIC both are read from the desktop at every launch."
        SettingRow {
            title: "Display"
            Choice { implicitWidth: 330; model: page.displayModel(); value: backend.conf.monitor; onPicked: (v) => backend.conf.monitor = v }
        }
        SettingRow {
            title: "Refresh rate"
            detail: "Myrddin never shows more frames a second than this. On other desktops it assumes 120 Hz, so pick your display's rate here."
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
            detail: "The most Auto will use. At 4×, up to three frames are added between each two of the game's. With a lower limit, the frame rate may stay below your display's."
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
            detail: "Lets Auto go up to 8× where the game runs below 30 frames a second. More added frames make the picture smoother, "
                  + "but the game does not respond any faster. Off by default; takes effect at the next launch."
            link: backend.multipliersPage
            linkText: "Experimental multipliers on GitHub"
            Toggle { checked: backend.conf.experimental; onToggled: (v) => backend.setOption("experimental", v) }
        }
        SettingRow {
            title: "Limit the game's own frame rate"
            detail: "For Auto. On: the game's own frame rate is held at a half, a third or a quarter of your display's (down to an eighth with 5× to 8×), so every refresh gets a frame, "
                  + "but the game responds a little later. Off: the game runs as fast as it can, and the frame rate may stay below your display's. Off by default."
            dimmed: backend.prefs.mode !== "auto"
            Toggle { checked: backend.conf.fill; onToggled: (v) => backend.setOption("fill", v) }
        }
    }

    Card {
        heading: "Performance overlays"
        note: "What is shown over the game to tell you how it runs. Chosen at launch."
        SettingRow {
            title: "Overlay"
            detail: overlay.which === "mangohud" ? (!backend.mangohudInstalled ? "MangoHud is not installed on this system: the game starts without it."
                                                    : overlay.generating ? "MangoHud counts every frame you see, generated ones included. Its own frame limit is switched off for the launch."
                                                    : "With frame generation Off, MangoHud shows the game's own frame rate.")
                  : overlay.which === "panel" ? "Myrddin's own readout, shown from the start. Its key switches between the frame rate you see, both frame rates, both with a timing graph, and off."
                                                + (overlay.generating ? "" : " With frame generation Off it shows the game's own rate, and generation can still be switched on while you play.")
                  : backend.mangohudInstalled ? "Nothing is drawn over the game." : "Nothing is drawn over the game. MangoHud is not installed on this system, so it is not listed."
            OverlayChoice { id: overlay }
        }
        SettingRow {
            title: "Panel corner and key"
            detail: "Where Myrddin's panel sits, and the key that switches what it shows while you play. The game gets the key too, so pick one the game does not use. To change it, click the key and press a new one. Key names are as on a US keyboard."
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
        note: "How the game's HUD looks in the frames Myrddin adds."
        SettingRow {
            title: "Move HUD markers"
            detail: "On: target markers, the compass and the flight display move as smoothly as the rest of the picture. Off: they move in steps, only with the game's own frames."
            Toggle { checked: backend.conf.markers; onToggled: (v) => backend.setOption("markers", v) }
        }
        SettingRow {
            title: "Keep the HUD sharp"
            detail: "On: the HUD is left out of the added frames and drawn over them, so its text stays sharp. Off: it smears when the view moves."
            Toggle { checked: backend.conf.keepHud; onToggled: (v) => backend.setOption("keepHud", v) }
        }
        SettingRow {
            title: "Smooth over frames without the HUD"
            detail: "Now and then the game sends a frame without its HUD. On: Myrddin works around it and keeps adding frames. Off: no frames are added from it."
            Toggle { checked: backend.conf.gapfill; onToggled: (v) => backend.setOption("gapfill", v) }
        }
        SettingRow {
            title: "Leave the map at the game's rate"
            detail: "On: the map at the top left changes only with the game's own frames. Off: it moves like the markers, and its lines look doubled."
            Toggle { checked: backend.conf.pictures; onToggled: (v) => backend.setOption("pictures", v) }
        }
    }

    Card {
        heading: "Performance"
        SettingRow {
            title: "Keep the GPU's speed up"
            detail: "Added frames are ready sooner when the game leaves the GPU half idle. Off by default."
            Toggle { checked: backend.conf.boost; onToggled: (v) => backend.setOption("boost", v) }
        }
    }

    Card {
        heading: "NVIDIA settings from the LUG wiki"
        note: "Added to Myrddin's own copy of your launch script, never to the script itself, and only if the script does not set them already. They take effect at the next launch."
        SettingRow {
            title: "DLSS 4"
            detail: "The game uses NVIDIA's newest DLSS instead of the one it came with, fetched by NVIDIA's own updater. It improves the picture, not the frame rate. On by default."
            Toggle { checked: backend.conf.dlss4; onToggled: (v) => backend.setOption("dlss4", v) }
        }
        SettingRow {
            title: "Graphics memory limit"
            detail: "The game is told the graphics card has less memory than it really has, so room is left for the desktop, DLSS and Myrddin. This avoids frame drops when the card's memory runs out. On by default."
            Toggle { checked: backend.conf.vramLimit; onToggled: (v) => backend.setOption("vramLimit", v) }
        }
    }

    Card {
        heading: "Advanced performance tweaks"
        note: "These need a one-time setup with administrator rights, described on GitHub; Myrddin itself never asks for a password. Until then they change nothing. Each takes effect at the next launch and is undone when the game closes."
        ActionButton {
            text: "Advanced performance tweaking on GitHub"
            icon: Theme.iconLink
            onClicked: backend.openLink(backend.tweaksPage)
        }
        Divider {}
        SettingRow {
            title: "CPU scheduler"
            detail: "The CPU scheduler decides which program runs on the CPU and when. The one picked here is used while the game runs; the system default comes back afterwards."
            Choice { implicitWidth: 240; model: page.schedModel(); value: backend.conf.scheduler; onPicked: (v) => backend.conf.scheduler = v }
        }
        Divider {}
        SettingRow {
            title: "Disable CPU prefetchers"
            detail: "Stops the CPU from reading memory ahead on a guess, so more of the memory's speed goes to what the game needs. For Intel Core CPUs of the 12th to 14th generation."
            Toggle { checked: backend.conf.prefetchOff; onToggled: (v) => backend.setOption("prefetchOff", v) }
        }
        Divider {}
        SettingRow {
            title: "Huge pages"
            detail: "The game's memory is handed out in bigger pieces, so the CPU finds its way around it faster."
            Toggle { checked: backend.conf.hugePages; onToggled: (v) => backend.setOption("hugePages", v) }
        }
    }

    Card {
        heading: "Known problems"
        note: backend.problems.length === 0 ? "Nothing on this desktop is known to cause trouble in the game." : "Desktop settings known to cause trouble in the game. Fix changes the setting; Undo puts it back."
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
            detail: "A short report of what went wrong in your last session, with your settings and system versions, but without your folder or computer names. It is copied for you, and the window says where to send it."
            ActionButton { text: "Report a bug"; icon: Theme.iconLog; onClicked: backend.makeBugReport() }
        }
        Divider {}
        SettingRow {
            title: "Run with logs"
            detail: "Keeps a log of each session: the launch messages, frame rates, and how much disk, memory and graphics memory the game used, all read from outside the game. When the game closes, you can read, copy or delete it."
            Toggle { checked: backend.prefs.watch; onToggled: (v) => backend.prefs.watch = v }
        }
        Divider {}
        SettingRow {
            title: "Reports"
            detail: "Each runs a check and shows the answer. Most need the game to be running."
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
            ActionButton { text: "Frame generation status"; enabled: !backend.toolBusy; onClicked: backend.runTool("Frame generation status", ["fg-where"]) }
            ActionButton { text: "Open the log folder"; icon: Theme.iconFolder; onClicked: backend.openFolder("logs") }
        }
    }
}
