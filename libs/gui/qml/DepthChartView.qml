import QtQuick 2.15
import QtQuick.Controls 2.15
import Sentinel.Charts 1.0
import "controls"

Rectangle {
    id: root
    color: "black"
    
    property string symbol: "BTC-USD"
    property bool stressTestMode: false
    property var chartModeController: null
    property bool gridModeEnabled: true  // Always true - pure grid-only mode!
    
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
    
    UnifiedGridRenderer {
        id: unifiedGridRenderer
        objectName: "unifiedGridRenderer"
        anchors.fill: parent
        anchors.leftMargin: 60   // Space for price axis
        anchors.bottomMargin: 30 // Space for time axis
        visible: true
        renderMode: UnifiedGridRenderer.LiquidityHeatmap
        showVolumeProfile: true
        intensityScale: 1.0
        maxCells: 50000
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
        
        //  DYNAMIC GRID LINES: Synchronized with TimeAxisModel
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
    
    //  PRICE AXIS (Y-AXIS) - LEFT SIDE with PriceAxisModel
    Rectangle {
        id: priceAxis
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30
        width: 60
        color: Qt.rgba(0, 0, 0, 0.3)
        border.color: "white"
        border.width: 1
        z: 2
        
        enabled: false  // Transparent to mouse events
        
        PriceAxisModel {
            id: priceAxisModel
        }
        
        Repeater {
            model: priceAxisModel
            Rectangle {
                width: priceAxis.width - 10
                height: 20
                color: "transparent"
                border.color: Qt.rgba(1, 1, 1, 0.3)
                border.width: model.isMajorTick ? 1 : 0
                
                y: model.position
                
                Text {
                    anchors.centerIn: parent
                    color: "#00FF00"
                    font.pixelSize: 10
                    font.bold: true
                    text: model.label
                }
            }
        }
        
        Text {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 5
            color: "white"
            font.pixelSize: 11
            font.bold: true
            text: "PRICE"
            rotation: -90
        }
    }
    
    //  TIME AXIS (X-AXIS) - BOTTOM with TimeAxisModel
    Rectangle {
        id: timeAxis
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom  
        anchors.leftMargin: 60
        height: 30
        color: Qt.rgba(0, 0, 0, 0.3)
        border.color: "white"
        border.width: 1
        z: 2
        
        enabled: false  // Transparent to mouse events
        
        TimeAxisModel {
            id: timeAxisModel
        }
        
        Repeater {
            model: timeAxisModel
            Rectangle {
                width: 80
                height: timeAxis.height - 10
                color: "transparent"
                border.color: Qt.rgba(1, 1, 1, 0.3)
                border.width: model.isMajorTick ? 1 : 0
                
                x: model.position
                
                Text {
                    anchors.centerIn: parent
                    color: "#00FFFF"
                    font.pixelSize: 9
                    font.bold: true
                    text: model.label
                }
            }
        }
        
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 5
            color: "white"
            font.pixelSize: 11
            font.bold: true
            text: "TIME →"
        }
    }

    // Control Panel (top right) - Refactored to use modular components
    Column {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        spacing: 5
        z: 10
        
        //  MOUSE EVENT ISOLATION: Ensure controls don't interfere with chart
        // The controls have their own MouseArea components that handle their events

        NavigationControls {
            target: unifiedGridRenderer
        }
        
        Text {
            width: 150
            text: " Active: " + root.currentActiveTimeframe + "ms"
            color: "#00FF00"
            font.pixelSize: 12
            font.bold: true
            wrapMode: Text.WordWrap
        }
        
        TimeframeSelector {
            target: unifiedGridRenderer
        }
        
        PriceResolutionSelector {
            target: unifiedGridRenderer
        }
        
        VolumeFilter {
            target: unifiedGridRenderer
            maxVolumeRange: root.maxVolumeRange
        }
        
        GridResolutionSelector {
            target: unifiedGridRenderer
        }
        
        // Layer Toggle Controls
        Column {
            spacing: 3
            
            Text {
                text: "Render Layers"
                color: "#FFD700"
                font.pixelSize: 10
                font.bold: true
            }
            
            Row {
                spacing: 8
                Rectangle {
                    width: 16; height: 16
                    border.color: "white"
                    color: unifiedGridRenderer.showHeatmapLayer ? "#00FF00" : "transparent"
                    radius: 2
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: unifiedGridRenderer.showHeatmapLayer = !unifiedGridRenderer.showHeatmapLayer
                    }
                }
                Text { text: "Heatmap"; color: "white"; font.pixelSize: 9 }
            }
            
            Row {
                spacing: 8
                Rectangle {
                    width: 16; height: 16
                    border.color: "white"
                    color: unifiedGridRenderer.showTradeBubbleLayer ? "#00FFFF" : "transparent"
                    radius: 2
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: unifiedGridRenderer.showTradeBubbleLayer = !unifiedGridRenderer.showTradeBubbleLayer
                    }
                }
                Text { text: "Trade Bubbles"; color: "white"; font.pixelSize: 9 }
            }
            
            Row {
                spacing: 8
                Rectangle {
                    width: 16; height: 16
                    border.color: "white"
                    color: unifiedGridRenderer.showTradeFlowLayer ? "#FF8800" : "transparent"
                    radius: 2
                    
                    MouseArea {
                        anchors.fill: parent
                        onClicked: unifiedGridRenderer.showTradeFlowLayer = !unifiedGridRenderer.showTradeFlowLayer
                    }
                }
                Text { text: "Trade Flow"; color: "white"; font.pixelSize: 9 }
            }
        }
        
        // Trade Bubble Controls (only visible when bubble layer is active)
        Column {
            spacing: 2
            visible: unifiedGridRenderer.showTradeBubbleLayer
            
            Text {
                text: "Bubble Settings"
                color: "#FFD700"
                font.pixelSize: 10
                font.bold: true
            }
            
            Row {
                spacing: 5
                Text { text: "Size:"; color: "white"; font.pixelSize: 9; width: 30 }
                Slider {
                    width: 80
                    from: 2; to: 30; stepSize: 1
                    value: unifiedGridRenderer.minBubbleRadius
                    onValueChanged: unifiedGridRenderer.minBubbleRadius = value
                }
                Text { text: unifiedGridRenderer.minBubbleRadius.toFixed(0); color: "white"; font.pixelSize: 9; width: 20 }
            }
            
            Row {
                spacing: 5
                Text { text: "Max:"; color: "white"; font.pixelSize: 9; width: 30 }
                Slider {
                    width: 80
                    from: 10; to: 50; stepSize: 1
                    value: unifiedGridRenderer.maxBubbleRadius
                    onValueChanged: unifiedGridRenderer.maxBubbleRadius = value
                }
                Text { text: unifiedGridRenderer.maxBubbleRadius.toFixed(0); color: "white"; font.pixelSize: 9; width: 20 }
            }
            
            Row {
                spacing: 5
                Text { text: "Alpha:"; color: "white"; font.pixelSize: 9; width: 30 }
                Slider {
                    width: 80
                    from: 0.1; to: 1.0; stepSize: 0.05
                    value: unifiedGridRenderer.bubbleOpacity
                    onValueChanged: unifiedGridRenderer.bubbleOpacity = value
                }
                Text { text: (unifiedGridRenderer.bubbleOpacity * 100).toFixed(0) + "%"; color: "white"; font.pixelSize: 9; width: 20 }
            }
        }
    }

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