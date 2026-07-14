import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.plasma5support as P5Support
import org.kde.kirigami as Kirigami

PlasmoidItem {
    id: root

    property bool connected: false
    property var leftPct: null
    property var rightPct: null

    // plasmashell disables file:// XHR, so read the state file the standard
    // plasmoid way: an executable DataSource that cats it. The reader writes
    // ~/.local/state/dao-battery.json (host/reader/dao-battery-reader.py).
    readonly property string readCmd:
        "cat \"$HOME/.local/state/dao-battery.json\" 2>/dev/null"

    P5Support.DataSource {
        id: executable
        engine: "executable"
        connectedSources: []
        onNewData: function(source, data) {
            disconnectSource(source) // one-shot per poll
            var out = (data["stdout"] || "").trim()
            if (!out) { root.connected = false; return }
            try {
                var s = JSON.parse(out)
                root.connected = !!s.connected
                root.leftPct = (s.left === null || s.left === undefined) ? null : s.left
                root.rightPct = (s.right === null || s.right === undefined) ? null : s.right
            } catch (e) {
                root.connected = false
            }
        }
    }

    function poll() {
        // reconnect triggers a fresh run of the command
        executable.connectSource(root.readCmd)
    }

    Timer {
        interval: 3000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.poll()
    }

    function fmt(v) {
        return (v === null || v === undefined) ? "—" : (v + "%")
    }

    function iconFor(v) {
        if (v === null || v === undefined) return "battery-missing"
        var lvl = Math.round(v / 10) * 10
        if (lvl > 100) lvl = 100
        if (lvl < 0) lvl = 0
        return "battery-" + (lvl < 10 ? "0" : "") + lvl
    }

    // ---- Panel (compact) ----
    compactRepresentation: MouseArea {
        Layout.minimumWidth: compactRow.implicitWidth + Kirigami.Units.smallSpacing * 2
        onClicked: root.expanded = !root.expanded

        RowLayout {
            id: compactRow
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                spacing: 2
                Kirigami.Icon {
                    source: root.iconFor(root.leftPct)
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    opacity: root.connected ? 1.0 : 0.4
                }
                PlasmaComponents.Label {
                    text: "L " + root.fmt(root.leftPct)
                    opacity: root.connected ? 1.0 : 0.4
                }
            }
            RowLayout {
                spacing: 2
                Kirigami.Icon {
                    source: root.iconFor(root.rightPct)
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    opacity: root.connected ? 1.0 : 0.4
                }
                PlasmaComponents.Label {
                    text: "R " + root.fmt(root.rightPct)
                    opacity: root.connected ? 1.0 : 0.4
                }
            }
        }
    }

    // ---- Popup (full) ----
    fullRepresentation: ColumnLayout {
        Layout.minimumWidth: Kirigami.Units.gridUnit * 14
        Layout.minimumHeight: Kirigami.Units.gridUnit * 8
        spacing: Kirigami.Units.largeSpacing

        PlasmaComponents.Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            font.bold: true
            text: "Dao Keyboard Battery"
        }

        PlasmaComponents.Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            visible: !root.connected
            opacity: 0.7
            text: "Dongle not connected"
        }

        GridLayout {
            Layout.alignment: Qt.AlignHCenter
            columns: 3
            rowSpacing: Kirigami.Units.largeSpacing
            columnSpacing: Kirigami.Units.largeSpacing
            visible: root.connected

            Kirigami.Icon {
                source: root.iconFor(root.leftPct)
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
            }
            PlasmaComponents.Label { text: "Left" }
            PlasmaComponents.Label { text: root.fmt(root.leftPct); font.bold: true }

            Kirigami.Icon {
                source: root.iconFor(root.rightPct)
                Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                Layout.preferredHeight: Kirigami.Units.iconSizes.medium
            }
            PlasmaComponents.Label { text: "Right" }
            PlasmaComponents.Label { text: root.fmt(root.rightPct); font.bold: true }
        }
    }
}
