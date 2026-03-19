import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10

    // Standard interface for all controls
    property var target: null  // UnifiedGridRenderer instance
    property bool enabled: true

    // Range auto-driven from observed data max; sqrt-scaled for sensitivity at low values
    readonly property real maxLiquidityRange: root.target
        ? Math.max(1.0, root.target.heatmapMaxObservedLiquidity)
        : 1000.0

    // Signals
    signal valueChanged(var newValue)

    // sqrt mapping helpers: slider pos → value uses quadratic, value → pos uses sqrt
    function valueToRatio(val) {
        return Math.sqrt(Math.max(0.0, val) / root.maxLiquidityRange)
    }
    function ratioToValue(ratio) {
        return ratio * ratio * root.maxLiquidityRange
    }

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
            const v = root.target.heatmapLiquidityThreshold
            return v >= 1000000 ? "Min Liq: " + (v / 1000000).toFixed(2) + "M"
                 : v >= 1000   ? "Min Liq: " + (v / 1000).toFixed(2) + "K"
                 : "Min Liq: " + v.toFixed(2)
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
            x: {
                if (!root.target) return 1
                const ratio = root.valueToRatio(root.target.heatmapLiquidityThreshold)
                return 1 + Math.max(0, Math.min(sliderTrack.width - width - 2,
                    ratio * (parent.width - width - 2)))
            }
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
                        const ratio = parent.x / (parent.parent.width - parent.width)
                        const value = root.ratioToValue(ratio)
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
            text: {
                const mid = root.ratioToValue(0.5)
                return mid >= 1000000 ? (mid / 1000000).toFixed(1) + "M"
                     : mid >= 1000   ? (mid / 1000).toFixed(1) + "K"
                     : mid.toFixed(0)
            }
            color: "#00FF00"
            font.pixelSize: 9
        }
        Text {
            text: {
                const mx = root.maxLiquidityRange
                return mx >= 1000000 ? (mx / 1000000).toFixed(1) + "M"
                     : mx >= 1000   ? (mx / 1000).toFixed(1) + "K"
                     : mx.toFixed(0)
            }
            color: "#00FF00"
            font.pixelSize: 9
        }
    }
}
