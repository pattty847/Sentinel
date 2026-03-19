import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10

    property var target: null
    property bool enabled: true

    readonly property real maxLiquidityRange: root.target
        ? Math.max(1.0, root.target.heatmapMaxObservedLiquidity)
        : 1000.0

    signal valueChanged(var newValue)

    // Log scale: value = minVal * (maxRange/minVal)^ratio
    // _minVal is the actual observed data minimum — log scale spans real data range.
    // Leftmost dead zone (ratio < deadZone) maps to threshold=0 (off).
    readonly property real _minVal: {
        const obsMin = root.target ? root.target.heatmapMinObservedLiquidity : 0
        const obsMax = root.maxLiquidityRange
        // fallback: 1/1000th of max (before data arrives or if min==0)
        return obsMin > 0 ? obsMin : (obsMax > 0 ? obsMax / 1000.0 : 1e-6)
    }
    readonly property real _deadZone: 0.04   // ~6px at 150px width = "off" zone

    function valueToRatio(val) {
        if (val <= 0) return 0
        const logSpan = Math.log(root.maxLiquidityRange / root._minVal)
        if (logSpan <= 0) return root._deadZone
        const r = Math.log(Math.max(val, root._minVal) / root._minVal) / logSpan
        return root._deadZone + r * (1.0 - root._deadZone)
    }
    function ratioToValue(ratio) {
        if (ratio < root._deadZone) return 0
        const r = (ratio - root._deadZone) / (1.0 - root._deadZone)
        const logSpan = Math.log(root.maxLiquidityRange / root._minVal)
        return root._minVal * Math.exp(r * logSpan)
    }

    function fmtVal(v) {
        if (v <= 0)       return "off"
        if (v >= 1000000) return (v / 1000000).toFixed(2) + "M"
        if (v >= 1000)    return (v / 1000).toFixed(1) + "K"
        return v.toFixed(1)
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
        text: root.target
            ? "Min Liq: " + root.fmtVal(root.target.heatmapLiquidityThreshold)
            : "Min Liq: off"
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
        spacing: 8
        Text { text: "off";  color: "#00FF00"; font.pixelSize: 9 }
        Text {
            text: root.fmtVal(root.ratioToValue(root._deadZone + (1.0 - root._deadZone) * 0.33))
            color: "#00FF00"; font.pixelSize: 9
        }
        Text {
            text: root.fmtVal(root.ratioToValue(root._deadZone + (1.0 - root._deadZone) * 0.67))
            color: "#00FF00"; font.pixelSize: 9
        }
        Text {
            text: root.fmtVal(root.maxLiquidityRange)
            color: "#00FF00"; font.pixelSize: 9
        }
    }
}
