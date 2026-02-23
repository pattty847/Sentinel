import QtQuick 2.15
import QtQuick.Controls 2.15
import Sentinel.Charts 1.0

// Stock candle chart view — receives candles from StockChartDock via setCandles().
// Stocks only. No demo data. No auto-load.
Rectangle {
    id: root
    color: "#0c0f12"

    // Set by C++ dock via rootObject()->setProperty(...)
    property string ticker:    ""
    property string company:   ""
    property string period:    "5y"
    property string statusMsg: "Select a stock from the Screener"
    property bool   loading:   false

    property int    hoveredIndex:  -1
    property var    hoveredCandle: ({})

    // Called by C++ to load candle data
    function setCandles(candleList) {
        candleChart.clearCandles()
        candleChart.setCandles(candleList)
    }

    function clearChart() {
        candleChart.clearCandles()
        root.hoveredIndex  = -1
        root.hoveredCandle = {}
    }

    Column {
        anchors.fill:    parent
        anchors.margins: 0
        spacing:         0

        // ── Header bar ────────────────────────────────────────────────────────
        Rectangle {
            id:     header
            width:  parent.width
            height: 38
            color:  "#111519"

            Row {
                anchors.fill:    parent
                anchors.margins: 10
                spacing:         14

                Text {
                    text:                root.ticker.length > 0 ? root.ticker : "—"
                    color:               "#e0e6ed"
                    font.pixelSize:      14
                    font.bold:           true
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    text:                root.company
                    color:               "#7a8a99"
                    font.pixelSize:      12
                    anchors.verticalCenter: parent.verticalCenter
                    visible:             root.company.length > 0
                }

                Rectangle { width: 1; height: 18; color: "#2a3440"; anchors.verticalCenter: parent.verticalCenter }

                Text {
                    text:                candleChart.candleCount + " candles"
                    color:               "#4a5a6a"
                    font.pixelSize:      11
                    anchors.verticalCenter: parent.verticalCenter
                    visible:             candleChart.candleCount > 0
                }

                // Loading indicator
                Rectangle {
                    visible:             root.loading
                    width:               12; height: 12; radius: 6
                    color:               "#f0a030"
                    anchors.verticalCenter: parent.verticalCenter
                    SequentialAnimation on opacity {
                        running:  root.loading
                        loops:    Animation.Infinite
                        NumberAnimation { to: 0.3; duration: 600 }
                        NumberAnimation { to: 1.0; duration: 600 }
                    }
                }
            }
        }

        // ── Chart area ────────────────────────────────────────────────────────
        Rectangle {
            id:     chartArea
            width:  parent.width
            height: parent.height - header.height - footer.height
            color:  "#0f1419"

            // Empty state
            Text {
                anchors.centerIn: parent
                text:             root.statusMsg
                color:            "#3a4a5a"
                font.pixelSize:   14
                visible:          candleChart.candleCount === 0 && !root.loading
            }

            CandlestickBatched {
                id:              candleChart
                anchors.fill:    parent
                anchors.margins: 6

                candleWidth:     8
                candleSpacing:   2
                visible:         candleChart.candleCount > 0

                onHoveredCandleChanged: function(index) {
                    root.hoveredIndex  = index
                    root.hoveredCandle = index >= 0 ? getCandleAt(index) : {}
                }
            }

            // ── Pan (drag) ────────────────────────────────────────────────────
            MouseArea {
                anchors.fill:    candleChart
                acceptedButtons: Qt.LeftButton
                cursorShape:     containsPress ? Qt.ClosedHandCursor : Qt.OpenHandCursor

                property real lastX: 0

                onPressed:  function(e) { lastX = e.x }
                onPositionChanged: function(e) {
                    if (!pressed) return
                    const dx = e.x - lastX
                    lastX = e.x
                    candleChart.viewOffset = candleChart.viewOffset + dx
                }
            }

            // ── Wheel zoom (zoom toward cursor) ───────────────────────────────
            WheelHandler {
                target:          null
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

                onWheel: function(e) {
                    const zoomFactor = e.angleDelta.y > 0 ? 1.12 : (1.0 / 1.12)
                    const oldScale   = candleChart.zoomScale
                    const newScale   = Math.max(0.2, Math.min(10.0, oldScale * zoomFactor))

                    // Keep the candle under the cursor stationary:
                    // cursorOffset = distance from right edge to cursor in old scale
                    // after scale change, shift viewOffset to preserve that distance
                    const cursorFromRight = candleChart.width - e.x
                    const offsetDelta     = cursorFromRight * (newScale / oldScale - 1.0)

                    candleChart.zoomScale  = newScale
                    candleChart.viewOffset = candleChart.viewOffset + offsetDelta
                }
            }

            // Hover OHLCV overlay
            Rectangle {
                visible:         root.hoveredIndex >= 0
                anchors.top:     parent.top
                anchors.left:    parent.left
                anchors.margins: 10
                width:           hoverCol.width + 18
                height:          hoverCol.height + 14
                color:           "#182028"
                radius:          5
                border.color:    "#2b4050"
                border.width:    1

                Column {
                    id:               hoverCol
                    anchors.centerIn: parent
                    spacing:          3

                    Text {
                        text:           root.hoveredCandle.date || ""
                        color:          "#c0ccd8"
                        font.pixelSize: 11
                        font.bold:      true
                    }

                    Repeater {
                        model: [
                            { label: "O", key: "open",   decimals: 2 },
                            { label: "H", key: "high",   decimals: 2 },
                            { label: "L", key: "low",    decimals: 2 },
                            { label: "C", key: "close",  decimals: 2 },
                        ]
                        Row {
                            spacing: 6
                            Text {
                                text:           modelData.label + ":"
                                color:          "#5a7080"
                                font.pixelSize: 11
                                width:          14
                            }
                            Text {
                                text:           (root.hoveredCandle[modelData.key] || 0).toFixed(modelData.decimals)
                                color:          modelData.key === "close"
                                                    ? (root.hoveredCandle.bullish ? "#2fdd7a" : "#ef5c55")
                                                    : "#a0b0bc"
                                font.pixelSize: 11
                                font.family:    "Roboto Mono"
                            }
                        }
                    }

                    Row {
                        spacing: 6
                        Text { text: "Vol:"; color: "#5a7080"; font.pixelSize: 11; width: 24 }
                        Text {
                            text:           ((root.hoveredCandle.volume || 0) / 1e6).toFixed(2) + "M"
                            color:          "#a0b0bc"
                            font.pixelSize: 11
                            font.family:    "Roboto Mono"
                        }
                    }
                }
            }
        }

        // ── Footer status bar ─────────────────────────────────────────────────
        Rectangle {
            id:     footer
            width:  parent.width
            height: 26
            color:  "#0d1115"

            Text {
                anchors.left:           parent.left
                anchors.leftMargin:     10
                anchors.verticalCenter: parent.verticalCenter
                text:                   root.statusMsg
                color:                  "#3d5060"
                font.pixelSize:         11
            }

            Text {
                anchors.right:           parent.right
                anchors.rightMargin:     10
                anchors.verticalCenter:  parent.verticalCenter
                text:                    "Daily · " + root.period + " · Stocks only"
                color:                   "#2a3a4a"
                font.pixelSize:          10
            }
        }
    }
}
