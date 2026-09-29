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
    title: "Sentinel · GPU bin lab"
    color: "#070b10"
    property var metrics: ({})
    property var frameHistory: []

    Component.onCompleted: {
        if (initialSynthetic > 0) {
            source.currentIndex = initialSynthetic >= 100000000 ? 3 : initialSynthetic >= 10000000 ? 2 : 1
            binLab.loadSynthetic(initialSynthetic)
        } else {
            hours.value = initialHours
            layer.currentIndex = initialLayer === "deep" ? 1 : 0
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

    header: Rectangle {
        height: 64
        color: "#101820"
        border.color: "#243442"
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 18
            spacing: 12
            Label { text: "GPU BIN LAB"; color: "#a6e7e9"; font.bold: true; font.pixelSize: 16; Layout.rightMargin: 16 }
            ComboBox {
                id: source
                model: ["Real HMC2", "Synthetic 1M", "Synthetic 10M", "Synthetic 100M"]
                Layout.preferredWidth: 170
            }
            Label { text: "Hours"; color: "#aab7c0"; visible: source.currentIndex === 0 }
            SpinBox { id: hours; from: 1; to: 720; value: 24; visible: source.currentIndex === 0; Layout.preferredWidth: 95 }
            ComboBox { id: layer; model: ["near", "deep"]; visible: source.currentIndex === 0; Layout.preferredWidth: 90 }
            Button {
                text: "Load"
                onClicked: {
                    if (source.currentIndex === 0) binLab.loadReal(hours.value, layer.currentText)
                    else binLab.loadSynthetic(source.currentIndex === 1 ? 1000000 : source.currentIndex === 2 ? 10000000 : 100000000)
                }
            }
            Label { text: binLab.status; color: "#b9c9d2"; elide: Text.ElideRight; Layout.fillWidth: true }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#080d12"
            border.color: "#2c3d49"
            BinLab { id: binLab; objectName: "binLab"; anchors.fill: parent }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                property real oldX: 0
                property real oldY: 0
                onPressed: function(mouse) { oldX = mouse.x; oldY = mouse.y }
                onPositionChanged: function(mouse) {
                    if (pressed) {
                        binLab.pan(mouse.x - oldX, mouse.y - oldY)
                        oldX = mouse.x; oldY = mouse.y
                    }
                }
                onWheel: function(wheel) {
                    binLab.zoom(wheel.angleDelta.y / 120.0,
                                (wheel.modifiers & Qt.ShiftModifier) !== 0,
                                wheel.x / width, wheel.y / height)
                    wheel.accepted = true
                }
            }
            Rectangle {
                anchors.left: parent.left; anchors.bottom: parent.bottom
                anchors.margins: 14
                width: hint.implicitWidth + 20; height: hint.implicitHeight + 12
                color: "#ba101820"
                Label { id: hint; anchors.centerIn: parent; text: "DRAG  pan     WHEEL  time + price     SHIFT + WHEEL  price"; color: "#9fb4bf"; font.pixelSize: 12 }
            }
        }
        Rectangle {
            Layout.preferredWidth: 304
            Layout.fillHeight: true
            color: "#111a23"
            border.color: "#2c3d49"
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12
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
                Repeater {
                    model: [
                        ["FPS", "fps", ""], ["Frame", "frameMs", " ms"],
                        ["Re-bin submit", "binSubmitMs", " ms"], ["GPU frame", "gpuFrameMs", " ms"],
                        ["Loaded entries", "entries", ""], ["GPU buffers", "gpuBytes", " bytes"],
                        ["Ticks / bin", "group", ""], ["Display tick", "tick", " $"],
                        ["Grid", "columns", " cols"], ["Rows", "rows", ""],
                        ["Load", "loadMs", " ms"], ["Decode", "decodeMs", " ms"],
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
                    text: "Unknown coverage is veiled. Source entries stay resident on the GPU while view changes rebuild only the output grid."
                    color: "#8198a6"; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: 11
                }
            }
        }
    }
}
