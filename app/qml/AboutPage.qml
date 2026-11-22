import QtQuick
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "About"

    Card {
        padding: 26
        RowLayout {
            Layout.fillWidth: true
            spacing: 20
            Mark { size: 64 }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Text { text: "Myrddin"; color: Theme.text; font.pixelSize: 24; font.weight: Font.DemiBold; font.letterSpacing: -0.3 }
                Text { text: "Version " + backend.version + (backend.runtimeVersion !== "" ? "  ·  runtime " + backend.runtimeVersion : ""); color: Theme.dim; font.pixelSize: 13 }
                Text { text: "Made by Claude, Anthropic's AI model, and directed by Cadwaladr."; color: Theme.dim; font.pixelSize: 13; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }
        Divider {}
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: 1.35
            color: Theme.dim
            font.pixelSize: 13
            text: "Frame generation for Star Citizen on Linux, as a Vulkan layer on the host side of Wine. It does not change the game's files, its code or its memory, and sends it no input."
        }
    }

    Card {
        color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.06)
        border.color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.35)
        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            Icon { path: Theme.iconWarn; color: Theme.warn; size: 22; Layout.alignment: Qt.AlignTop }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: 1.35
                color: Theme.text
                font.pixelSize: 13
                text: "Experimental, and tested on one machine. Star Citizen is protected by Easy Anti-Cheat. Myrddin is not affiliated with or endorsed by Cloud Imperium Games and comes with no guarantee of any kind. You use it at your own risk."
            }
        }
    }

    Card {
        heading: "Licence"
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: 1.35
            color: Theme.dim
            font.pixelSize: 13
            text: "Myrddin is free software under the GNU General Public License, version 3 or later. The code, the tests and the documents were written by Claude, working in Claude Code; Cadwaladr directed the work, decided what it should do and tested it in the game. Myrddin's frame generation from the game's motion vectors is its own. The approach comes from a fork of the PS5 emulator KytyPS5 (chenxiao07/KytyPS5, pull requests 117 and 118 by Bios2000): a guide, none of its code."
        }
        Repeater {
            model: [
                { name: "AMD FidelityFX SDK 1.1.4", what: "frame interpolation and optical flow, changed to yield several generated frames per pair", licence: "MIT" },
                { name: "Eden Emulator Project", what: "the SDK's Linux build recipe and Vulkan test helpers (Copyright 2026 Eden Emulator Project)", licence: "GPL-3.0-or-later" },
                { name: "Adwaita Sans", what: "the letters of the panel in the game", licence: "SIL OFL 1.1" },
                { name: "Inter", what: "the letters of this window", licence: "SIL OFL 1.1" },
                { name: "Qt 6", what: "this window", licence: "LGPL-3.0" },
                { name: "ICU", what: "Unicode data inside Qt, in the AppImage", licence: "Unicode" }
            ]
            RowLayout {
                id: part
                required property var modelData
                Layout.fillWidth: true
                spacing: 12
                Text { text: part.modelData.name; color: Theme.text; font.pixelSize: 13; font.weight: Font.Medium; Layout.preferredWidth: 190 }
                Text { text: part.modelData.what; color: Theme.dim; font.pixelSize: 13; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Text { text: part.modelData.licence; color: Theme.faint; font.pixelSize: 12 }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                lineHeight: 1.35
                color: Theme.faint
                font.pixelSize: 12
                text: "Qt holds code of other projects. Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved."
                      + (backend.licenceFolder !== "" ? " The AppImage also carries system libraries; every part, its licence and its source are listed in THIRD-PARTY-APPIMAGE.txt beside the licence texts." : "")
            }
            ActionButton {
                visible: backend.licenceFolder !== ""
                kind: "quiet"
                icon: Theme.iconFolder
                text: "Licence texts"
                onClicked: backend.openFolder("licences")
            }
        }
        Divider {}
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            lineHeight: 1.35
            color: Theme.faint
            font.pixelSize: 12
            text: "Star Citizen is a trademark of Cloud Imperium Games; FidelityFX and FSR are AMD's; DLSS is NVIDIA's; PS5 is Sony Interactive Entertainment's; Claude and Claude Code are Anthropic's. Myrddin is an independent project: none of them is affiliated with it or endorses it. It owes its footing to the Star Citizen LUG, whose runners and Helper it builds on."
        }
    }
}
