import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10

    // Standard interface for all controls
    property var target: null  // UnifiedGridRenderer instance
    property bool enabled: true
    property int currentTimeframe: target ? target.timeframeMs : 1000

    // Signals
    signal valueChanged(var newValue)

    // Update current timeframe when target changes
    Connections {
        target: root.target
        function onTimeframeChanged() {
            root.currentTimeframe = root.target.timeframeMs
        }
    }

    Text {
        text: "Timeframe"
        color: "white"
        font.pixelSize: 12
    }

    // All timeframes match what the server pre-builds and the TopToolbar
    // combo: 1s, 1m, 5m, 15m, 1h, 4h, 1D.

    // Row 1: 1s  1m  5m  15m
    Row {
        spacing: 3

        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 1000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === 1000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "1s"; font.bold: root.currentTimeframe === 1000 }
            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: { if (root.target) { root.target.setTimeframe(1000); root.valueChanged(1000) } }
            }
        }

        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 60000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === 60000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "1m"; font.bold: root.currentTimeframe === 60000 }
            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: { if (root.target) { root.target.setTimeframe(60000); root.valueChanged(60000) } }
            }
        }

        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 300000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === 300000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "5m"; font.bold: root.currentTimeframe === 300000 }
            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: { if (root.target) { root.target.setTimeframe(300000); root.valueChanged(300000) } }
            }
        }

        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 900000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === 900000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "15m"; font.bold: root.currentTimeframe === 900000 }
            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: { if (root.target) { root.target.setTimeframe(900000); root.valueChanged(900000) } }
            }
        }
    }

    // Row 2: 1h  4h  1D
    Row {
        spacing: 3

        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 3600000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === 3600000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "1h"; font.bold: root.currentTimeframe === 3600000 }
            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: { if (root.target) { root.target.setTimeframe(3600000); root.valueChanged(3600000) } }
            }
        }

        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 14400000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === 14400000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "4h"; font.bold: root.currentTimeframe === 14400000 }
            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: { if (root.target) { root.target.setTimeframe(14400000); root.valueChanged(14400000) } }
            }
        }

        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 86400000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === 86400000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "1D"; font.bold: root.currentTimeframe === 86400000 }
            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: { if (root.target) { root.target.setTimeframe(86400000); root.valueChanged(86400000) } }
            }
        }
    }
}
