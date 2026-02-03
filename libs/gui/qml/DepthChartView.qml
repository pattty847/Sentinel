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
    property bool showHeatmap: true
    property bool showCandles: false

    property int currentActiveTimeframe: 100

    // Asset-specific volume ranges (BTC/ETH/DOGE differ by orders of magnitude)
    property real maxVolumeRange: {
        if (symbol.includes("BTC")) return 100.0;
        if (symbol.includes("ETH")) return 500.0;
        if (symbol.includes("DOGE")) return 10000.0;
        return 1000.0;
    }
    
    property real lastTimeSpan: 0
    property int lastTimeframe: 0
    property bool showTimeGrid: false

    Connections {
        target: unifiedGridRenderer
        function onViewportChanged() {}
        function onTimeframeChanged() {}
        function onPanVisualOffsetChanged() {}
    }

    Connections {
        target: chartModeController
        function onComponentVisibilityChanged(component, visible) {
            if (component === "candles") {
                root.showCandles = visible
            } else if (component === "orderBook") {
                root.showHeatmap = visible
            }
        }
    }
    
    Rectangle {
        id: heatmapBackground
        anchors.fill: parent
        anchors.rightMargin: 70
        anchors.bottomMargin: 30
        color: root.color
        visible: root.showHeatmap
        z: 0
    }

    UnifiedGridRenderer {
        id: unifiedGridRenderer
        objectName: "unifiedGridRenderer"
        anchors.fill: parent
        anchors.rightMargin: 70
        anchors.bottomMargin: 30
        visible: root.showHeatmap
        intensityScale: 1.0
        maxCells: 500000
        heatmapBackgroundColor: "black"
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
        viewState: unifiedGridRenderer.viewState
        candleBuffer: dataSource ? dataSource.candleBuffer : null
        symbol: root.symbol
        timeframeSec: 1
        z: 2
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
        width: 70
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 2
        
        enabled: false
        clip: true

        PriceAxisModel {
            id: priceAxisModel
            target: unifiedGridRenderer
            tickSize: unifiedGridRenderer.heatmapTickSize
        }
        
        Item {
            id: priceAxisLabels
            anchors.fill: parent
            
            Repeater {
                model: priceAxisModel
                
                Item {
                    width: priceAxis.width
                    height: 16
                    y: model.position - 8 + 1.5
                    visible: model.label !== ""
                    
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        color: "#E0E0E0"
                        font.pixelSize: 10
                        font.family: "monospace"
                        text: model.label
                    }
                }
            }
        }
    }
    
    Rectangle {
        id: timeAxis
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom  
        anchors.rightMargin: 70
        height: 30
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 2
        
        enabled: false
        clip: true

        TimeAxisModel {
            id: timeAxisModel
            target: unifiedGridRenderer
        }
        
        Item {
            id: timeAxisLabels
            anchors.fill: parent
            
            Repeater {
                model: timeAxisModel
                
                Item {
                    width: 80
                    height: timeAxis.height
                    x: model.position - 40
                    visible: model.label !== ""
                    
                    Text {
                        anchors.centerIn: parent
                        color: "#E0E0E0"
                        font.pixelSize: 10
                        font.family: "monospace"
                        text: model.label
                    }
                }
            }
        }
    }
    
    Rectangle {
        id: axisCorner
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 70
        height: 30
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 3
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
