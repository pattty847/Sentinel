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

    // Convenience: fraction of candleChart height used for volume bars (mirrors C++ kVolFraction)
    readonly property real volFraction: candleChart.volumeHeightFraction

    // ── Data API (called by C++) ───────────────────────────────────────────────

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
        root.secSignalPayload    = payload || ({})
        root.secSignalAggregates = payload && payload.daily_aggregates ? payload.daily_aggregates : []
        rebuildSecSignalMarkers()
    }

    function clearSecSignals() {
        root.secSignalPayload    = ({})
        root.secSignalAggregates = []
        root.secSignalMarkers    = []
        root.hoveredSignalIndex  = -1
        root.hoveredSignal       = ({})
        candleChart.setSecSignalOverrides([])
    }

    // ── Coordinate helpers ────────────────────────────────────────────────────

    function normalizeDateKey(value) {
        if (!value) return ""
        if (typeof value === "string") return value.slice(0, 10)
        const date = new Date(value)
        if (isNaN(date.getTime())) return ""
        return date.toISOString().slice(0, 10)
    }

    // X center of candle[index] in candleChart-local coordinates.
    function candleCenterX(index) {
        if (index < 0 || index >= root.candleData.length) return -1000
        const totalWidth = (candleChart.candleWidth + candleChart.candleSpacing) * candleChart.zoomScale
        const startX = candleChart.width - root.candleData.length * totalWidth + candleChart.viewOffset
        return startX + index * totalWidth + candleChart.candleWidth * candleChart.zoomScale * 0.5
    }

    // Convert a price to a Y pixel within candleChart (candle region only, not volume area).
    // Uses the live visiblePriceMin/Max exposed by the C++ renderer.
    function priceToY(price) {
        const vMin = candleChart.visiblePriceMin
        const vMax = candleChart.visiblePriceMax
        if (vMax <= vMin) return 0
        const candleAreaH = candleChart.height * (1.0 - root.volFraction)
        const pad    = candleAreaH * 0.05
        const chartH = candleAreaH - 2.0 * pad
        const norm   = (price - vMin) / (vMax - vMin)
        return pad + (1.0 - norm) * chartH
    }

    function signalVisible(index) {
        const x = candleCenterX(index)
        return x >= -12 && x <= candleChart.width + 12
    }

    // ── SEC marker classification ─────────────────────────────────────────────

    function markerColor(marker) {
        const buys  = Number(marker.open_market_buy_count  || 0)
        const sells = Number(marker.open_market_sell_count || 0)
        const score = Number(marker.signal_strength_score  || 0)
        if (buys > 0 && sells === 0 && score >= 0) return "#2fdd7a"
        if (sells > 0 && buys === 0 && score <= 0) return "#ef5c55"
        if (buys > 0 || sells > 0)                 return "#e7c75f"
        return "#7a8794"
    }

    function markerLabel(marker) {
        const buys  = Number(marker.open_market_buy_count  || 0)
        const sells = Number(marker.open_market_sell_count || 0)
        if (buys > 0 && sells === 0) return "B"
        if (sells > 0 && buys === 0) return "S"
        if (buys > 0 || sells > 0)   return "M"
        return "N"
    }

    // Signal type int matching C++ setSecSignalOverrides convention: 1=buy, 2=sell, 3=mixed
    function markerSignalType(marker) {
        const lbl = markerLabel(marker)
        if (lbl === "B") return 1
        if (lbl === "S") return 2
        if (lbl === "M") return 3
        return 0
    }

    // ── Marker Y positioning ──────────────────────────────────────────────────
    // Sells land just above the high wick; buys just below the low wick.
    // Neutral/mixed stay at a fixed top row (y=4).
    // A simple greedy pass then nudges overlapping same-side markers further apart.

    function computeMarkerY(marker) {
        const lbl  = markerLabel(marker)
        const cidx = marker.candleIndex
        if (cidx < 0 || cidx >= root.candleData.length) return 4
        const candle     = root.candleData[cidx]
        const markerH    = 22          // px height of the marker widget
        const gap        = 4           // px gap between wick tip and marker edge
        const candleAreaH = candleChart.height * (1.0 - root.volFraction)

        if (lbl === "S") {
            // Above the high wick
            const highY = root.priceToY(candle.high || 0)
            return Math.max(4, highY - markerH - gap)
        }
        if (lbl === "B") {
            // Below the low wick
            const lowY = root.priceToY(candle.low || 0)
            return Math.min(candleAreaH - markerH - gap, lowY + gap)
        }
        // Mixed / Neutral: top strip
        return 4
    }

    // ── Marker + signal-override rebuild ─────────────────────────────────────

    function rebuildSecSignalMarkers() {
        // 1. Map trading date → candle index
        const dateToIndex = ({})
        for (let i = 0; i < root.candleData.length; ++i) {
            const candle = root.candleData[i]
            dateToIndex[normalizeDateKey(candle.date || candle.timestamp)] = i
        }

        // 2. Build marker list
        const markers = []
        for (let i = 0; i < root.secSignalAggregates.length; ++i) {
            const agg     = root.secSignalAggregates[i]
            const dateKey = normalizeDateKey(agg.event_anchor_timestamp || agg.filing_date || agg.transaction_date)
            if (!(dateKey in dateToIndex)) continue
            const marker = Object.assign({}, agg)
            marker.candleIndex = dateToIndex[dateKey]
            markers.push(marker)
        }
        root.secSignalMarkers = markers

        // 3. Push candle body color overrides to the C++ renderer
        const overrides = []
        for (let i = 0; i < markers.length; ++i) {
            const st = root.markerSignalType(markers[i])
            if (st > 0)
                overrides.push({ index: markers[i].candleIndex, signalType: st })
        }
        candleChart.setSecSignalOverrides(overrides)
    }

    // ── Tooltip / summary helpers ─────────────────────────────────────────────

    function markerSummary(marker) {
        const netValue = Number(marker.net_open_market_value || marker.net_value || 0)
        const strength = Number(marker.signal_strength_score || 0)
        return "Net " + formatCurrency(netValue) + " | score " + strength.toFixed(2)
    }

    function markerReasons(marker) {
        if (!marker || !marker.signal_strength_reason) return ""
        if (marker.signal_strength_reason.join) return marker.signal_strength_reason.join(", ")
        return String(marker.signal_strength_reason)
    }

    function formatCurrency(value) {
        const amount    = Number(value || 0)
        const absAmount = Math.abs(amount)
        const sign      = amount < 0 ? "-" : ""
        if (absAmount >= 1000000000) return sign + "$" + (absAmount / 1000000000).toFixed(absAmount >= 10000000000 ? 0 : 1) + "B"
        if (absAmount >= 1000000)    return sign + "$" + (absAmount / 1000000).toFixed(absAmount >= 10000000 ? 0 : 1) + "M"
        if (absAmount >= 1000)       return sign + "$" + (absAmount / 1000).toFixed(absAmount >= 100000 ? 0 : 1) + "K"
        return sign + "$" + Math.round(absAmount).toLocaleString()
    }

    function hasZeroValueNote(marker) {
        if (!marker || !marker.key_events) return false
        for (let i = 0; i < marker.key_events.length; ++i) {
            const event      = marker.key_events[i]
            const signalClass = String(event.signal_class || "")
            const grossValue  = Number(event.gross_value || 0)
            if (grossValue === 0 && (signalClass === "option_exercise" || signalClass === "award_or_grant" || signalClass === "derivative_conversion" || signalClass === "gift"))
                return true
        }
        return false
    }

    function zeroValueNoteText(marker) {
        if (!hasZeroValueNote(marker)) return ""
        return "$0 on some neutral/derivative rows means the filing did not report a clean cash value for that leg."
    }

    // ── Signal tooltip placement ───────────────────────────────────────────────

    function signalTooltipX() {
        if (!root.hoveredSignal || root.hoveredSignal.candleIndex === undefined) return 10
        const markerX   = candleCenterX(root.hoveredSignal.candleIndex)
        // markerX is in candleChart coords; adjust to chartArea coords
        const chartMarkerX = markerX + candleChart.x
        const cardWidth = signalTooltipCard.width
        const margin    = 12
        const preferRight = chartMarkerX < (chartArea.width * 0.55)
        const desiredX = preferRight ? (chartMarkerX + margin) : (chartMarkerX - cardWidth - margin)
        return Math.max(10, Math.min(chartArea.width - cardWidth - 10, desiredX))
    }

    function signalTooltipY() {
        return Math.max(10, Math.min(chartArea.height - signalTooltipCard.height - 10, 38))
    }

    // ── Nice-number helper (mirrors PriceAxisModel::calculateNicePriceStep) ──
    // Rounds rawStep up to the nearest 1/2/2.5/5/10 × magnitude value.
    function niceStep(rawStep) {
        if (rawStep <= 0) return 1
        const mag  = Math.pow(10, Math.floor(Math.log10(rawStep)))
        const norm = rawStep / mag
        let nice
        if      (norm <= 1.0) nice = 1.0
        else if (norm <= 2.0) nice = 2.0
        else if (norm <= 2.5) nice = 2.5
        else if (norm <= 5.0) nice = 5.0
        else                  nice = 10.0
        return nice * mag
    }

    // ── Price axis labels ─────────────────────────────────────────────────────
    // Computes tick positions using the niceStep algorithm so labels always land
    // on clean round numbers (e.g. $150, $155, $160) regardless of zoom level.
    property var priceAxisTicks: {
        var _min = candleChart.visiblePriceMin  // binding dep → reacts to pan/zoom
        var _max = candleChart.visiblePriceMax
        if (_max <= _min || candleChart.candleCount === 0) return []

        const candleAreaH = candleChart.height * (1.0 - root.volFraction)
        const minGapPx    = 50
        const maxCount    = Math.max(2, Math.floor(candleAreaH / minGapPx))
        const step        = root.niceStep((_max - _min) / (maxCount - 1))

        // Decimal places inferred from the visible range (mirrors PriceAxisModel::formatLabel)
        const range    = _max - _min
        const decimals = range > 1000 ? 0 : range > 100 ? 1 : 2

        // Align first tick to a step boundary so labels stay stable during pan
        const firstIdx = Math.ceil(_min / step)
        const lastIdx  = Math.floor(_max / step)
        const ticks    = []
        for (let i = firstIdx; i <= lastIdx && ticks.length < 20; ++i) {
            const price = i * step
            const y     = root.priceToY(price) - 7   // center label on the grid line
            if (y >= 0 && y <= candleAreaH)
                ticks.push({ label: "$" + price.toFixed(decimals), y: y })
        }
        return ticks
    }

    // ── Time axis labels ──────────────────────────────────────────────────────
    // Picks a nice candle interval from the 1/2/5/10 progression so labels are
    // evenly spaced and the first tick is aligned to a multiple of the interval
    // (so ticks stay stable / don't drift while panning).
    property var timeAxisTicks: {
        var _off   = candleChart.viewOffset        // binding dep (pan)
        var _zoom  = candleChart.zoomScale         // binding dep (zoom)
        var _first = candleChart.firstVisibleIndex
        var _last  = candleChart.lastVisibleIndex
        if (_first < 0 || _last < 0 || root.candleData.length === 0) return []

        const visibleCount = _last - _first + 1
        const targetCount  = 6

        // Nice candle intervals following the 1/2/5/10 progression.
        // 63 ≈ quarter, 126 ≈ half-year, 252 ≈ trading year.
        const candidates = [1, 2, 5, 10, 20, 50, 63, 126, 252, 504, 1008]
        const rawInterval = Math.max(1, Math.round(visibleCount / targetCount))
        let interval = candidates[candidates.length - 1]
        for (let c = 0; c < candidates.length; ++c) {
            if (candidates[c] >= rawInterval) { interval = candidates[c]; break }
        }

        // Date format adapts to interval granularity
        function formatDate(d, iv) {
            if (!d) return ""
            if (iv >= 252) return d.slice(0, 4)     // YYYY
            if (iv >= 20)  return d.slice(0, 7)     // YYYY-MM
            return d.slice(5, 10)                    // MM-DD
        }

        // Align first tick to a multiple of interval (stable under pan)
        const alignedFirst = Math.ceil(_first / interval) * interval
        const ticks = []
        for (let i = alignedFirst; i <= _last; i += interval) {
            if (i >= root.candleData.length) break
            const d = root.candleData[i].date || ""
            if (!d) continue
            const x = root.candleCenterX(i)
            if (x >= 0 && x <= candleChart.width)
                ticks.push({ label: formatDate(d, interval), x: x })
        }
        return ticks
    }

    // ── Layout ────────────────────────────────────────────────────────────────

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

                Text {
                    text:                candleChart.candleCount + " candles"
                    color:               "#4a5a6a"
                    font.pixelSize:      11
                    anchors.verticalCenter: parent.verticalCenter
                    visible:             candleChart.candleCount > 0
                }

                Rectangle {
                    visible:             root.loading
                    width: 12; height: 12; radius: 6
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

            // ── Candle chart (leaves room for price axis right, time axis bottom) ──
            CandlestickBatched {
                id:           candleChart
                anchors.top:  parent.top
                anchors.left: parent.left
                anchors.right:        priceAxis.left
                anchors.bottom:       timeAxis.top
                anchors.topMargin:    6
                anchors.leftMargin:   6
                anchors.rightMargin:  0
                anchors.bottomMargin: 0

                candleWidth:   8
                candleSpacing: 2
                visible:       candleChart.candleCount > 0

                onHoveredCandleChanged: function(index) {
                    root.hoveredIndex  = index
                    root.hoveredCandle = index >= 0 ? getCandleAt(index) : {}
                }
            }

            // ── Price axis (right strip) ──────────────────────────────────────
            Item {
                id:             priceAxis
                anchors.top:    parent.top
                anchors.right:  parent.right
                anchors.bottom: timeAxis.top
                anchors.topMargin:   6
                anchors.rightMargin: 4
                width:          56
                visible:        candleChart.candleCount > 0

                // Subtle left border
                Rectangle {
                    anchors.left:   parent.left
                    anchors.top:    parent.top
                    anchors.bottom: parent.bottom
                    width: 1
                    color: "#1e2a35"
                }

                Repeater {
                    model: root.priceAxisTicks
                    delegate: Text {
                        x:              6
                        y:              modelData.y
                        text:           modelData.label
                        color:          "#4a5f70"
                        font.pixelSize: 10
                        font.family:    "Roboto Mono"
                    }
                }
            }

            // ── Time axis (bottom strip) ──────────────────────────────────────
            Item {
                id:             timeAxis
                anchors.left:   parent.left
                anchors.right:  priceAxis.left
                anchors.bottom: parent.bottom
                anchors.leftMargin:   6
                anchors.bottomMargin: 4
                height:         20
                visible:        candleChart.candleCount > 0

                // Subtle top border
                Rectangle {
                    anchors.left:  parent.left
                    anchors.right: parent.right
                    anchors.top:   parent.top
                    height: 1
                    color: "#1e2a35"
                }

                Repeater {
                    model: root.timeAxisTicks
                    delegate: Text {
                        // x is in candleChart coordinates; candleChart.x = 6 from chartArea
                        x:              modelData.x - implicitWidth / 2
                        y:              4
                        text:           modelData.label
                        color:          "#4a5f70"
                        font.pixelSize: 10
                        font.family:    "Roboto Mono"
                    }
                }
            }

            // ── SEC signal markers overlay ────────────────────────────────────
            // Buys appear below the low wick; sells above the high wick;
            // mixed/neutral appear in the top strip.
            Item {
                id:      secOverlayLayer
                anchors.fill: candleChart
                visible: candleChart.candleCount > 0 && root.secSignalMarkers.length > 0

                Repeater {
                    model: root.secSignalMarkers

                    Item {
                        id:      markerItem
                        width:   22
                        height:  22

                        // x tracks pan/zoom reactively by referencing the live viewport props
                        x: {
                            var _off  = candleChart.viewOffset   // binding dep
                            var _zoom = candleChart.zoomScale    // binding dep
                            return root.candleCenterX(modelData.candleIndex) - width / 2
                        }

                        // y tracks price range reactively (sells above wick, buys below wick)
                        y: {
                            var _min = candleChart.visiblePriceMin  // binding dep
                            var _max = candleChart.visiblePriceMax  // binding dep
                            return root.computeMarkerY(modelData)
                        }

                        visible: root.signalVisible(modelData.candleIndex)
                        z: 5

                        Rectangle {
                            anchors.centerIn: parent
                            width:  18
                            height: 18
                            radius: 9
                            color:        root.markerColor(modelData)
                            border.color: hoveredArea.containsMouse ? "#ffffff" : "#0f1419"
                            border.width: hoveredArea.containsMouse ? 2 : 1
                            opacity: 0.92

                            Text {
                                anchors.centerIn: parent
                                text:       root.markerLabel(modelData)
                                color:      "#0f1419"
                                font.pixelSize: 10
                                font.bold:  true
                            }
                        }

                        MouseArea {
                            id: hoveredArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onEntered: {
                                root.hoveredSignalIndex = index
                                root.hoveredSignal      = modelData
                            }
                            onExited: {
                                if (root.hoveredSignalIndex === index) {
                                    root.hoveredSignalIndex = -1
                                    root.hoveredSignal      = ({})
                                }
                            }
                            onClicked: {
                                root.hoveredSignalIndex = index
                                root.hoveredSignal      = modelData
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

                    // Adjust e.x to candleChart-local coordinates so zoom anchors to cursor
                    const localX        = Math.max(0, Math.min(candleChart.width, e.x - candleChart.x))
                    const cursorFromRight = candleChart.width - localX
                    const offsetDelta   = cursorFromRight * (newScale / oldScale - 1.0)

                    candleChart.zoomScale  = newScale
                    candleChart.viewOffset = candleChart.viewOffset + offsetDelta
                }
            }

            // ── Hover OHLCV overlay ───────────────────────────────────────────
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

            // ── Signal detail tooltip card ─────────────────────────────────────
            Rectangle {
                id:      signalTooltipCard
                visible: root.hoveredSignalIndex >= 0
                x:       root.signalTooltipX()
                y:       root.signalTooltipY()
                width:   320
                height:  signalInfoCol.implicitHeight + 14
                color:   "#182028"
                radius:  5
                border.color: "#2b4050"
                border.width: 1

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

        // ── Footer ────────────────────────────────────────────────────────────
        Rectangle {
            id:     footer
            width:  parent.width
            height: 26
            color:  "#0d1115"

            Text {
                anchors.right:          parent.right
                anchors.rightMargin:    10
                anchors.verticalCenter: parent.verticalCenter
                text:                   "Stocks only"
                color:                  "#2a3a4a"
                font.pixelSize:         10
            }

            Text {
                anchors.left:           parent.left
                anchors.leftMargin:     10
                anchors.verticalCenter: parent.verticalCenter
                text: root.secSignalsLoading
                    ? "SEC signals loading"
                    : ((root.secSignalPayload.llm_digest && root.secSignalPayload.llm_digest.summary)
                        ? ("SEC insiders | filings " + (root.secSignalPayload.llm_digest.summary.total_filings || 0)
                           + " | insiders " + (root.secSignalPayload.llm_digest.summary.unique_insiders || 0))
                        : "SEC insiders idle")
                color:                  "#355066"
                font.pixelSize:         10
            }
        }
    }
}
