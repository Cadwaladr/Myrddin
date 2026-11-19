import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

PageView {
    id: page
    signal go(string page)
    title: "Setup"
    subtitle: "Where the game is, and whether this system has what a launch needs."

    FolderDialog {
        id: folders
        title: "The folder Star Citizen is installed in"
        onAccepted: backend.conf.prefix = backend.fullPath(selectedFolder.toString())
    }
    FileDialog {
        id: scripts
        title: "The script that starts the game"
        onAccepted: backend.conf.launchScript = backend.fullPath(selectedFile.toString())
    }

    Card {
        heading: "Where the game is"
        note: "The Wine prefix Star Citizen is installed in: the folder that holds sc-launch.sh. The LUG Helper puts it in ~/Games/star-citizen unless told otherwise."

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Field {
                id: prefixField
                Layout.fillWidth: true
                placeholderText: "~/Games/star-citizen"
                text: backend.tidyPath(backend.conf.prefix)
                valid: text === "" || backend.looksLikePrefix(text)
                onEditingFinished: {
                    backend.conf.prefix = backend.fullPath(text)
                    text = Qt.binding(() => backend.tidyPath(backend.conf.prefix))
                }
            }
            ActionButton { text: "Browse"; icon: Theme.iconFolder; onClicked: folders.open() }
            ActionButton { text: "Find it"; icon: Theme.iconSearch; onClicked: backend.applyFix("prefix") }
        }
        Text {
            visible: !prefixField.valid
            text: "That folder has neither sc-launch.sh nor a drive_c in it."
            color: Theme.bad
            font.pixelSize: 13
        }
        Divider {}
        SettingRow {
            title: "Launch script"
            detail: backend.tidyPath(backend.launchScriptInUse)
            ActionButton {
                text: backend.conf.launchScript === "" ? "Use another" : "Back to the default"
                onClicked: backend.conf.launchScript === "" ? scripts.open() : backend.conf.launchScript = ""
            }
        }
    }

    Card {
        heading: "System check"
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: -44
            Item { Layout.fillWidth: true }
            ActionButton { kind: "quiet"; icon: Theme.iconRefresh; busy: backend.checking; text: "Check again"; onClicked: backend.refreshChecks() }
        }
        Repeater {
            model: backend.checks
            CheckRow {
                required property var modelData
                item: modelData
                onFix: (key) => backend.applyFix(key)
                onGo: (name) => page.go(name)
            }
        }
    }

    Card {
        heading: "This program"
        SettingRow {
            title: "In the application menu"
            detail: "An entry for Myrddin, wherever this file lies."
            Toggle { checked: backend.menuEntry; onToggled: (v) => backend.setMenuEntry(v) }
        }
        Divider {}
        SettingRow {
            title: "\"myrddin\" in the terminal"
            detail: backend.cliNote !== "" ? backend.cliNote : "A link in ~/.local/bin, so that \"myrddin fg 4\" works from any terminal. Every sc-turbo command goes through it."
            Toggle { checked: backend.cliLink; onToggled: (v) => backend.setCliLink(v) }
        }
        Divider {}
        SettingRow {
            title: "Close this window after a launch"
            detail: "The game does not need it; the settings can be changed from a terminal too."
            Toggle { checked: backend.prefs.closeAfterLaunch; onToggled: (v) => backend.prefs.closeAfterLaunch = v }
        }
        Divider {}
        SettingRow {
            title: "Myrddin's files"
            detail: backend.tidyPath(backend.runtimeDir) + "\n" + backend.runtimeVersion
            ActionButton { text: "Open"; icon: Theme.iconFolder; onClicked: backend.openFolder("runtime") }
        }
    }
}
