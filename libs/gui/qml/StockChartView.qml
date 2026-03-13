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
    property bool   secSignalsLoading: false

    property int    hoveredIndex:  -1
    property var    hoveredCandle: ({})
    property var    candleData: []
    property var    secSignalPayload: ({})
    property var    secSignalAggregates: []
    property var    secSignalMarkers: []
    property int    hoveredSignalIndex: -1
    property var    hoveredSignal: ({})

    // Called by C++ to load candle data
    function setCandles(candleList) {
        root.candleData = candleList || []
        candleChart.clearCandles()
        candleChart.setCandles(candleList)
        rebuildSecSignalMarkers()
    }

    function clearChart() {
        root.candleData = []
        candleChart.clearCandles()
        root.hoveredIndex  = -1
        root.hoveredCandle = {}
        clearSecSignals()
    }

    function setSecSignals(payload) {
        root.secSignalPayload = payload || ({})
        root.secSignalAggregates = payload && payload.daily_aggregates ? payload.daily_aggregates : []
        rebuildSecSignalMarkers()
    }

    function clearSecSignals() {
        root.secSignalPayload = ({})
        root.secSignalAggregates = []
        root.secSignalMarkers = []
        root.hoveredSignalIndex = -1
        root.hoveredSignal = ({})
    }

    function normalizeDateKey(value) {
        if (!value)
            return ""
        if (typeof value === "string")
            return value.slice(0, 10)
        const date = new Date(value)
        if (isNaN(date.getTime()))
            return ""
        return date.toISOString().slice(0, 10)
    }

    function rebuildSecSignalMarkers() {
        const dateToIndex = ({})
        for (let i = 0; i < root.candleData.length; ++i) {
            const candle = root.candleData[i]
            dateToIndex[normalizeDateKey(candle.date || candle.timestamp)] = i
        }

        const markers = []
        for (let i = 0; i < root.secSignalAggregates.length; ++i) {
            const aggregate = root.secSignalAggregates[i]
            const dateKey = normalizeDateKey(aggregate.event_anchor_timestamp || aggregate.filing_date || aggregate.transaction_date)
            if (!(dateKey in dateToIndex))
                continue
            const marker = Object.assign({}, aggregate)
            marker.candleIndex = dateToIndex[dateKey]
            markers.push(marker)
        }
        root.secSignalMarkers = markers
    }

    function candleCenterX(index) {
        if (index < 0 || index >= root.candleData.length)
            return -1000
        const totalWidth = (candleChart.candleWidth + candleChart.candleSpacing) * candleChart.zoomScale
        const startX = candleChart.width - root.candleData.length * totalWidth + candleChart.viewOffset
        return startX + index * totalWidth + candleChart.candleWidth * candleChart.zoomScale * 0.5
    }

    function signalVisible(index) {
        const x = candleCenterX(index)
        return x >= -12 && x <= candleChart.width + 12
    }

    function markerColor(marker) {
        const buys = Number(marker.open_market_buy_count || 0)
        const sells = Number(marker.open_market_sell_count || 0)
        const score = Number(marker.signal_strength_score || 0)
        if (buys > 0 && sells === 0 && score >= 0)
            return "#2fdd7a"
        if (sells > 0 && buys === 0 && score <= 0)
            return "#ef5c55"
        if (buys > 0 || sells > 0)
            return "#e7c75f"
        return "#7a8794"
    }

    function markerLabel(marker) {
        const buys = Number(marker.open_market_buy_count || 0)
        const sells = Number(marker.open_market_sell_count || 0)
        if (buys > 0 && sells === 0)
            return "B"
        if (sells > 0 && buys === 0)
            return "S"
        if (buys > 0 || sells > 0)
            return "M"
        return "N"
    }

    function markerSummary(marker) {
        const netValue = Number(marker.net_open_market_value || marker.net_value || 0)
        const strength = Number(marker.signal_strength_score || 0)
        return "Net " + formatCurrency(netValue) + " | score " + strength.toFixed(2)
    }

    function markerReasons(marker) {
        if (!marker || !marker.signal_strength_reason)
            return ""
        if (marker.signal_strength_reason.join)
            return marker.signal_strength_reason.join(", ")
        return String(marker.signal_strength_reason)
    }

    function formatCurrency(value) {
        const amount = Number(value || 0)
        const absAmount = Math.abs(amount)
        const sign = amount < 0 ? "-" : ""
        if (absAmount >= 1000000000)
            return sign + "$" + (absAmount / 1000000000).toFixed(absAmount >= 10000000000 ? 0 : 1) + "B"
        if (absAmount >= 1000000)
            return sign + "$" + (absAmount / 1000000).toFixed(absAmount >= 10000000 ? 0 : 1) + "M"
        if (absAmount >= 1000)
            return sign + "$" + (absAmount / 1000).toFixed(absAmount >= 100000 ? 0 : 1) + "K"
        return sign + "$" + Math.round(absAmount).toLocaleString()
    }

    function hasZeroValueNote(marker) {
        if (!marker || !marker.key_events)
            return false
        for (let i = 0; i < marker.key_events.length; ++i) {
            const event = marker.key_events[i]
            const signalClass = String(event.signal_class || "")
            const grossValue = Number(event.gross_value || 0)
            if (grossValue === 0 && (signalClass === "option_exercise" || signalClass === "award_or_grant" || signalClass === "derivative_conversion" || signalClass === "gift"))
                return true
        }
        return false
    }

    function zeroValueNoteText(marker) {
        if (!hasZeroValueNote(marker))
            return ""
        return "$0 on some neutral/derivative rows means the filing did not report a clean cash value for that leg."
    }

    function signalTooltipX() {
        if (!root.hoveredSignal || root.hoveredSignal.candleIndex === undefined)
            return 10
        const markerX = candleCenterX(root.hoveredSignal.candleIndex)
        const cardWidth = signalTooltipCard.width
        const margin = 12
        const preferRight = markerX < (chartArea.width * 0.55)
        const desiredX = preferRight ? (markerX + margin) : (markerX - cardWidth - margin)
        return Math.max(10, Math.min(chartArea.width - cardWidth - 10, desiredX))
    }

    function signalTooltipY() {
        const desiredY = 38
        return Math.max(10, Math.min(chartArea.height - signalTooltipCard.height - 10, desiredY))
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

                // TICKER | TIMEFRAME — single source of truth for symbol and range
                Text {
                    text:                (root.ticker.length > 0 ? root.ticker : "—") + " | " + root.period
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

                Rectangle { width: 1; height: 18; color: "#2a3440"; anchors.verticalCenter: parent.verticalCenter; visible: candleChart.candleCount > 0 }

                // Candle count for screenshots / context only (no duplication with footer)
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

            Item {
                id: secOverlayLayer
                anchors.fill: candleChart
                visible: candleChart.candleCount > 0 && root.secSignalMarkers.length > 0

                Repeater {
                    model: root.secSignalMarkers

                    Item {
                        width: 22
                        height: 22
                        x: root.candleCenterX(modelData.candleIndex) - width / 2
                        y: 12
                        visible: root.signalVisible(modelData.candleIndex)
                        z: 5

                        Rectangle {
                            anchors.centerIn: parent
                            width: 18
                            height: 18
                            radius: 9
                            color: root.markerColor(modelData)
                            border.color: hoveredArea.containsMouse ? "#ffffff" : "#0f1419"
                            border.width: hoveredArea.containsMouse ? 2 : 1
                            opacity: 0.92

                            Text {
                                anchors.centerIn: parent
                                text: root.markerLabel(modelData)
                                color: "#0f1419"
                                font.pixelSize: 10
                                font.bold: true
                            }
                        }

                        MouseArea {
                            id: hoveredArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: {
                                root.hoveredSignalIndex = index
                                root.hoveredSignal = modelData
                            }
                            onExited: {
                                if (root.hoveredSignalIndex === index) {
                                    root.hoveredSignalIndex = -1
                                    root.hoveredSignal = ({})
                                }
                            }
                            onClicked: {
                                root.hoveredSignalIndex = index
                                root.hoveredSignal = modelData
                            }
                        }
                    }
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

            Rectangle {
                id:              signalTooltipCard
                visible:         root.hoveredSignalIndex >= 0
                x:               root.signalTooltipX()
                y:               root.signalTooltipY()
                width:           320
                height:          signalInfoCol.implicitHeight + 14
                color:           "#182028"
                radius:          5
                border.color:    "#2b4050"
                border.width:    1

                Column {
                    id:               signalInfoCol
                    anchors.left:     parent.left
                    anchors.right:    parent.right
                    anchors.margins:  9
                    anchors.verticalCenter: parent.verticalCenter
                    spacing:          4

                    Text {
                        text:           (root.hoveredSignal.event_anchor_timestamp || "") + "  " + root.markerLabel(root.hoveredSignal)
                        color:          "#dbe6ef"
                        font.pixelSize: 11
                        font.bold:      true
                    }

                    Text {
                        text:           root.markerSummary(root.hoveredSignal)
                        color:          "#9fb2c4"
                        font.pixelSize: 10
                    }

                    Text {
                        text:           root.markerReasons(root.hoveredSignal)
                        color:          "#6e8599"
                        font.pixelSize: 10
                        wrapMode:       Text.WordWrap
                        width:          parent.width
                        visible:        text.length > 0
                    }

                    Repeater {
                        model: root.hoveredSignal.key_events || []

                        Text {
                            text:           "- " + (modelData.owner_name || "Unknown") + " | " + (modelData.signal_class || "other") + " | " + root.formatCurrency(modelData.gross_value || 0)
                            color:          "#c3d1dd"
                            font.pixelSize: 10
                            wrapMode:       Text.WordWrap
                            width:          parent.width
                        }
                    }

                    Repeater {
                        model: root.hoveredSignal.filing_links || []

                        Text {
                            text:           modelData
                            color:          "#5f9fd6"
                            font.pixelSize: 9
                            wrapMode:       Text.WrapAnywhere
                            width:          parent.width
                        }
                    }

                    Text {
                        text:           root.zeroValueNoteText(root.hoveredSignal)
                        color:          "#6e8599"
                        font.pixelSize: 9
                        wrapMode:       Text.WordWrap
                        width:          parent.width
                        visible:        text.length > 0
                    }
                }
            }
        }

        // ── Footer: no duplicate ticker/range/candles; right side only ─────────
        Rectangle {
            id:     footer
            width:  parent.width
            height: 26
            color:  "#0d1115"

            Text {
                anchors.right:           parent.right
                anchors.rightMargin:    10
                anchors.verticalCenter:  parent.verticalCenter
                text:                    "Stocks only"
                color:                   "#2a3a4a"
                font.pixelSize:          10
            }

            Text {
                anchors.left:            parent.left
                anchors.leftMargin:      10
                anchors.verticalCenter:  parent.verticalCenter
                text: root.secSignalsLoading
                    ? "SEC signals loading"
                    : ((root.secSignalPayload.llm_digest && root.secSignalPayload.llm_digest.summary)
                        ? ("SEC insiders | filings " + (root.secSignalPayload.llm_digest.summary.total_filings || 0)
                           + " | insiders " + (root.secSignalPayload.llm_digest.summary.unique_insiders || 0))
                        : "SEC insiders idle")
                color:                   "#355066"
                font.pixelSize:          10
            }
        }
    }
}
