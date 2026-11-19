import QtQuick
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "Options"
    subtitle: "Kept in sc-turbo.conf, so a launch from a terminal uses them too."

    readonly property bool live: backend.state === "running" && backend.layerSession
    // what the window sent to the running game; the layer starts every session with its defaults
    property var sent: ({})
    Connections {
        target: backend
        function onStateChanged() { if (backend.state !== "running") page.sent = ({}) }
    }
    function state(name, fallback) { return page.sent[name] !== undefined ? page.sent[name] : fallback }
    function remember(name, value) { var s = page.sent; s[name] = value; page.sent = s }

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
        if (mine !== "" && !list.some((e) => e.value === mine)) list.push({ text: "scx_" + mine + " (not installed)", value: mine })
        return list
    }

    Card {
        heading: "Display"
        note: "The display the game takes as its main one, and the rate Myrddin fills with frames. On KDE Plasma and COSMIC both are asked of the desktop at every launch."
        SettingRow {
            title: "Display"
            Choice { implicitWidth: 330; model: page.displayModel(); value: backend.conf.monitor; onPicked: (v) => backend.conf.monitor = v }
        }
        SettingRow {
            title: "Refresh rate to fill"
            detail: "Elsewhere than on those two desktops 120 Hz is assumed: name the rate here."
            Choice { implicitWidth: 200; model: page.rateModel(); value: backend.conf.targetRate; onPicked: (v) => backend.conf.targetRate = v }
        }
    }

    Card {
        heading: "The multiplier"
        note: "The multiplier chosen on Play is the upper limit. Below it the layer picks one as the game's own rate changes."
        SettingRow {
            title: "Fill the display"
            detail: "On: the fewest generated frames that fill the display's rate, evenly spaced. A game that could run faster than its step is held there: 30 frames a second at ×4 on a 120 Hz display, 40 at ×3. Off: the game is never held, and the shown rate may stay under the display's."
            Toggle { checked: backend.conf.fill; onToggled: (v) => backend.conf.fill = v }
        }
    }

    Card {
        heading: "In the picture"
        SettingRow {
            title: "MangoHud"
            detail: backend.mangohudInstalled ? "Sits below Myrddin's layer, so its frame rate is the shown one. Its own frame limit must stay off." : "Not installed on this system."
            dimmed: !backend.mangohudInstalled
            Toggle { checked: backend.conf.mangohud; enabled: backend.mangohudInstalled; onToggled: (v) => backend.conf.mangohud = v }
        }
        Divider {}
        SettingRow {
            title: "Performance panel"
            detail: "Myrddin's own readout. In the game a key steps through it: off, the shown rate, both rates, both with a pacing graph."
            Toggle { checked: backend.prefs.panel; onToggled: (v) => backend.prefs.panel = v }
        }
        SettingRow {
            title: "Panel corner and key"
            detail: "The key is Ctrl+Shift+` unless you choose another; it is set up on KDE Plasma and COSMIC."
            dimmed: !backend.prefs.panel
            RowLayout {
                spacing: 10
                Segmented {
                    cell: 44
                    enabled: backend.prefs.panel
                    options: ["↖", "↗", "↙", "↘"]
                    index: Math.max(0, ["tl", "tr", "bl", "br"].indexOf(backend.prefs.panelCorner))
                    onChosen: (i) => { var c = ["tl", "tr", "bl", "br"][i]; backend.prefs.panelCorner = c; backend.panel(c) }
                }
                ActionButton {
                    text: "Set up the key"
                    enabled: backend.prefs.panel
                    onClicked: { backend.runTool("Panel key", ["fg-panel-key", "install"]); page.go("session") }
                }
            }
        }
        Divider {}
        SettingRow {
            title: "Move HUD markers"
            detail: "World markers, the compass and flight displays are put where they are on their way in every generated frame. Off: they stay at the game's own rate."
            Toggle { checked: backend.conf.markers; onToggled: (v) => backend.conf.markers = v }
        }
    }

    Card {
        heading: "CPU scheduler"
        note: "A sched_ext scheduler for as long as the game runs; the kernel's own comes back afterwards. On the test machine scx_lavd measured the same as the kernel's own."
        SettingRow {
            title: "While the game runs"
            detail: backend.schedulerAllowed ? "" : "Switching is not set up on this system: it needs scx-scheds, scx-tools and a polkit rule (the README says how)."
            Choice { implicitWidth: 240; model: page.schedModel(); value: backend.conf.scheduler; onPicked: (v) => backend.conf.scheduler = v }
        }
    }

    Card {
        heading: "Recording"
        SettingRow {
            title: "Record disk and memory use"
            detail: "The game's disk, memory and GPU-memory use, second by second, read from outside. Session shows the report."
            Toggle { checked: backend.prefs.watch; onToggled: (v) => backend.prefs.watch = v }
        }
    }

    Card {
        heading: "In the running game"
        note: page.live ? "These act at once and go back to their defaults with the next launch." : "Available while the game runs with the layer. They act at once and go back to their defaults with the next launch."
        Repeater {
            model: [
                { name: "fill", kind: "param", on: backend.conf.fill, title: "Fill the display", detail: "As above, for this session: off lets a held game run free at once." },
                { name: "fg-cap", kind: "switch", on: true, title: "Stop at the display's rate", detail: "No more frames are shown than the display can take." },
                { name: "fg-hud", kind: "switch", on: true, title: "Keep the HUD apart", detail: "The HUD is taken out before frames are generated and put back sharp." },
                { name: "gapfill", kind: "param", on: true, title: "Bridge frames without a HUD capture", detail: "Their scene is worked out from the frame and the last capture." },
                { name: "pictures", kind: "param", on: true, title: "Leave the map at the game's rate", detail: "Pictures in the HUD that turn in place are not moved; moving them doubled their lines." },
                { name: "boost", kind: "param", on: false, title: "Hold the GPU's speed up", detail: "Generated frames are ready sooner when the game leaves the GPU half idle." }
            ]
            SettingRow {
                id: liveRow
                required property var modelData
                title: modelData.title
                detail: modelData.detail
                dimmed: !page.live
                Toggle {
                    enabled: page.live
                    checked: page.state(liveRow.modelData.name, liveRow.modelData.on)
                    onToggled: (v) => {
                        page.remember(liveRow.modelData.name, v)
                        if (liveRow.modelData.kind === "param") backend.sendParam(liveRow.modelData.name, v ? "1" : "0")
                        else backend.liveSwitch(liveRow.modelData.name, v)
                    }
                }
            }
        }
    }
}
