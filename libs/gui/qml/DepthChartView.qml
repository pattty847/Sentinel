import QtQuick 2.15
import QtQuick.Controls
import Sentinel 1.0
import Sentinel.Charts 1.0

Rectangle {
    id: root
    color: uiTheme ? uiTheme.bg : "black"
    
    property string symbol: "BTC-USD"
    property bool stressTestMode: false
    property var chartModeController: null
    property bool gridModeEnabled: true
    property bool candlesEnabled: chartModeController ? chartModeController.candlesEnabled : true
    property bool showCandles: candlesEnabled

    property int currentActiveTimeframe: 100

    // Asset-specific volume ranges (BTC/ETH/DOGE differ by orders of magnitude)
    // TODO: Make dynamic
    property real maxVolumeRange: {
        if (symbol.includes("BTC")) return 10.0;
        if (symbol.includes("ETH")) return 500.0;
        if (symbol.includes("DOGE")) return 10000.0;
        return 1000.0;
    }
    
    property real lastTimeSpan: 0
    property int lastTimeframe: 0
    property bool showTimeGrid: false

    function clampChartX(x) {
        return Math.max(0, Math.min(unifiedGridRenderer.width, x))
    }

    function clampChartY(y) {
        return Math.max(0, Math.min(unifiedGridRenderer.height, y))
    }

    function mapPriceAxisPoint(source, x, y) {
        const p = source.mapToItem(unifiedGridRenderer, x, y)
        return Qt.point(Math.max(0, unifiedGridRenderer.width - 1), clampChartY(p.y))
    }

    function mapTimeAxisPoint(source, x, y) {
        const p = source.mapToItem(unifiedGridRenderer, x, y)
        return Qt.point(clampChartX(p.x), Math.max(0, unifiedGridRenderer.height - 1))
    }

    function chartYFromItem(item, x, y) {
        const p = item.mapToItem(unifiedGridRenderer, x, y)
        return clampChartY(p.y)
    }

    Connections {
        target: unifiedGridRenderer
        function onViewportChanged() {}
        function onTimeframeChanged() {}
        function onPanVisualOffsetChanged() {}
    }

    Rectangle {
        id: heatmapBackground
        anchors.fill: parent
        anchors.rightMargin: 90
        anchors.bottomMargin: 30
        color: root.color
        visible: true
        z: 0
    }

    UnifiedGridRenderer {
        id: unifiedGridRenderer
        objectName: "unifiedGridRenderer"
        anchors.fill: parent
        anchors.rightMargin: 90
        anchors.bottomMargin: 30
        visible: true
        intensityScale: 1.0
        maxCells: 500000
        heatmapBackgroundColor: "black"
        priceAxisSource: priceAxisModel
        timeAxisSource: timeAxisModel
        z: 1
        
        onTimeframeChanged: {
            root.currentActiveTimeframe = unifiedGridRenderer.timeframeMs
            console.log(" Timeframe changed to:", root.currentActiveTimeframe, "ms")
        }
        
        Component.onCompleted: {
            root.currentActiveTimeframe = unifiedGridRenderer.timeframeMs
        }
    }

    CandlestickOverlayItem {
        id: candleOverlay
        anchors.fill: unifiedGridRenderer
        visible: root.showCandles
        mappingProvider: unifiedGridRenderer
        candleBuffer: dataSource ? dataSource.candleBuffer : null
        symbol: root.symbol
        timeframeSec: Math.max(1, Math.round(unifiedGridRenderer.timeframeMs / 1000))
        z: 2
    }

    AlgoOverlayRenderer {
        id: algoOverlayRenderer
        objectName: "algoOverlayRenderer"
        anchors.fill: unifiedGridRenderer
        mappingProvider: unifiedGridRenderer
        enabled: true
        z: 3
    }

    PaperTradeOverlayModel {
        id: paperTradeOverlayModel
        objectName: "paperTradeOverlayModel"
        symbol: root.symbol
        mappingProvider: unifiedGridRenderer
    }

    PaperTradeOverlayRenderer {
        id: paperTradeOverlayRenderer
        objectName: "paperTradeOverlayRenderer"
        anchors.fill: unifiedGridRenderer
        mappingProvider: unifiedGridRenderer
        overlayModel: paperTradeOverlayModel
        enabled: true
        z: 4
    }

    Item {
        id: paperTradeOverlayLayer
        anchors.fill: unifiedGridRenderer
        z: 5

        function formatQty(value) {
            return Number(value).toFixed(4)
        }

        function formatPrice(value) {
            return "$" + Number(value).toFixed(2)
        }

        function formatPnl(value) {
            const numeric = Number(value)
            return (numeric >= 0 ? "+" : "") + "$" + numeric.toFixed(2)
        }

        function formatPct(value) {
            const numeric = Number(value)
            return (numeric >= 0 ? "+" : "") + numeric.toFixed(2) + "%"
        }

        function percentBetween(entryPrice, targetPrice, isLong) {
            const entry = Number(entryPrice)
            const target = Number(targetPrice)
            if (entry <= 0 || target <= 0)
                return 0
            const raw = ((target - entry) / entry) * 100.0
            return isLong ? raw : -raw
        }

        function priceToChartY(price) {
            const _mappingRevision = paperTradeOverlayRenderer.mappingRevision
            return paperTradeOverlayRenderer.screenYForPrice(Number(price))
        }

        Repeater {
            model: paperTradeOverlayModel.openOrders
            delegate: Item {
                anchors.fill: parent
                property real lineY: paperTradeOverlayLayer.priceToChartY(modelData.price)
                visible: lineY >= 0 && lineY <= parent.height
                onLineYChanged: paperTradeOverlayModel.logOrderOverlaySample(modelData.orderId, lineY, "line_y_changed")

                Rectangle {
                    id: orderPill
                    x: Math.max(8, parent.width - width - 18)
                    y: Math.max(6, Math.min(parent.height - height - 6, lineY - height * 0.5))
                    radius: 4
                    color: "#0d1422"
                    border.width: 1
                    border.color: modelData.side === "BUY" ? "#4f8cff" : "#f59b5a"
                    height: 22
                    width: orderRow.implicitWidth + 16

                    Row {
                        id: orderRow
                        anchors.centerIn: parent
                        spacing: 6

                        Text {
                            text: modelData.side === "BUY" ? "Buy" : "Sell"
                            color: modelData.side === "BUY" ? "#8fb3ff" : "#ffc187"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                        Text {
                            text: Number(modelData.qty).toFixed(4)
                            color: "#d9e2ff"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                        Text {
                            text: "Limit"
                            color: "#97a7d8"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                    }
                }
            }
        }

        Repeater {
            model: Object.keys(paperTradeOverlayModel.activePosition).length ? 1 : 0
            delegate: Item {
                id: positionOverlayItem
                anchors.fill: parent
                readonly property var pos: paperTradeOverlayModel.activePosition
                property real lineY: paperTradeOverlayLayer.priceToChartY(pos.entryPrice)
                property real markY: paperTradeOverlayLayer.priceToChartY(pos.markPrice)
                property bool isLong: pos.side === "LONG"
                property color lineColor: isLong ? "#4f8cff" : "#f26d6d"
                property color pnlColor: Number(pos.totalPnl) >= 0 ? "#5be39b" : "#ff8c82"
                property real topY: Math.max(0, Math.min(lineY, markY))
                property real bottomY: Math.min(parent.height, Math.max(lineY, markY))
                property real bandHeight: Math.max(1, bottomY - topY)
                property bool riskDragging: false
                visible: riskDragging || (lineY >= 0 && lineY <= parent.height) || (markY >= 0 && markY <= parent.height)
                onLineYChanged: paperTradeOverlayModel.logPositionOverlaySample(lineY, markY, "entry_line_y_changed")
                onMarkYChanged: paperTradeOverlayModel.logPositionOverlaySample(lineY, markY, "mark_line_y_changed")

                function dragPrice(mouseArea, mouseX, mouseY) {
                    const y = root.chartYFromItem(mouseArea, mouseX, mouseY)
                    paperTradeOverlayModel.updateRiskDrag(y)
                }

                function riskY(price) {
                    return paperTradeOverlayLayer.priceToChartY(price)
                }

                readonly property bool riskShowConfirm: paperTradeOverlayModel.riskConfirmVisible

                Rectangle {
                    id: positionPill
                    x: Math.max(8, parent.width - width - 18)
                    y: Math.max(6, Math.min(parent.height - height - 6, lineY - height * 0.5))
                    radius: 4
                    color: "#101724"
                    border.width: 1
                    border.color: lineColor
                    height: 24
                    width: positionRow.implicitWidth + 18

                    Row {
                        id: positionRow
                        anchors.centerIn: parent
                        spacing: 8

                        Text {
                            text: (isLong ? "Long " : "Short ") + paperTradeOverlayLayer.formatQty(pos.absQty)
                            color: "#d9e2ff"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                        Text {
                            text: paperTradeOverlayLayer.formatPnl(pos.openPnl) + " / " + paperTradeOverlayLayer.formatPct(pos.openPnlPct)
                            color: pnlColor
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                        Text {
                            text: "@ " + paperTradeOverlayLayer.formatPrice(pos.entryPrice)
                            color: "#98a9db"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                    }
                }

                Item {
                    id: positionActionRow
                    visible: paperTradeOverlayModel.canShowRiskControls()
                    x: Math.max(8, positionPill.x - width - 10)
                    y: Math.max(6, Math.min(parent.height - height - 6, lineY - height * 0.5))

                    readonly property real _spacing: 6
                    width: positionOverlayItem.riskShowConfirm
                           ? (discardRect.width + _spacing + confirmRect.width)
                           : (tpLauncherRect.width + _spacing + slLauncherRect.width)
                    height: positionOverlayItem.riskShowConfirm ? 24 : 22

                    Rectangle {
                        id: discardRect
                        visible: positionOverlayItem.riskShowConfirm
                        x: 0
                        height: 24
                        width: discardText.implicitWidth + 16
                        radius: 4
                        color: "#1d2230"
                        border.width: 1
                        border.color: "#5a647c"

                        Text {
                            id: discardText
                            anchors.centerIn: parent
                            text: "Discard"
                            color: "#d7dff7"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: paperTradeOverlayModel.discardStagedRisk()
                        }
                    }

                    Rectangle {
                        id: confirmRect
                        visible: positionOverlayItem.riskShowConfirm
                        x: discardRect.width + positionActionRow._spacing
                        height: 24
                        width: confirmText.implicitWidth + 16
                        radius: 4
                        color: "#3b6cff"
                        border.width: 1
                        border.color: "#7da2ff"

                        Text {
                            id: confirmText
                            anchors.centerIn: parent
                            text: "Confirm"
                            color: "#ffffff"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: paperTradeOverlayModel.confirmStagedRisk()
                        }
                    }

                    Rectangle {
                        id: tpLauncherRect
                        visible: !positionOverlayItem.riskShowConfirm
                        x: 0
                        height: 22
                        width: tpLauncherText.implicitWidth + 14
                        radius: 4
                        color: "#0d1422"
                        border.width: 1
                        border.color: "#13c9a7"

                        Text {
                            id: tpLauncherText
                            anchors.centerIn: parent
                            text: "TP"
                            color: "#13c9a7"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                    }

                    Rectangle {
                        id: slLauncherRect
                        visible: !positionOverlayItem.riskShowConfirm
                        x: tpLauncherRect.width + positionActionRow._spacing
                        height: 22
                        width: slLauncherText.implicitWidth + 14
                        radius: 4
                        color: "#0d1422"
                        border.width: 1
                        border.color: "#f0ab28"

                        Text {
                            id: slLauncherText
                            anchors.centerIn: parent
                            text: "SL"
                            color: "#f0ab28"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                    }
                }

                Rectangle {
                    id: tpPillRect
                    visible: paperTradeOverlayModel.riskState.hasTakeProfit
                    x: Math.max(8, parent.width - width - 18)
                    y: Math.max(6, Math.min(parent.height - height - 6, riskY(paperTradeOverlayModel.riskState.takeProfitPrice) - height * 0.5))
                    height: 22
                    width: tpRow.implicitWidth + 16
                    radius: 4
                    color: "#0d1422"
                    border.width: 1
                    border.color: "#13c9a7"

                    Row {
                        id: tpRow
                        anchors.centerIn: parent
                        spacing: 8

                        Text {
                            text: "TP"
                            color: "#13c9a7"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                        Text {
                            text: paperTradeOverlayLayer.formatPct(paperTradeOverlayLayer.percentBetween(pos.entryPrice, paperTradeOverlayModel.riskState.takeProfitPrice, isLong))
                            color: "#9df0dc"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                    }
                }

                Rectangle {
                    id: slPillRect
                    visible: paperTradeOverlayModel.riskState.hasStopLoss
                    x: Math.max(8, parent.width - width - 18)
                    y: Math.max(6, Math.min(parent.height - height - 6, riskY(paperTradeOverlayModel.riskState.stopLossPrice) - height * 0.5))
                    height: 22
                    width: slRow.implicitWidth + 16
                    radius: 4
                    color: "#0d1422"
                    border.width: 1
                    border.color: "#f0ab28"

                    Row {
                        id: slRow
                        anchors.centerIn: parent
                        spacing: 8

                        Text {
                            text: "SL"
                            color: "#f0ab28"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                        Text {
                            text: paperTradeOverlayLayer.formatPct(paperTradeOverlayLayer.percentBetween(pos.entryPrice, paperTradeOverlayModel.riskState.stopLossPrice, isLong))
                            color: "#ffd480"
                            font.pixelSize: 11
                            font.family: "Roboto Mono"
                        }
                    }
                }

                MouseArea {
                    id: riskDragCapture
                    anchors.fill: parent
                    z: 100
                    preventStealing: true

                    function hitTarget(mx, my) {
                        var targets = [
                            { item: tpLauncherRect, leg: "tp" },
                            { item: slLauncherRect, leg: "sl" },
                            { item: tpPillRect,     leg: "tp" },
                            { item: slPillRect,     leg: "sl" }
                        ]
                        for (var i = 0; i < targets.length; ++i) {
                            var t = targets[i]
                            if (!t.item.visible) continue
                            var p = t.item.mapToItem(positionOverlayItem, 0, 0)
                            if (mx >= p.x && mx <= p.x + t.item.width &&
                                my >= p.y && my <= p.y + t.item.height)
                                return t.leg
                        }
                        return ""
                    }

                    onPressed: function(mouse) {
                        var leg = hitTarget(mouse.x, mouse.y)
                        if (leg !== "" && paperTradeOverlayModel.beginRiskDrag(leg)) {
                            positionOverlayItem.riskDragging = true
                            positionOverlayItem.dragPrice(riskDragCapture, mouse.x, mouse.y)
                        } else {
                            mouse.accepted = false
                        }
                    }
                    onPositionChanged: function(mouse) {
                        if (pressed)
                            positionOverlayItem.dragPrice(riskDragCapture, mouse.x, mouse.y)
                    }
                    onReleased: function() {
                        positionOverlayItem.riskDragging = false
                        paperTradeOverlayModel.endRiskDrag()
                    }
                    onCanceled: function() {
                        positionOverlayItem.riskDragging = false
                        paperTradeOverlayModel.endRiskDrag()
                    }
                }
            }
        }
    }
    
    Item {
        id: gridLines
        anchors.fill: unifiedGridRenderer
        visible: root.showTimeGrid
        z: 3
        enabled: false

        function getXForTimePoint(timePoint) {
            var viewport = {
                timeStart_ms: unifiedGridRenderer.visibleTimeStart,
                timeEnd_ms: unifiedGridRenderer.visibleTimeEnd,
                priceMin: unifiedGridRenderer.minPrice,
                priceMax: unifiedGridRenderer.maxPrice,
                width: unifiedGridRenderer.width,
                height: unifiedGridRenderer.height
            };
            
            var timeRange = viewport.timeEnd_ms - viewport.timeStart_ms;
            var priceRange = viewport.priceMax - viewport.priceMin;
            
            if (timeRange <= 0 || priceRange <= 0) return 0;
            
            var normalizedTime = (timePoint - viewport.timeStart_ms) / timeRange;
            var normalizedPrice = (unifiedGridRenderer.minPrice - viewport.priceMin) / priceRange;
            
            normalizedTime = Math.max(0, Math.min(1, normalizedTime));
            normalizedPrice = Math.max(0, Math.min(1, normalizedPrice));
            
            var x = normalizedTime * viewport.width;
            var y = (1.0 - normalizedPrice) * viewport.height;
            
            return x;
        }
        
        function getTimePointForIndex(index, step, timeframe) {
            return unifiedGridRenderer.visibleTimeStart + (index * step * timeframe);
        }
        
        Repeater {
            id: gridRepeater
            model: root.showTimeGrid ? timeAxisModel : null
            
            Rectangle {
                width: 1
                height: parent.height
                color: model.isMajorTick ? "#F0F0F0" : "#DCDCDC"
                visible: root.showTimeGrid
                
                x: model.position + unifiedGridRenderer.panVisualOffset.x
            }
        }
    }
    
    Rectangle {
        id: priceAxis
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30
        anchors.topMargin: 10
        width: 90
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 5
        
        enabled: true
        clip: true

        PriceAxisModel {
            id: priceAxisModel
            target: unifiedGridRenderer
            tickSize: unifiedGridRenderer.heatmapTickSize
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeVerCursor

            property real lastY: 0

            onPressed: function(mouse) {
                lastY = mouse.y
            }
            onPositionChanged: function(mouse) {
                if (!pressed)
                    return
                const p = root.mapPriceAxisPoint(priceAxis, mouse.x, mouse.y)
                const deltaY = mouse.y - lastY
                lastY = mouse.y
                unifiedGridRenderer.zoomPriceAt(-deltaY * 24.0, p.y,
                                                unifiedGridRenderer.height)
            }
            onReleased: {}
            onCanceled: {}
        }

        WheelHandler {
            target: null

            onWheel: function(event) {
                const p = root.mapPriceAxisPoint(priceAxis, event.x, event.y)
                unifiedGridRenderer.zoomPriceAt(event.angleDelta.y, p.y,
                                                unifiedGridRenderer.height)
                event.accepted = true
            }
        }
    }
    
    Rectangle {
        id: timeAxis
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom  
        anchors.rightMargin: 90
        height: 30
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 2
        
        enabled: true
        clip: true

        TimeAxisModel {
            id: timeAxisModel
            target: unifiedGridRenderer
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeHorCursor

            property real lastX: 0

            onPressed: function(mouse) {
                lastX = mouse.x
            }
            onPositionChanged: function(mouse) {
                if (!pressed)
                    return
                const p = root.mapTimeAxisPoint(timeAxis, mouse.x, mouse.y)
                const deltaX = mouse.x - lastX
                lastX = mouse.x
                unifiedGridRenderer.zoomTimeAt(deltaX * 24.0, p.x,
                                               unifiedGridRenderer.width)
            }
            onReleased: {}
            onCanceled: {}
        }

        WheelHandler {
            target: null

            onWheel: function(event) {
                const p = root.mapTimeAxisPoint(timeAxis, event.x, event.y)
                unifiedGridRenderer.zoomTimeAt(event.angleDelta.y, p.x,
                                               unifiedGridRenderer.width)
                event.accepted = true
            }
        }
    }
    
    Rectangle {
        id: axisCorner
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 90
        height: 30
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 6
    }

    Rectangle {
        id: fpsOverlay
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: 8
        anchors.topMargin: 6
        color: Qt.rgba(0, 0, 0, 0.45)
        radius: 4
        z: 10
        visible: unifiedGridRenderer.showGpuStatsOverlay
        width: Math.max(fpsText.implicitWidth, debugText.implicitWidth) + 12
        height: fpsText.implicitHeight + debugText.implicitHeight + 10

        Text {
            id: fpsText
            anchors.top: parent.top
            anchors.topMargin: 4
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#9ef6ff"
            font.pixelSize: 12
            text: "FPS: 0.0"
        }

        Text {
            id: debugText
            anchors.top: fpsText.bottom
            anchors.topMargin: 2
            anchors.horizontalCenter: parent.horizontalCenter
            color: "#d5f7ff"
            font.pixelSize: 10
            text: ""
        }

        Timer {
            interval: 250
            repeat: true
            running: fpsOverlay.visible
            onTriggered: {
                fpsText.text = "FPS: " + unifiedGridRenderer.getCurrentFPS().toFixed(1);
                var zoom = unifiedGridRenderer.getZoomFactor();
                var t0 = unifiedGridRenderer.getVisibleTimeStart();
                var t1 = unifiedGridRenderer.getVisibleTimeEnd();
                var p0 = unifiedGridRenderer.getMinPrice();
                var p1 = unifiedGridRenderer.getMaxPrice();
                var pan = unifiedGridRenderer.getPanVisualOffset();
                debugText.text =
                    "Zoom: " + zoom.toFixed(2) +
                    "  Time: " + t0 + "→" + t1 +
                    "  Price: " + p0.toFixed(2) + "→" + p1.toFixed(2) +
                    "  Pan: (" + pan.x.toFixed(1) + "," + pan.y.toFixed(1) + ")";
            }
        }
    }

    Rectangle {
        id: gpuStatsOverlay
        anchors.left: parent.left
        anchors.top: fpsOverlay.bottom
        anchors.leftMargin: 8
        anchors.topMargin: 4
        color: Qt.rgba(0, 0, 0, 0.45)
        radius: 4
        z: 10
        visible: unifiedGridRenderer.showGpuStatsOverlay
        width: gpuStatsColumn.implicitWidth + 12
        height: gpuStatsColumn.implicitHeight + 10

        Column {
            id: gpuStatsColumn
            anchors.centerIn: parent
            spacing: 2

            Text {
                color: "#FFD700"
                font.pixelSize: 11
                font.bold: true
                text: "📊 Resource Usage"
            }

            Text {
                id: textureSizeText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Texture: N/A"
            }

            Text {
                id: textureMemText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Memory: N/A"
            }

            Text {
                id: textureFormatText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Format: N/A"
            }

            Text {
                id: uploadBandwidthText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Upload: 0.0 MB/s"
            }

            Text {
                id: ringCursorText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Ring Cursor: N/A"
            }

            Text {
                id: dirtyRegionsText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Dirty Regions: 0"
            }

            Text {
                id: labelRingText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Label ring: N/A"
            }

            Text {
                id: msdfAtlasText
                color: "#9ef6ff"
                font.pixelSize: 10
                text: "Glyph atlas: N/A"
            }
        }

        Timer {
            interval: 250
            repeat: true
            running: gpuStatsOverlay.visible
            onTriggered: {
                textureSizeText.text = "Texture: " + unifiedGridRenderer.getTextureSize();
                textureMemText.text = "Memory: " + unifiedGridRenderer.getTextureMemory();
                textureFormatText.text = "Format: " + unifiedGridRenderer.getTextureFormat();
                uploadBandwidthText.text = "Upload: " + unifiedGridRenderer.getUploadBandwidth().toFixed(2) + " MB/s";
                ringCursorText.text = "Ring Cursor: " + unifiedGridRenderer.getRingCursorInfo();
                dirtyRegionsText.text = "Dirty Regions: " + unifiedGridRenderer.getDirtyRegionCount();
                labelRingText.text = unifiedGridRenderer.getLabelRingMemory();
                msdfAtlasText.text = unifiedGridRenderer.getMsdfAtlasMemory();
            }
        }
    }

    Rectangle {
        id: dataPipelineOverlay
        anchors.left: parent.left
        anchors.top: viewportMathOverlay.visible
                     ? viewportMathOverlay.bottom
                     : (gpuStatsOverlay.visible ? gpuStatsOverlay.bottom : parent.top)
        anchors.leftMargin: 8
        anchors.topMargin: 4
        color: Qt.rgba(0, 0, 0, 0.45)
        radius: 4
        z: 10
        visible: unifiedGridRenderer.showDataPipelineOverlay
        width: dataPipelineText.implicitWidth + 12
        height: dataPipelineText.implicitHeight + 10

        Text {
            id: dataPipelineText
            anchors.centerIn: parent
            color: "#9ef6ff"
            font.pixelSize: 10
            font.family: "monospace"
            text: ""
        }

        Timer {
            interval: 250
            repeat: true
            running: dataPipelineOverlay.visible
            onTriggered: {
                dataPipelineText.text = unifiedGridRenderer.getDataPipelineDebug();
            }
        }
    }

    Rectangle {
        id: viewportMathOverlay
        anchors.left: parent.left
        anchors.top: gpuStatsOverlay.visible ? gpuStatsOverlay.bottom : parent.top
        anchors.leftMargin: 8
        anchors.topMargin: 4
        color: Qt.rgba(0, 0, 0, 0.45)
        radius: 4
        z: 10
        visible: unifiedGridRenderer.showViewportMathOverlay
        width: viewportMathText.implicitWidth + 12
        height: viewportMathText.implicitHeight + 10

        Text {
            id: viewportMathText
            anchors.centerIn: parent
            color: "#9ef6ff"
            font.pixelSize: 10
            font.family: "monospace"
            text: ""
        }

        Timer {
            interval: 250
            repeat: true
            running: viewportMathOverlay.visible
            onTriggered: {
                viewportMathText.text = unifiedGridRenderer.getViewportMathDebug();
            }
        }
    }

    focus: true
    Keys.onPressed: function(event) {
        switch(event.key) {
            case Qt.Key_R: unifiedGridRenderer.resetZoom(); event.accepted = true; break;
            case Qt.Key_A: unifiedGridRenderer.enableAutoScroll(!unifiedGridRenderer.autoScrollEnabled); event.accepted = true; break;
            case Qt.Key_Plus:
            case Qt.Key_Equal: unifiedGridRenderer.zoomIn(); event.accepted = true; break;
            case Qt.Key_Minus:
            case Qt.Key_Underscore: unifiedGridRenderer.zoomOut(); event.accepted = true; break;
            case Qt.Key_Left: unifiedGridRenderer.panLeft(); event.accepted = true; break;
            case Qt.Key_Right: unifiedGridRenderer.panRight(); event.accepted = true; break;
            case Qt.Key_Up: unifiedGridRenderer.panUp(); event.accepted = true; break;
            case Qt.Key_Down: unifiedGridRenderer.panDown(); event.accepted = true; break;
            case Qt.Key_G: 
                if (event.modifiers & Qt.ControlModifier) {
                    root.showTimeGrid = !root.showTimeGrid;
                    console.log("🔬 Grid lines:", root.showTimeGrid ? "ENABLED" : "DISABLED");
                    event.accepted = true;
                }
                break;
        }
    }
} 
