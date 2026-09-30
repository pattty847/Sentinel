import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Sentinel.Lab 1.0

ApplicationWindow {
    id: root
    visible: true
    width: 1500
    height: 940
    minimumWidth: 900
    minimumHeight: 600
    title: "Sentinel · GPU heatmap lab (HeatmapRenderNode)"
    color: "#070b10"
    property var metrics: ({})
    property var frameHistory: []
    property string screenshotNotice: ""
    readonly property var hysteresisPresets: [0, 0.15, 0.25, 0.4]
    readonly property var minRowPresets: [1, 1.5, 2, 3, 4]
    readonly property var prepModes: ["full", "viewport", "whole-chunk", "whole-chunk-cpu", "hybrid"]
    function money(v) { return "$" + Number(v).toString() }
    function syncTickControls() {
        tickMode.currentIndex = binLab.manualMode ? 1 : 0
        manualPreset.currentIndex = binLab.offeredTicks.indexOf(binLab.manualTick)
        hysteresisBox.currentIndex = hysteresisPresets.indexOf(binLab.hysteresis)
        minRowBox.currentIndex = minRowPresets.indexOf(binLab.minRowPx)
        crossfadeBox.checked = binLab.crossfade
    }

    Component.onCompleted: {
        binLab.prepMode = initialPrep
        prepBox.currentIndex = prepModes.indexOf(initialPrep)
        binLab.timeframeMinutes = initialTf
        syncTickControls()
        var presets = [1, 5, 15, 60, 240, 1440]
        timeframe.currentIndex = presets.indexOf(initialTf)
        if (timeframe.currentIndex < 0) {
            timeframe.currentIndex = 6
            customTf.text = String(initialTf)
        }
        if (initialSynthetic > 0) {
            source.currentIndex = initialSynthetic >= 10000000 ? 2 : 1
            binLab.loadSynthetic(initialSynthetic)
        } else {
            hours.value = initialHours
            layerBox.currentIndex = initialLayer === "deep" ? 1 : 0
            binLab.loadReal(initialHours, initialLayer)
        }
    }
    Timer {
        interval: 250
        repeat: true
        running: true
        onTriggered: {
            root.metrics = binLab.metrics()
            var values = root.frameHistory.slice()
            values.push(root.metrics.frameMs || 0)
            if (values.length > 90) values.shift()
            root.frameHistory = values
            graph.requestPaint()
        }
    }

    Shortcut {
        sequence: "S"
        onActivated: root.screenshotNotice = binLab.saveScreenshot("") ?
            "Screenshot saved to screenshots/" : "Screenshot failed"
    }
    Connections {
        target: binLab
        function onTimeframeChanged() {
            var preset = [1, 5, 15, 60, 240, 1440].indexOf(binLab.timeframeMinutes)
            timeframe.currentIndex = preset >= 0 ? preset : 6
            if (preset < 0) customTf.text = String(binLab.timeframeMinutes)
        }
        function onTickChanged() { root.syncTickControls() }
        function onPresetsChanged() { root.syncTickControls() }
    }

    component ChartCell: Rectangle {
            id: cell
            property alias lab: cellLab
            property string labName: ""
            property var cellMetrics: ({})
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#080d12"
            border.color: "#2c3d49"
            BinLab { id: cellLab; objectName: cell.labName; anchors.fill: parent }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                property real oldX: 0
                property real oldY: 0
                onPressed: function(mouse) { oldX = mouse.x; oldY = mouse.y }
                onPositionChanged: function(mouse) {
                    if (pressed) {
                        cellLab.pan(mouse.x - oldX, mouse.y - oldY)
                        oldX = mouse.x; oldY = mouse.y
                    }
                }
                onWheel: function(wheel) {
                    cellLab.wheelZoom(wheel.angleDelta.x, wheel.angleDelta.y,
                                     (wheel.modifiers & Qt.ShiftModifier) !== 0,
                                     wheel.x / width, wheel.y / height)
                    wheel.accepted = true
                }
            }
            Rectangle {
                anchors.fill: parent
                visible: cellLab.status.startsWith("Timeframe unavailable:")
                color: "#080d12"
                Label {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 40, 520)
                    text: cellLab.status
                    color: "#e2b5a6"
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
            Rectangle {
                anchors.left: parent.left; anchors.bottom: parent.bottom
                anchors.margins: 14
                width: hint.implicitWidth + 20; height: hint.implicitHeight + 12
                color: "#ba101820"
                Label { id: hint; anchors.centerIn: parent; text: "DRAG  pan     WHEEL  time + price     SHIFT + WHEEL  price     S  screenshot"; color: "#9fb4bf"; font.pixelSize: 12 }
            }
            // Resolution indicator (Manual): columns that cannot build the locked tick veil.
            Rectangle {
                anchors.left: parent.left; anchors.top: parent.top
                anchors.margins: 14
                visible: (cell.cellMetrics.indicator || "") !== ""
                width: Math.min(parent.width - 28, indicator.implicitWidth + 20); height: indicator.implicitHeight + 12
                color: "#dd1c1408"
                border.color: "#f0b46a"
                Label {
                    id: indicator
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, parent.parent.width - 48)
                    text: cell.cellMetrics.indicator || ""
                    color: "#f0b46a"; font.pixelSize: 13; wrapMode: Text.WordWrap
                }
            }
            Label {
                anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10
                text: cellLab.timeframeMinutes + "m · " + cellLab.prepMode + " · " +
                      ((cell.cellMetrics.gpuBytes || 0) / 1048576).toFixed(1) + " MB GPU"
                color: "#a6e7e9"; font.family: "Menlo"; font.pixelSize: 12
            }
        }

    readonly property var binLab: mainCell.lab
    function forEachChart(f) {
        f(mainCell.lab)
        for (var i = 0; i < extraCharts.count; ++i) f(extraCharts.itemAt(i).lab)
    }

    header: Rectangle {
        height: 112
        color: "#101820"
        border.color: "#243442"
        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                Label { text: "GPU BIN LAB"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 16; Layout.rightMargin: 16 }
                ComboBox {
                    id: source
                    model: ["Real HMC2", "Synthetic 1M", "Synthetic 10M"]
                    Layout.preferredWidth: 170
                }
                Label { text: "Hours"; color: "#aab7c0"; visible: source.currentIndex === 0 }
                SpinBox { id: hours; from: 1; to: 720; value: 24; visible: source.currentIndex === 0; Layout.preferredWidth: 95 }
                ComboBox { id: layerBox; model: ["near", "deep"]; visible: source.currentIndex === 0; Layout.preferredWidth: 90 }
                Button {
                    text: "Load"
                    onClicked: {
                        if (source.currentIndex === 0) root.forEachChart(function(c) { c.loadReal(hours.value, layerBox.currentText) })
                        else binLab.loadSynthetic(source.currentIndex === 1 ? 1000000 : 10000000)
                    }
                }
                Label { text: "Prep"; color: "#aab7c0"; Layout.leftMargin: 10 }
                ComboBox {
                    id: prepBox
                    model: ["full (S4 lab)", "viewport (V)", "whole-chunk (W)", "whole-chunk CPU (W)", "hybrid (resident chunks)"]
                    Layout.preferredWidth: 190
                    ToolTip.visible: hovered
                    ToolTip.text: "B1: viewport = source clipped to the view +-1 view, screen grid; whole-chunk = 64-column tiles binned over their whole price extent (GPU or CPU), pan/zoom = mapping only; hybrid = the tiles' sources stay on the GPU and each tile bins the rows around the view"
                    onActivated: {
                        var mode = root.prepModes[currentIndex]
                        root.forEachChart(function(c) { c.prepMode = mode })
                    }
                }
                Label { text: binLab.status; color: "#b9c9d2"; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: "Timeframe"; color: "#aab7c0" }
                ComboBox {
                    id: timeframe
                    model: ["1m", "5m", "15m", "1h", "4h", "1D", "Custom"]
                    Layout.preferredWidth: 100
                    onActivated: {
                        if (currentIndex < 6) binLab.timeframeMinutes = [1, 5, 15, 60, 240, 1440][currentIndex]
                        else customTf.forceActiveFocus()
                    }
                }
                TextField {
                    id: customTf
                    Layout.preferredWidth: 52
                    placeholderText: "16"
                    validator: IntValidator { bottom: 1; top: 1440 }
                    onEditingFinished: {
                        if (acceptableInput) {
                            binLab.timeframeMinutes = Number(text)
                            timeframe.currentIndex = 6
                        }
                    }
                }
                Label { text: "Tick"; color: "#aab7c0"; Layout.leftMargin: 10 }
                ComboBox {
                    id: tickMode
                    model: ["Auto", "Manual"]
                    Layout.preferredWidth: 100
                    onActivated: binLab.manualMode = currentIndex === 1
                }
                ComboBox {
                    id: manualPreset
                    model: binLab.offeredTicks.map(function(v) { return root.money(v) })
                    displayText: binLab.manualMode ? root.money(binLab.manualTick) : "preset"
                    Layout.preferredWidth: 90
                    onActivated: binLab.manualTick = binLab.offeredTicks[currentIndex]
                }
                Label { text: "h"; color: "#aab7c0" }
                ComboBox {
                    id: hysteresisBox
                    model: root.hysteresisPresets.map(function(v) { return String(v) })
                    Layout.preferredWidth: 80
                    enabled: !binLab.manualMode
                    onActivated: binLab.hysteresis = root.hysteresisPresets[currentIndex]
                }
                Label { text: "Min row px"; color: "#aab7c0" }
                ComboBox {
                    id: minRowBox
                    model: root.minRowPresets.map(function(v) { return String(v) })
                    Layout.preferredWidth: 70
                    enabled: !binLab.manualMode
                    onActivated: binLab.minRowPx = root.minRowPresets[currentIndex]
                }
                CheckBox {
                    id: crossfadeBox
                    text: "Crossfade 150 ms"
                    onToggled: binLab.crossfade = checked
                }
                Button { text: "Screenshot  S"; onClicked: root.screenshotNotice =
                             binLab.saveScreenshot("") ? "Screenshot saved" : "Screenshot failed" }
                Label { text: root.screenshotNotice; color: "#a6e7e9"; Layout.fillWidth: true; elide: Text.ElideRight }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12
        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: chartCount > 1 ? 2 : 1
            rowSpacing: 6
            columnSpacing: 6
            ChartCell { id: mainCell; labName: "binLab"; cellMetrics: root.metrics }
            Repeater {
                id: extraCharts
                model: Math.max(0, chartCount - 1)
                delegate: ChartCell {
                    labName: "extraLab" + index
                    Component.onCompleted: {
                        lab.timeframeMinutes = [5, 60, 15][index]
                        lab.prepMode = initialPrep
                        lab.loadReal(initialHours, initialLayer) // controls are not complete yet
                    }
                    Timer {
                        interval: 500; repeat: true; running: true
                        onTriggered: parent.cellMetrics = parent.lab.metrics()
                    }
                }
            }
        }
        Rectangle {
            Layout.preferredWidth: 304
            Layout.fillHeight: true
            color: "#111a23"
            border.color: "#2c3d49"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 3
                Label { text: "RENDER TELEMETRY"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 14 }
                Label { text: "Frame time · last 90 samples"; color: "#aab7c0"; font.pixelSize: 12 }
                Canvas {
                    id: graph
                    Layout.fillWidth: true
                    Layout.preferredHeight: 106
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.reset(); ctx.fillStyle = "#0a1118"; ctx.fillRect(0, 0, width, height)
                        ctx.strokeStyle = "#263b48"; ctx.beginPath()
                        ctx.moveTo(0, height * 0.5); ctx.lineTo(width, height * 0.5); ctx.stroke()
                        if (root.frameHistory.length < 2) return
                        ctx.strokeStyle = "#5dd4d8"; ctx.lineWidth = 1.5; ctx.beginPath()
                        for (var i = 0; i < root.frameHistory.length; ++i) {
                            var x = i * width / 89
                            var y = height - Math.min(root.frameHistory[i], 33.3) / 33.3 * height
                            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                        }
                        ctx.stroke()
                    }
                }
                Label { text: "TICK (E1-E3)"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 12 }
                Repeater {
                    model: [
                        ["Mode", "mode", ""], ["Tick", "tick", " $"], ["h", "hysteresis", ""],
                        ["Min row px", "minRowPx", ""], ["Row height", "rowPx", " px"],
                        ["commonTick (view)", "commonTick", " $"], ["Re-bin CPU submit", "binSubmitMs", " ms"],
                        ["Tick-change CPU submit", "tickChangeBinMs", " ms"], ["Bins since start", "rebins", ""],
                        ["Tick changes", "tickChanges", ""], ["Crossfade", "crossfadeMs", " ms"]
                    ]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        Label { text: modelData[0]; color: "#9bafba"; Layout.fillWidth: true; font.pixelSize: 12 }
                        Label {
                            text: {
                                var value = root.metrics[modelData[1]]
                                if (value === undefined) return "—"
                                return typeof value === "number" ? value.toFixed(value % 1 === 0 ? 0 : 3) + modelData[2] : String(value)
                            }
                            color: "#e8f0f2"; font.family: "Menlo"; font.pixelSize: 12
                        }
                    }
                }
                Label {
                    visible: (root.metrics.indicator || "") !== ""
                    text: root.metrics.indicator || ""
                    color: "#f0b46a"; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 11
                }
                Label { text: "PREP (B1)"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 12 }
                Repeater {
                    model: [
                        ["Prep mode", "prep", "", 0], ["GPU memory", "gpuBytes", " MB", 1],
                        ["Prep CPU (source/cells)", "prepCpuBytes", " MB", 1], ["Decoded chunks", "chunkBytes", " MB", 1],
                        ["Tile intermediates", "interBytes", " MB", 1], ["Process footprint", "footprintBytes", " MB", 1],
                        ["Last prep", "lastPrepMs", " ms", 0], ["Prep builds", "prepBuilds", "", 0],
                        ["Tiles kept / resident", "tiles", "", 0], ["Chunk hits", "chunkHits", "", 0],
                        ["Chunk misses", "chunkMisses", "", 0], ["Chunk decodes", "chunkLoads", "", 0]
                    ]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        visible: root.metrics[modelData[1]] !== undefined
                        Label { text: modelData[0]; color: "#9bafba"; Layout.fillWidth: true; font.pixelSize: 12 }
                        Label {
                            text: {
                                var value = root.metrics[modelData[1]]
                                if (value === undefined) return "—"
                                if (modelData[3] === 1) return (value / 1048576).toFixed(1) + modelData[2]
                                if (modelData[1] === "tiles") return value + " / " + (root.metrics.tilesResident || 0)
                                return typeof value === "number" ? value.toFixed(value % 1 === 0 ? 0 : 1) + modelData[2] : String(value)
                            }
                            color: "#e8f0f2"; font.family: "Menlo"; font.pixelSize: 12
                        }
                    }
                }
                Label { text: "RENDER"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 12 }
                Repeater {
                    model: [
                        ["FPS", "fps", ""], ["Frame", "frameMs", " ms"],
                        ["GPU frame (all passes)", "gpuFrameMs", " ms"],
                        ["Loaded entries", "entries", ""], ["GPU buffers", "gpuBytes", " bytes"],
                        ["Ticks / bin", "group", ""],
                        ["Grid", "columns", " cols"], ["Rows", "rows", ""],
                        ["Load", "loadMs", " ms"], ["Compose", "composeMs", " ms"], ["GPU source build", "buildMs", " ms"],
                        ["First painted", "firstFrameMs", " ms"]
                    ]
                    delegate: RowLayout {
                        Layout.fillWidth: true
                        Label { text: modelData[0]; color: "#9bafba"; Layout.fillWidth: true; font.pixelSize: 12 }
                        Label {
                            text: {
                                var value = root.metrics[modelData[1]]
                                if (value === undefined) return "—"
                                return typeof value === "number" && value < 10000 ? value.toFixed(2) + modelData[2] : value.toLocaleString() + modelData[2]
                            }
                            color: "#e8f0f2"; font.family: "Menlo"; font.pixelSize: 12
                        }
                    }
                }
                Item { Layout.fillHeight: true }
                Label {
                    text: "A column is exactly the timeframe: zoom never changes it; time zoom-out stops at 1 column/px. Auto tick: smallest preset >= min row px, hysteresis h. Manual: locked preset, zoom only scales, price zoom-out stops at 1 row/px, remembered per symbol + timeframe; history that cannot build it is veiled, never coarsened. Grey veil: unproven or unbuildable. Blue hatch: not loaded."
                    color: "#8198a6"; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 11
                }
            }
        }
    }
}
