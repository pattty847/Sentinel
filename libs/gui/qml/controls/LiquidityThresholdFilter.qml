import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10

    property var target: null
    property bool enabled: true

    signal valueChanged(var newValue)

    // All range state computed directly from target — no public override surface.
    readonly property real _maxVal: root.target
        ? Math.max(1e-9, root.target.heatmapMaxObservedLiquidity)
        : 1e-9
    readonly property real _minVal: {
        const obsMin = root.target ? root.target.heatmapMinObservedLiquidity : 0
        return (obsMin > 0 && obsMin < _maxVal) ? obsMin : Math.max(1e-9, _maxVal / 1000.0)
    }

    // Log scale: value = _minVal * (_maxVal/_minVal)^r, r in [0,1]
    // Left dead zone (~4%) = off (threshold 0).
    readonly property real _deadZone: 0.04

    function valueToRatio(val) {
        if (val <= 0 || _maxVal <= _minVal) return 0
        const logSpan = Math.log(_maxVal / _minVal)
        if (logSpan <= 0) return 0
        const r = Math.log(Math.max(val, _minVal) / _minVal) / logSpan
        return _deadZone + Math.max(0, Math.min(1, r)) * (1.0 - _deadZone)
    }
    function ratioToValue(ratio) {
        if (ratio < _deadZone || _maxVal <= _minVal) return 0
        const r = (ratio - _deadZone) / (1.0 - _deadZone)
        const logSpan = Math.log(_maxVal / _minVal)
        return _minVal * Math.exp(Math.min(r, 1.0) * logSpan)
    }

    function fmtVal(v) {
        if (v <= 0)       return "off"
        if (v >= 1000000) return (v / 1000000).toFixed(2) + "M"
        if (v >= 1000)    return (v / 1000).toFixed(2) + "K"
        if (v >= 1)       return v.toFixed(3)
        return v.toExponential(2)
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
              + "  [" + root.fmtVal(root._minVal) + "–" + root.fmtVal(root._maxVal) + "]"
            : "Min Liq: off"
        color: "#00FF00"
        font.pixelSize: 10
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
                        const trackW = parent.parent.width - parent.width
                        const ratio = parent.x / trackW
                        const value = root.ratioToValue(ratio)
                        console.log("[LiqSlider] px=" + parent.x.toFixed(1)
                            + " ratio=" + ratio.toFixed(3)
                            + " => threshold=" + value.toFixed(6)
                            + " _minVal=" + root._minVal.toExponential(3)
                            + " _maxVal=" + root._maxVal.toFixed(4))
                        root.target.heatmapLiquidityThreshold = value
                        root.valueChanged(value)
                    }
                }
            }
        }
    }

    Row {
        spacing: 6
        Text { text: "off";                                   color: "#00FF00"; font.pixelSize: 9 }
        Text { text: root.fmtVal(root.ratioToValue(0.35));   color: "#00FF00"; font.pixelSize: 9 }
        Text { text: root.fmtVal(root.ratioToValue(0.65));   color: "#00FF00"; font.pixelSize: 9 }
        Text { text: root.fmtVal(root._maxVal);              color: "#00FF00"; font.pixelSize: 9 }
    }
}
