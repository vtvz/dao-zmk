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

    function colorFor(v) {
        if (v === null || v === undefined) return Kirigami.Theme.disabledTextColor
        if (v < 10) return Kirigami.Theme.negativeTextColor   // red below 10%
        if (v <= 30) return Kirigami.Theme.neutralTextColor   // amber when low
        return Kirigami.Theme.highlightColor
    }

    // A thin vertical bar that fills from the bottom by percentage.
    component FillBar: Item {
        id: bar
        property var pct: null
        property string tag: ""

        Rectangle {
            id: track
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: Math.max(1, width * 0.18)
            border.color: Kirigami.ColorUtils.linearInterpolation(
                Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, 0.35)

            Rectangle {
                id: fill
                anchors {
                    left: parent.left; right: parent.right; bottom: parent.bottom
                    margins: track.border.width + 1
                }
                readonly property real p: (bar.pct === null || bar.pct === undefined)
                    ? 0 : Math.max(0, Math.min(100, bar.pct)) / 100
                height: (parent.height - 2 * (track.border.width + 1)) * p
                radius: width / 2
                color: root.colorFor(bar.pct)
                Behavior on height { NumberAnimation { duration: 200 } }
            }
        }
    }

    // Compact unit: left bar, keyboard glyph, right bar — [L] ⌨ [R].
    component Gauge: RowLayout {
        id: gauge
        property real cell: Kirigami.Units.iconSizes.smallMedium
        spacing: Math.round(cell * 0.18)
        readonly property real barWidth: Math.max(4, gauge.cell * 0.42)

        FillBar {
            pct: root.leftPct; tag: "L"
            Layout.preferredWidth: gauge.barWidth
            Layout.preferredHeight: gauge.cell
            opacity: root.connected ? 1.0 : 0.45
        }
        Kirigami.Icon {
            source: "input-keyboard"
            Layout.preferredWidth: gauge.cell
            Layout.preferredHeight: gauge.cell
            opacity: root.connected ? 0.9 : 0.4
        }
        FillBar {
            pct: root.rightPct; tag: "R"
            Layout.preferredWidth: gauge.barWidth
            Layout.preferredHeight: gauge.cell
            opacity: root.connected ? 1.0 : 0.45
        }
    }

    // ---- Panel (compact) ----
    compactRepresentation: MouseArea {
        id: compact
        onClicked: root.expanded = !root.expanded

        readonly property bool horizontal: compact.height <= compact.width
        readonly property real thin: horizontal ? compact.height : compact.width

        Layout.preferredWidth: gaugeItem.implicitWidth
        Layout.preferredHeight: thin

        Gauge {
            id: gaugeItem
            anchors.centerIn: parent
            cell: compact.thin
        }
    }

    // ---- Popup (full) ----
    fullRepresentation: ColumnLayout {
        Layout.minimumWidth: Kirigami.Units.gridUnit * 14
        Layout.minimumHeight: Kirigami.Units.gridUnit * 9
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

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: Kirigami.Units.gridUnit * 3
            visible: root.connected

            Repeater {
                model: [
                    { tag: "Left",  pct: root.leftPct },
                    { tag: "Right", pct: root.rightPct }
                ]
                delegate: ColumnLayout {
                    id: half
                    required property var modelData
                    spacing: Kirigami.Units.smallSpacing
                    FillBar {
                        pct: half.modelData.pct
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 1.2
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                    }
                    PlasmaComponents.Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: root.fmt(half.modelData.pct)
                        font.bold: true
                    }
                    PlasmaComponents.Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: half.modelData.tag
                        opacity: 0.75
                    }
                }
            }
        }
    }
}
