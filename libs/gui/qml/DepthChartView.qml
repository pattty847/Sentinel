import QtQuick 2.15
import QtQuick.Controls
import Sentinel 1.0

Rectangle {
    id: root
    color: uiTheme ? uiTheme.bg : "black"
    
    property string symbol: "BTC-USD"
    property bool stressTestMode: false
    property var chartModeController: null
    property bool gridModeEnabled: true  // Always true - pure grid-only mode!
    property bool showHeatmap: true
    property bool showCandles: false

    // uiTheme is provided by QmlSceneController
    
    // Track current active timeframe for button highlighting
    property int currentActiveTimeframe: 100  // Default
    
    //  ASSET-AWARE VOLUME SCALING: Dynamic range based on asset type
    property real maxVolumeRange: {
        if (symbol.includes("BTC")) return 100.0;  // BTC range: 0-100
        if (symbol.includes("ETH")) return 500.0;  // ETH range: 0-500
        if (symbol.includes("DOGE")) return 10000.0; // DOGE range: 0-10k
        return 1000.0; // Default range for other assets
    }
    
    // 🚨 DEBUG STATE: Track axis changes
    property real lastTimeSpan: 0
    property int lastTimeframe: 0
    
    // 🔬 VISUAL DEBUG: Grid line toggle (Ctrl+G to toggle)
    property bool showTimeGrid: true
    
    //  SIGNAL CONNECTIONS: Update axes when viewport or timeframe changes
    Connections {
        target: unifiedGridRenderer
        function onViewportChanged() {
            // QML property bindings will automatically recalculate 
            // when the underlying properties change - no explicit update needed
        }
        function onTimeframeChanged() {
            // QML property bindings will automatically recalculate 
            // when timeframeMs property changes - no explicit update needed
        }
        function onPanVisualOffsetChanged() {
            // Grid lines will update their positions via property bindings
        }
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
        anchors.rightMargin: 70  // Space for price axis (RIGHT side)
        anchors.bottomMargin: 30 // Space for time axis
        color: root.color
        visible: root.showHeatmap
        z: 0
    }

    UnifiedGridRenderer {
        id: unifiedGridRenderer
        objectName: "unifiedGridRenderer"
        anchors.fill: parent
        anchors.rightMargin: 70  // Space for price axis (RIGHT side)
        anchors.bottomMargin: 30 // Space for time axis
        visible: root.showHeatmap
        intensityScale: 1.0
        maxCells: 500000
        heatmapBackgroundColor: "black"
        z: 1
        
        // Update our tracked timeframe when it changes
        onTimeframeChanged: {
            root.currentActiveTimeframe = unifiedGridRenderer.timeframeMs
            console.log(" Timeframe changed to:", root.currentActiveTimeframe, "ms")
        }
        
        Component.onCompleted: {
            // Initialize with current timeframe
            root.currentActiveTimeframe = unifiedGridRenderer.timeframeMs
        }
    }

    CandleChartView {
        id: candleOverlay
        anchors.fill: unifiedGridRenderer
        visible: root.showCandles
    }
    
    // 🔬 VERTICAL GRID LINES: Visual confirmation of time column alignment
    Item {
        id: gridLines
        anchors.fill: unifiedGridRenderer
        visible: root.showTimeGrid
        z: 3  // Above chart and ensure visibility
        
        //  MOUSE EVENT TRANSPARENCY: Don't interfere with chart mouse events
        enabled: false  // Make this item transparent to mouse events
        
        //  COORDINATE SYSTEM INTEGRATION: Use proper CoordinateSystem
        function getXForTimePoint(timePoint) {
            // Use the same coordinate system as the heatmap cells (without visual pan offset)
            // Create viewport object matching the C++ CoordinateSystem
            var viewport = {
                timeStart_ms: unifiedGridRenderer.visibleTimeStart,
                timeEnd_ms: unifiedGridRenderer.visibleTimeEnd,
                priceMin: unifiedGridRenderer.minPrice,
                priceMax: unifiedGridRenderer.maxPrice,
                width: unifiedGridRenderer.width,
                height: unifiedGridRenderer.height
            };
            
            // Calculate normalized coordinates (same as CoordinateSystem::worldToScreen)
            var timeRange = viewport.timeEnd_ms - viewport.timeStart_ms;
            var priceRange = viewport.priceMax - viewport.priceMin;
            
            if (timeRange <= 0 || priceRange <= 0) return 0;
            
            var normalizedTime = (timePoint - viewport.timeStart_ms) / timeRange;
            var normalizedPrice = (unifiedGridRenderer.minPrice - viewport.priceMin) / priceRange;
            
            // Clamp to viewport bounds
            normalizedTime = Math.max(0, Math.min(1, normalizedTime));
            normalizedPrice = Math.max(0, Math.min(1, normalizedPrice));
            
            // Convert to screen coordinates (same as CoordinateSystem)
            var x = normalizedTime * viewport.width;
            var y = (1.0 - normalizedPrice) * viewport.height;  // Flip Y for screen coordinates
            
            return x;
        }
        
        function getTimePointForIndex(index, step, timeframe) {
            return unifiedGridRenderer.visibleTimeStart + (index * step * timeframe);
        }
        
        //  DYNAMIC GRID LINES: Synchronized with TimeAxisModel; move with pan for immediate feedback
        Repeater {
            id: gridRepeater
            model: root.showTimeGrid ? timeAxisModel : null
            
            Rectangle {
                width: 1
                height: parent.height
                color: model.isMajorTick ? "#F0F0F0" : "#DCDCDC"
                visible: root.showTimeGrid
                
                x: model.position
            }
        }
    }
    
    //  PRICE AXIS (Y-AXIS) - RIGHT SIDE with PriceAxisModel
    Rectangle {
        id: priceAxis
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30
        anchors.topMargin: 10  // Prevent label clipping at top
        width: 70
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 2
        
        enabled: false  // Transparent to mouse events
        
        // Clip labels to axis bounds
        clip: true
        
        PriceAxisModel {
            id: priceAxisModel
            target: unifiedGridRenderer
            tickSize: unifiedGridRenderer.heatmapTickSize
        }
        
        Repeater {
            model: priceAxisModel
            
            Item {
                width: priceAxis.width
                height: 16
                // Apply pan offset so axis labels move immediately with mouse drag
                // Model position is already the row center (in screen px).
                // Nudge down slightly so text center aligns visually with row center.
                y: model.position - 8 + 1.5
                
                // Only show if within clipped bounds (with margin)
                visible: y >= -4 && y <= priceAxis.height - 12
                
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
    
    //  TIME AXIS (X-AXIS) - BOTTOM with TimeAxisModel
    Rectangle {
        id: timeAxis
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom  
        anchors.rightMargin: 70  // Match price axis width
        height: 30
        color: Qt.rgba(0.05, 0.05, 0.1, 0.85)
        border.color: Qt.rgba(1, 1, 1, 0.3)
        border.width: 1
        z: 2
        
        enabled: false  // Transparent to mouse events
        
        // Clip labels to axis bounds
        clip: true
        
        TimeAxisModel {
            id: timeAxisModel
            target: unifiedGridRenderer
        }
        
        Repeater {
            model: timeAxisModel
            
            Item {
                width: 80
                height: timeAxis.height
                // Apply pan offset so axis labels move immediately with mouse drag
                x: model.position - 40
                
                // Only show if within clipped bounds
                visible: x >= -40 && x <= timeAxis.width - 40
                
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
    
    // Corner piece where axes meet (bottom-right)
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

    // FPS overlay (polls QML invokable for now).
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

    // GPU Stats overlay (Tier 1 debug info)
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
                id: glyphAtlasText
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
                glyphAtlasText.text = unifiedGridRenderer.getGlyphAtlasMemory();
            }
        }
    }

    // Data Pipeline overlay (Tier 1 debug info)
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

    // Viewport Math overlay (Tier 1 debug info)
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

    // Control UI removed in favor of top toolbar (chart-only controls)

    // Keyboard Shortcuts
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
