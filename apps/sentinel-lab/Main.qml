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
    title: "Sentinel · GPU heatmap lab (production path)"
    color: "#070b10"
    property var metrics: ({})
    property var frameHistory: []
    property string screenshotNotice: ""
    readonly property var hysteresisPresets: [0, 0.15, 0.25, 0.4]
    readonly property var minRowPresets: [1, 1.5, 2, 3, 4]
    function money(v) { return "$" + Number(v).toString() }
    function mb(v) { return ((v || 0) / 1048576).toFixed(1) + " MB" }
    function syncTickControls() {
        tickMode.currentIndex = binLab.manualMode ? 1 : 0
        manualPreset.currentIndex = binLab.offeredTicks.indexOf(binLab.manualTick)
        hysteresisBox.currentIndex = hysteresisPresets.indexOf(binLab.hysteresis)
        minRowBox.currentIndex = minRowPresets.indexOf(binLab.minRowPx)
        crossfadeBox.checked = binLab.crossfade
        bandBox.checked = binLab.showBandEdges
    }

    Component.onCompleted: {
        binLab.timeframeMinutes = initialTf
        binLab.showBandEdges = initialBandEdges
        syncTickControls()
        var presets = [1, 5, 15, 60, 240, 1440]
        timeframe.currentIndex = presets.indexOf(initialTf)
        if (timeframe.currentIndex < 0) {
            timeframe.currentIndex = 6
            customTf.text = String(initialTf)
        }
        hours.value = initialHours
        binLab.loadReal(initialHours)
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
            // E4: the finest source's coverage band (the near band) in view.
            Canvas {
                id: bandCanvas
                anchors.fill: parent
                visible: cellLab.showBandEdges
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    if (!cellLab.showBandEdges) return
                    ctx.strokeStyle = "#ffd84a"
                    ctx.lineWidth = 1.5
                    ctx.setLineDash([6, 4])
                    var edges = cellLab.bandEdges()
                    ctx.beginPath()
                    for (var i = 0; i < edges.length; ++i) {
                        var e = edges[i]
                        ctx.moveTo(e[0], e[2]); ctx.lineTo(e[1], e[2])
                        ctx.moveTo(e[0], e[3]); ctx.lineTo(e[1], e[3])
                    }
                    ctx.stroke()
                }
                Connections {
                    target: cellLab
                    function onBandEdgesChanged() { bandCanvas.requestPaint() }
                }
            }
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
            // Resolution indicator: rows in view no source builds at the tick veil.
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
                text: cellLab.timeframeMinutes + "m · " + root.money(cell.cellMetrics.tick || 0) + " · " +
                      root.mb(cell.cellMetrics.residentBytes) + " resident"
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
                Label { text: "GPU HEATMAP LAB"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 16; Layout.rightMargin: 16 }
                Label { text: "Hours"; color: "#aab7c0" }
                SpinBox { id: hours; from: 1; to: 720; value: 24; Layout.preferredWidth: 95 }
                Button { text: "Load"; onClicked: root.forEachChart(function(c) { c.loadReal(hours.value) }) }
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
                CheckBox {
                    id: bandBox
                    text: "Band edges (E4)"
                    onToggled: root.forEachChart(function(c) { c.showBandEdges = bandBox.checked })
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
                        lab.showBandEdges = initialBandEdges
                        lab.loadReal(initialHours) // controls are not complete yet
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
            Flickable {
                anchors.fill: parent
                anchors.margins: 14
                contentHeight: panel.implicitHeight
                clip: true
                ColumnLayout {
                    id: panel
                    width: parent.width
                    spacing: 3
                    component StatRow: RowLayout {
                        property string name
                        property string value
                        Layout.fillWidth: true
                        Label { text: parent.name; color: "#9bafba"; Layout.fillWidth: true; font.pixelSize: 12 }
                        Label { text: parent.value; color: "#e8f0f2"; font.family: "Menlo"; font.pixelSize: 12 }
                    }
                    Label { text: "RENDER TELEMETRY"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 14 }
                    Label { text: "Frame time · last 90 samples"; color: "#aab7c0"; font.pixelSize: 12 }
                    Canvas {
                        id: graph
                        Layout.fillWidth: true
                        Layout.preferredHeight: 90
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
                    Label { text: "TICK"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 12 }
                    StatRow { name: "Mode"; value: root.metrics.mode || "—" }
                    StatRow { name: "Tick"; value: root.money(root.metrics.tick || 0) }
                    StatRow { name: "h"; value: String(root.metrics.hysteresis) }
                    StatRow { name: "Min row px"; value: String(root.metrics.minRowPx) }
                    StatRow { name: "Row height"; value: (root.metrics.rowPx || 0).toFixed(2) + " px" }
                    StatRow { name: "Finest tick in view"; value: root.money(root.metrics.commonTick || 0) }
                    StatRow { name: "Last re-bin (CPU submit)"; value: (root.metrics.lastBinMs || 0).toFixed(3) + " ms" }
                    StatRow { name: "Tick changes"; value: String(root.metrics.tickChanges || 0) }
                    StatRow { name: "Crossfade"; value: (root.metrics.crossfadeMs || 0) + " ms" + (root.metrics.crossfading ? " (fading)" : "") }
                    StatRow { name: "Holding old picture"; value: root.metrics.holding ? "yes" : "no" }
                    Label {
                        visible: (root.metrics.indicator || "") !== ""
                        text: root.metrics.indicator || ""
                        color: "#f0b46a"; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 11
                    }
                    Label { text: "GPU (NODE)"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 12 }
                    StatRow { name: "Resident (cap 320 MB)"; value: root.mb(root.metrics.residentBytes) }
                    StatRow { name: "Sources / bins"; value: root.mb(root.metrics.sourceBytes) + " / " + root.mb(root.metrics.binBytes) }
                    StatRow { name: "Resident sources"; value: String(root.metrics.residentSources || 0) }
                    StatRow { name: "Uploads / passes / fills"; value: (root.metrics.sourcesUploaded || 0) + " / " + (root.metrics.binPasses || 0) + " / " + (root.metrics.fillPasses || 0) }
                    StatRow { name: "Evictions / missing"; value: (root.metrics.evictions || 0) + " / " + (root.metrics.missingReports || 0) }
                    StatRow { name: "Slots ready/fallback/partial"; value: (root.metrics.readySlots || 0) + "/" + (root.metrics.fallbackSlots || 0) + "/" + (root.metrics.partialSlots || 0) }
                    StatRow { name: "Loading slots"; value: String(root.metrics.loadingSlots || 0) }
                    StatRow { name: "Refused spans (CPU ceiling)"; value: (root.metrics.refusedSpans || 0) + " · " + root.mb(root.metrics.refusedBytes) }
                    Label { text: "DATA (CONTROLLER)"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 12 }
                    StatRow { name: "Decoded chunks"; value: root.mb(root.metrics.chunkBytes) + " · " + (root.metrics.chunkEntries || 0) }
                    StatRow { name: "Chunk decodes / fetched"; value: (root.metrics.chunkLoads || 0) + " / " + (root.metrics.fetchedChunks || 0) }
                    StatRow { name: "Span builds / cache hits"; value: (root.metrics.spanBuilds || 0) + " / " + (root.metrics.spanCacheHits || 0) }
                    StatRow { name: "Span images alive"; value: root.mb(root.metrics.spanLiveBytes) }
                    StatRow { name: "CPU committed (1 GiB)"; value: root.mb(root.metrics.cpuCommittedBytes) }
                    StatRow { name: "Snapshots published"; value: String(root.metrics.publications || 0) }
                    StatRow { name: "Process footprint"; value: root.mb(root.metrics.footprintBytes) }
                    Label { text: "RENDER"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 12 }
                    StatRow { name: "FPS"; value: (root.metrics.fps || 0).toFixed(1) }
                    StatRow { name: "Frame"; value: (root.metrics.frameMs || 0).toFixed(2) + " ms" }
                    StatRow { name: "GPU frame (all passes)"; value: (root.metrics.gpuFrameMs || 0).toFixed(2) + " ms" }
                    StatRow { name: "Node prepare"; value: (root.metrics.prepareMs || 0).toFixed(2) + " ms" }
                    StatRow { name: "Kernel"; value: root.metrics.kernel || "—" }
                    StatRow { name: "First painted"; value: (root.metrics.firstFrameMs || 0).toFixed(0) + " ms" }
                    Label {
                        text: "A column is exactly the timeframe: zoom never changes it; time zoom-out stops at 1 column/px. Auto tick: smallest preset >= min row px that every row in view can build, hysteresis h. Manual: locked preset, zoom only scales, price zoom-out stops at 1 row/px, remembered per symbol + timeframe; rows no source builds are veiled, never coarsened. The coarsest source bins first; finer sources fill only its veil. Grey veil: unproven or unbuildable. Blue hatch: not loaded (or refused by the CPU ceiling). Yellow dashes: the near band edge (E4)."
                        color: "#8198a6"; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 11
                    }
                }
            }
        }
    }
}
