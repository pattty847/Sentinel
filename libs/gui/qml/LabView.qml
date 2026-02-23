import QtQuick 2.15
import QtQuick.Controls 2.15
import Sentinel.Charts 1.0

Rectangle {
    id: root
    color: "#0c0f12"

    property int hoveredIndex: -1
    property var hoveredCandle: ({})

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // Header bar
        Rectangle {
            height: 42
            width: parent.width
            color: "#151a1f"
            radius: 6

            Row {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 16

                Text {
                    text: "Lab - Candlestick Demo"
                    color: "#d6dbe0"
                    font.pixelSize: 14
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: "Candles: " + candleChart.candleCount
                    color: "#98a2ad"
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: "Width:"
                    color: "#98a2ad"
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }

                Slider {
                    id: widthSlider
                    from: 4
                    to: 24
                    value: 12
                    width: 120
                    anchors.verticalCenter: parent.verticalCenter
                    onValueChanged: candleChart.candleWidth = value
                }

                Text {
                    text: widthSlider.value.toFixed(0) + "px"
                    color: "#98a2ad"
                    font.pixelSize: 12
                    width: 36
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: "Spacing:"
                    color: "#98a2ad"
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }

                Slider {
                    id: spacingSlider
                    from: 0
                    to: 12
                    value: 4
                    width: 100
                    anchors.verticalCenter: parent.verticalCenter
                    onValueChanged: candleChart.candleSpacing = value
                }

                Text {
                    text: spacingSlider.value.toFixed(0) + "px"
                    color: "#98a2ad"
                    font.pixelSize: 12
                    width: 36
                    anchors.verticalCenter: parent.verticalCenter
                }

                Button {
                    text: "Load 30"
                    onClicked: candleChart.loadDemoData(30)
                    anchors.verticalCenter: parent.verticalCenter
                }

                Button {
                    text: "Load 100"
                    onClicked: candleChart.loadDemoData(100)
                    anchors.verticalCenter: parent.verticalCenter
                }

                Button {
                    text: "Clear"
                    onClicked: candleChart.clearCandles()
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // Main chart area
        Rectangle {
            id: chartArea
            width: parent.width
            height: parent.height - 120
            color: "#0f1419"
            radius: 8
            border.color: "#1d262e"
            border.width: 1

            CandlestickBatched {
                id: candleChart
                anchors.fill: parent
                anchors.margins: 8

                candleWidth: 12
                candleSpacing: 4

                Component.onCompleted: {
                    loadDemoData(30);
                }

                onHoveredCandleChanged: function(index) {
                    root.hoveredIndex = index;
                    if (index >= 0) {
                        root.hoveredCandle = getCandleAt(index);
                    } else {
                        root.hoveredCandle = {};
                    }
                }

                onCandleClicked: function(index) {
                    console.log("Candle clicked:", index, JSON.stringify(getCandleAt(index)));
                }
            }

            // Hover info overlay
            Rectangle {
                visible: root.hoveredIndex >= 0
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 12
                width: infoColumn.width + 20
                height: infoColumn.height + 16
                color: "#1a2530"
                radius: 6
                border.color: "#2b4756"
                border.width: 1

                Column {
                    id: infoColumn
                    anchors.centerIn: parent
                    spacing: 4

                    Text {
                        text: "Candle #" + root.hoveredIndex
                        color: "#d6dbe0"
                        font.pixelSize: 12
                        font.bold: true
                    }

                    Text {
                        text: "O: " + (root.hoveredCandle.open || 0).toFixed(2)
                        color: "#98a2ad"
                        font.pixelSize: 11
                        font.family: "Roboto Mono"
                    }

                    Text {
                        text: "H: " + (root.hoveredCandle.high || 0).toFixed(2)
                        color: "#98a2ad"
                        font.pixelSize: 11
                        font.family: "Roboto Mono"
                    }

                    Text {
                        text: "L: " + (root.hoveredCandle.low || 0).toFixed(2)
                        color: "#98a2ad"
                        font.pixelSize: 11
                        font.family: "Roboto Mono"
                    }

                    Text {
                        text: "C: " + (root.hoveredCandle.close || 0).toFixed(2)
                        color: root.hoveredCandle.bullish ? "#2fdd7a" : "#ef5c55"
                        font.pixelSize: 11
                        font.family: "Roboto Mono"
                        font.bold: true
                    }

                    Text {
                        text: "Vol: " + ((root.hoveredCandle.volume || 0) / 1000).toFixed(1) + "K"
                        color: "#98a2ad"
                        font.pixelSize: 11
                        font.family: "Roboto Mono"
                    }
                }
            }
        }

        // Footer info bar
        Rectangle {
            width: parent.width
            height: 36
            color: "#151a1f"
            radius: 6

            Row {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 24

                Text {
                    text: root.hoveredIndex >= 0
                        ? "Hovering candle " + root.hoveredIndex + " - Click to select"
                        : "Hover over candles to see OHLCV data"
                    color: "#98a2ad"
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: "|"
                    color: "#3d4f5f"
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text: "GPU Batched Rendering"
                    color: "#5f8498"
                    font.pixelSize: 12
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}
