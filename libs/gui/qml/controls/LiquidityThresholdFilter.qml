import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10

    // Standard interface for all controls
    property var target: null  // UnifiedGridRenderer instance
    property bool enabled: true

    // Asset-aware liquidity scaling from parent context
    property real maxLiquidityRange: 1000.0  // Default, should be set by parent

    // Signals
    signal valueChanged(var newValue)

    Rectangle {
        width: 150
        height: 25
        color: Qt.rgba(0.1, 0.1, 0.1, 0.9)
        border.color: "lime"
        border.width: 1
        radius: 5
        Text {
            anchors.centerIn: parent
            color: "white"
            font.pixelSize: 12
            font.bold: true
            text: " LIQ THRESHOLD"
        }
    }

    Text {
        text: {
            if (!root.target) return "Min Liquidity: 0.00"
            return "Min Liquidity: " + root.target.heatmapLiquidityThreshold.toFixed(2)
        }
        color: "#00FF00"
        font.pixelSize: 11
        font.bold: true
    }

    Rectangle {
        id: sliderTrack
        width: 150
        height: 25
        color: Qt.rgba(0.12, 0.12, 0.12, 0.9)
        border.color: "lime"
        border.width: 1
        radius: 3
        layer.enabled: true
        layer.smooth: false

        Rectangle {
            id: thresholdSliderHandle
            width: 18
            height: parent.height - 4
            color: "#f8f8f8"
            border.color: "black"
            border.width: 1
            radius: 2
            anchors.verticalCenter: parent.verticalCenter
            z: 10
            opacity: 1.0
            x: 1 + Math.max(0, Math.min(sliderTrack.width - width - 2,
                root.target ? (root.target.heatmapLiquidityThreshold / root.maxLiquidityRange) * (parent.width - width - 2) : 0))
            layer.enabled: true
            layer.smooth: false

            MouseArea {
                anchors.fill: parent
                drag.target: parent
                drag.axis: Drag.XAxis
                drag.minimumX: 0
                drag.maximumX: sliderTrack.width - parent.width - 2
                enabled: root.enabled && root.target

                onPositionChanged: {
                    if (drag.active && root.target) {
                        var ratio = parent.x / (parent.parent.width - parent.width)
                        var value = ratio * root.maxLiquidityRange
                        root.target.heatmapLiquidityThreshold = value
                        root.valueChanged(value)
                    }
                }
            }
        }
    }

    Row {
        spacing: 15
        Text {
            text: "0"
            color: "#00FF00"
            font.pixelSize: 9
        }
        Text {
            text: (root.maxLiquidityRange / 2).toFixed(0)
            color: "#00FF00"
            font.pixelSize: 9
        }
        Text {
            text: root.maxLiquidityRange.toFixed(0)
            color: "#00FF00"
            font.pixelSize: 9
        }
    }
}
