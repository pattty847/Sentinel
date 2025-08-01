import QtQuick 2.15
import Sentinel.Charts 1.0

Rectangle {
    id: root
    color: "black"
    
    property string symbol: "BTC-USD"
    property bool stressTestMode: false
    property var chartModeController: null
    property bool gridModeEnabled: true  // Always true - pure grid-only mode!
    
    // Track current active timeframe for button highlighting
    property int currentActiveTimeframe: 100  // Default
    
    // 🚀 ASSET-AWARE VOLUME SCALING: Dynamic range based on asset type
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
    
    // 🚀 SIGNAL CONNECTIONS: Update axes when viewport or timeframe changes
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
            console.log("🎯 Timeframe changed to:", root.currentActiveTimeframe, "ms")
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
        
        // 🎯 GRID LINE HELPER FUNCTIONS
        function getXForTimePoint(timePoint) {
            if (unifiedGridRenderer.visibleTimeEnd <= unifiedGridRenderer.visibleTimeStart) return 0;
            var relativeTime = timePoint - unifiedGridRenderer.visibleTimeStart;
            var timeSpan = unifiedGridRenderer.visibleTimeEnd - unifiedGridRenderer.visibleTimeStart;
            var positionRatio = relativeTime / timeSpan;
            return positionRatio * width;
        }
        
        function getTimePointForIndex(index, step, timeframe) {
            return unifiedGridRenderer.visibleTimeStart + (index * step * timeframe);
        }
        
        // 🔄 DYNAMIC GRID LINES: Recreate when viewport/timeframe changes
        Repeater {
            id: gridRepeater
            model: root.showTimeGrid ? 32 : 0  // Max 32 grid lines to avoid spam
            
            Rectangle {
                width: 1
                height: parent.height
                color: (index % 5 === 0) ? "#F0F0F0" : "#DCDCDC"
                visible: root.showTimeGrid && unifiedGridRenderer.visibleTimeEnd > 0
                
                x: {
                    if (unifiedGridRenderer.visibleTimeEnd <= 0 || unifiedGridRenderer.timeframeMs <= 0) return 0;
                    
                    // Calculate time point for this grid line
                    var timeSpan = unifiedGridRenderer.visibleTimeEnd - unifiedGridRenderer.visibleTimeStart;
                    var timeframe = unifiedGridRenderer.timeframeMs;
                    var bucketCount = Math.max(1, Math.floor(timeSpan / timeframe));
                    var step = Math.max(1, Math.ceil(bucketCount / 16));  // Same logic as axis
                    
                    var timePoint = parent.getTimePointForIndex(index, step, timeframe);
                    var baseX = parent.getXForTimePoint(timePoint);
                    
                    // 🚀 REAL-TIME SYNC: Apply visual pan offset for smooth dragging
                    return baseX + unifiedGridRenderer.panVisualOffset.x;
                }
            }
        }
    }
    
    // 📊 PRICE AXIS (Y-AXIS) - LEFT SIDE
    Rectangle {
        id: priceAxis
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30
        width: 60
        color: Qt.rgba(0, 0, 0, 0.8)
        border.color: "white"
        border.width: 1
        z: 5
        
        Column {
            anchors.fill: parent
            anchors.margins: 5
            
            // 🎯 DYNAMIC PRICE TICK CALCULATION: Nice price intervals
            property double priceRange: {
                var minPrice = unifiedGridRenderer.minPrice;
                var maxPrice = unifiedGridRenderer.maxPrice;
                
                if (minPrice == 0 && maxPrice == 0) {
                    return 1000; // Fallback range
                }
                
                return maxPrice - minPrice;
            }
            property double rawTickSpacing: priceRange / 12  // Target ~12 ticks
            property double niceTickSpacing: getNiceNumber(rawTickSpacing)
            property int tickCount: Math.max(1, Math.min(16, Math.ceil(priceRange / niceTickSpacing)))
            
            function getNiceNumber(raw) {
                if (raw <= 0) return 1.0;
                var exponent = Math.floor(Math.log10(raw));
                var fraction = raw / Math.pow(10, exponent);
                var niceFraction;
                if (fraction < 1.5) niceFraction = 1;
                else if (fraction < 3) niceFraction = 2;
                else if (fraction < 7) niceFraction = 5;
                else niceFraction = 10;
                return niceFraction * Math.pow(10, exponent);
            }
            
            Repeater {
                model: parent.tickCount
                Rectangle {
                    width: parent.width - 10
                    height: parent.height / parent.tickCount
                    color: "transparent"
                    border.color: Qt.rgba(1, 1, 1, 0.3)
                    border.width: 1
                    
                    // 🎯 SCOPE FIX: Store tick spacing accessible to Text component
                    property double priceTickSpacing: parent.niceTickSpacing
                    
                    Text {
                        anchors.centerIn: parent
                        color: "#00FF00"
                        font.pixelSize: 10
                        font.bold: true
                        text: {
                            if (unifiedGridRenderer.maxPrice > 0 && parent.priceTickSpacing > 0) {
                                // 🎯 NICE PRICE TICKS: Use calculated nice intervals
                                var basePrice = unifiedGridRenderer.minPrice;
                                var tickSpacing = parent.priceTickSpacing;
                                var priceLevel = basePrice + (index * tickSpacing);
                                
                                if (isNaN(priceLevel)) {
                                    return "$N/A";
                                }
                                
                                // Format based on price magnitude
                                if (priceLevel >= 1000) {
                                    return "$" + priceLevel.toFixed(0);
                                } else if (priceLevel >= 1) {
                                    return "$" + priceLevel.toFixed(2);
                                } else {
                                    return "$" + priceLevel.toFixed(4);
                                }
                            } else {
                                return "$0.00"; // Fallback when no data
                            }
                        }
                    }
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
    
    // ⏰ TIME AXIS (X-AXIS) - BOTTOM
    Rectangle {
        id: timeAxis
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom  
        anchors.leftMargin: 60
        height: 30
        color: Qt.rgba(0, 0, 0, 0.8)
        border.color: "white"
        border.width: 1
        z: 5
        
        Row {
            anchors.fill: parent
            anchors.margins: 5
            
            // 🚀 REAL-TIME SYNC: Apply visual pan offset for smooth dragging
            transform: Translate {
                x: unifiedGridRenderer.panVisualOffset.x
            }
            
            // 🎯 DYNAMIC TICK CALCULATION: Perfectly aligned with grid buckets
            property int bucketCount: {
                var timeSpan = unifiedGridRenderer.visibleTimeEnd - unifiedGridRenderer.visibleTimeStart;
                var timeframe = unifiedGridRenderer.timeframeMs;
                
                if (timeSpan <= 0 || timeframe <= 0) {
                    return 8; // Fallback to original count
                }
                
                return Math.max(1, Math.floor(timeSpan / timeframe));
            }
            property int maxTicks: 16  // Don't spam the UI
            property int step: (bucketCount > 0 && maxTicks > 0) ? Math.max(1, Math.ceil(bucketCount / maxTicks)) : 1
            property int tickCount: (bucketCount > 0 && step > 0) ? Math.max(1, Math.ceil(bucketCount / step)) : 8
            
            // 🔧 STEP CALCULATION DEBUG: Explicit step calculation to avoid race conditions
            function calculateStepSafely() {
                if (bucketCount === 0 || maxTicks === 0) {
                    console.warn("⚠️ Cannot calculate step — bucketCount=" + bucketCount + " maxTicks=" + maxTicks);
                    return 1;
                }
                var stepSize = Math.max(1, Math.ceil(bucketCount / maxTicks));
                console.log("✅ STEP CALC: bucketCount=" + bucketCount + " / maxTicks=" + maxTicks + " = " + stepSize);
                return stepSize;
            }
            
            // 🎯 PROPER TIME FORMATTING: Dynamic format based on timeframe
            function formatTimeLabel(timePoint, timeframe) {
                var d = new Date(timePoint);
                var h = d.getHours().toString().padStart(2, '0');
                var m = d.getMinutes().toString().padStart(2, '0');
                var s = d.getSeconds().toString().padStart(2, '0');
                var ms = d.getMilliseconds().toString().padStart(3, '0');

                // Dynamically switch formatting based on timeframe
                if (timeframe < 1000) return m + ":" + s + "." + ms;     // MM:SS.mmm for <1s
                if (timeframe < 60000) return h + ":" + m + ":" + s;     // HH:MM:SS for <1min  
                return h + ":" + m;                                      // HH:MM for 1min+
            }
            
            // 🚨 SMART DEBUG: Only log when axis config changes
            property string debugInfo: {
                var timeSpan = unifiedGridRenderer.visibleTimeEnd - unifiedGridRenderer.visibleTimeStart;
                var timeframe = unifiedGridRenderer.timeframeMs;
                var spanSeconds = (timeSpan / 1000).toFixed(1);
                var info = "AXIS: " + spanSeconds + "s viewport, " + timeframe + "ms buckets, " 
                         + bucketCount + " total, step=" + step + ", ticks=" + tickCount;
                
                // Only log on significant changes
                if (Math.abs(timeSpan - (root.lastTimeSpan || 0)) > 5000 || 
                    timeframe !== (root.lastTimeframe || 0)) {
                    console.log("🎯 " + info);
                    root.lastTimeSpan = timeSpan;
                    root.lastTimeframe = timeframe;
                }
                return info;
            }
            
            Repeater {
                model: parent.tickCount
                Rectangle {
                    width: parent.width / parent.tickCount
                    height: parent.height - 10
                    color: "transparent"
                    border.color: Qt.rgba(1, 1, 1, 0.3)
                    border.width: 1
                    
                    // 🎯 SCOPE FIX: Store step value accessible to Text component
                    property int timeStep: parent.step
                    
                    // 🎯 PROPER TIME FORMATTING: Make function accessible to Text
                    function formatTime(timePoint, timeframe) {
                        return parent.formatTimeLabel(timePoint, timeframe);
                    }
                    
                    Text {
                        anchors.centerIn: parent
                        color: "#00FFFF"
                        font.pixelSize: 9
                        font.bold: true
                        text: {
                            if (unifiedGridRenderer.visibleTimeEnd > 0 && unifiedGridRenderer.timeframeMs > 0) {
                                // 🎯 GRID-ALIGNED AXIS: Each tick = exactly one timeframe bucket
                                var currentTimeframe = unifiedGridRenderer.timeframeMs;
                                var startTime = unifiedGridRenderer.visibleTimeStart;
                                var step = parent.timeStep;
                                
                                // 🔧 RACE CONDITION FIX: Validate step before use
                                if (!step || step <= 0) {
                                    console.warn("⚠️ TIME CALC: Invalid step=" + step + ", using fallback");
                                    step = 1;
                                }
                                
                                // Each tick represents step * timeframe buckets
                                var timePoint = startTime + (index * step * currentTimeframe);
                                
                                if (isNaN(timePoint) || timePoint <= 0) {
                                    return "N/A";
                                }
                                
                                // 🎯 USE PROPER TIME FORMATTING FUNCTION
                                return parent.formatTime(timePoint, currentTimeframe);
                            } else {
                                return "00:00"; // Fallback when no data
                            }
                        }
                    }
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

    // Control Panel (top right)
    Column {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        spacing: 5
        z: 10

        Rectangle {
            width: 150
            height: 30
            color: Qt.rgba(0, 0, 0, 0.7)
            border.color: "cyan"
            border.width: 1
            radius: 5
            Text {
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 12
                text: "🔍 ZOOM CONTROLS"
            }
        }
        Row {
            spacing: 5
            Rectangle {
                width: 35
                height: 25
                color: Qt.rgba(0, 0.4, 0, 0.8)
                border.color: "lime"
                border.width: 1
                radius: 3
                Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 10; text: "+" }
                MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.zoomIn() }
            }
            Rectangle {
                width: 35
                height: 25
                color: Qt.rgba(0.4, 0, 0, 0.8)
                border.color: "red"
                border.width: 1
                radius: 3
                Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 10; text: "-" }
                MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.zoomOut() }
            }
            Rectangle {
                width: 35
                height: 25
                color: Qt.rgba(0, 0, 0.4, 0.8)
                border.color: "cyan"
                border.width: 1
                radius: 3
                Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 10; text: "RST" }
                MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.resetZoom() }
            }
            Rectangle {
                width: 35
                height: 25
                color: unifiedGridRenderer.autoScrollEnabled ? Qt.rgba(0, 0.4, 0, 0.8) : Qt.rgba(0.4, 0, 0, 0.8)
                border.color: unifiedGridRenderer.autoScrollEnabled ? "lime" : "red"
                border.width: 1
                radius: 3
                Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 10; text: "AUTO" }
                MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.enableAutoScroll(!unifiedGridRenderer.autoScrollEnabled) }
            }
        }
        Text {
            width: 150
            text: "✅ Active: " + root.currentActiveTimeframe + "ms"
            color: "#00FF00"
            font.pixelSize: 12
            font.bold: true
            wrapMode: Text.WordWrap
        }
        // Timeframe Controls
        Column {
            spacing: 5
            z: 10
            Text { text: "Timeframe (ms)"; color: "white"; font.pixelSize: 12 }
            Text { text: "Select aggregation timeframe:"; color: "#CCCCCC"; font.pixelSize: 10; visible: true }
            Row {
                spacing: 3
                Rectangle {
                    width: 40; height: 25
                    color: root.currentActiveTimeframe === 100 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
                    border.color: root.currentActiveTimeframe === 100 ? "white" : "white"
                    border.width: root.currentActiveTimeframe === 100 ? 2 : 1
                    radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "100"; font.bold: root.currentActiveTimeframe === 100 }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setTimeframe(100) }
                }
                Rectangle {
                    width: 40; height: 25
                    color: root.currentActiveTimeframe === 250 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
                    border.color: root.currentActiveTimeframe === 250 ? "white" : "white"
                    border.width: root.currentActiveTimeframe === 250 ? 2 : 1
                    radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "250"; font.bold: root.currentActiveTimeframe === 250 }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setTimeframe(250) }
                }
                Rectangle {
                    width: 40; height: 25
                    color: root.currentActiveTimeframe === 500 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
                    border.color: root.currentActiveTimeframe === 500 ? "white" : "white"
                    border.width: root.currentActiveTimeframe === 500 ? 2 : 1
                    radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "500"; font.bold: root.currentActiveTimeframe === 500 }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setTimeframe(500) }
                }
            }
            Row {
                spacing: 3
                Rectangle {
                    width: 40; height: 25
                    color: root.currentActiveTimeframe === 1000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
                    border.color: root.currentActiveTimeframe === 1000 ? "white" : "white"
                    border.width: root.currentActiveTimeframe === 1000 ? 2 : 1
                    radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "1s"; font.bold: root.currentActiveTimeframe === 1000 }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setTimeframe(1000) }
                }
                Rectangle {
                    width: 40; height: 25
                    color: root.currentActiveTimeframe === 2000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
                    border.color: root.currentActiveTimeframe === 2000 ? "white" : "white"
                    border.width: root.currentActiveTimeframe === 2000 ? 2 : 1
                    radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "2s"; font.bold: root.currentActiveTimeframe === 2000 }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setTimeframe(2000) }
                }
                Rectangle {
                    width: 40; height: 25
                    color: root.currentActiveTimeframe === 5000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
                    border.color: root.currentActiveTimeframe === 5000 ? "white" : "white"
                    border.width: root.currentActiveTimeframe === 5000 ? 2 : 1
                    radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 9; text: "5s"; font.bold: root.currentActiveTimeframe === 5000 }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setTimeframe(5000) }
                }
            }
        }
        // 🚀 DYNAMIC GRID CONTROLS - PRICE RESOLUTION
        Column {
            spacing: 5
            z: 10
            
            Rectangle {
                width: 150
                height: 25
                color: Qt.rgba(0, 0, 0, 0.7)
                border.color: "orange"
                border.width: 1
                radius: 5
                Text {
                    anchors.centerIn: parent
                    color: "white"
                    font.pixelSize: 12
                    font.bold: true
                    text: "💰 PRICE RESOLUTION"
                }
            }
            
            Text { 
                text: "Price Bucket: $" + unifiedGridRenderer.getCurrentPriceResolution().toFixed(2)
                color: "#FFA500"; font.pixelSize: 11; font.bold: true 
            }
            
            Rectangle {
                width: 150
                height: 25
                color: Qt.rgba(0, 0, 0, 0.5)
                border.color: "orange"
                border.width: 1
                radius: 3
                
                Rectangle {
                    id: priceSliderHandle
                    width: 15
                    height: 23
                    color: "orange"
                    border.color: "white"
                    border.width: 1
                    radius: 2
                    x: Math.max(0, Math.min(parent.width - width, 
                        (Math.log(unifiedGridRenderer.getCurrentPriceResolution() / 0.01) / Math.log(100)) * (parent.width - width)))
                    
                    MouseArea {
                        anchors.fill: parent
                        drag.target: parent
                        drag.axis: Drag.XAxis
                        drag.minimumX: 0
                        drag.maximumX: parent.parent.width - parent.width
                        
                        onPositionChanged: {
                            if (drag.active) {
                                var ratio = parent.x / (parent.parent.width - parent.width)
                                var resolution = 0.01 * Math.pow(100, ratio) // Range from $0.01 to $1.00
                                unifiedGridRenderer.setPriceResolution(resolution)
                            }
                        }
                    }
                }
            }
            
            Row {
                spacing: 3
                Text { text: "$0.01"; color: "#FFA500"; font.pixelSize: 9 }
                Text { text: "$0.10"; color: "#FFA500"; font.pixelSize: 9 }
                Text { text: "$1.00"; color: "#FFA500"; font.pixelSize: 9 }
            }
        }
        
        // 🚀 DYNAMIC GRID CONTROLS - VOLUME FILTER
        Column {
            spacing: 5
            z: 10
            
            Rectangle {
                width: 150
                height: 25
                color: Qt.rgba(0, 0, 0, 0.7)
                border.color: "lime"
                border.width: 1
                radius: 5
                Text {
                    anchors.centerIn: parent
                    color: "white"
                    font.pixelSize: 12
                    font.bold: true
                    text: "🔍 VOLUME FILTER"
                }
            }
            
            Text { 
                text: "Min Volume: " + unifiedGridRenderer.minVolumeFilter.toFixed(2)
                color: "#00FF00"; font.pixelSize: 11; font.bold: true 
            }
            
            Rectangle {
                width: 150
                height: 25
                color: Qt.rgba(0, 0, 0, 0.5)
                border.color: "lime"
                border.width: 1
                radius: 3
                
                Rectangle {
                    id: volumeSliderHandle
                    width: 15
                    height: 23
                    color: "lime"
                    border.color: "white"
                    border.width: 1
                    radius: 2
                    x: Math.max(0, Math.min(parent.width - width, 
                        (unifiedGridRenderer.minVolumeFilter / root.maxVolumeRange) * (parent.width - width)))
                    
                    MouseArea {
                        anchors.fill: parent
                        drag.target: parent
                        drag.axis: Drag.XAxis
                        drag.minimumX: 0
                        drag.maximumX: parent.parent.width - parent.width
                        
                        onPositionChanged: {
                            if (drag.active) {
                                var ratio = parent.x / (parent.parent.width - parent.width)
                                var volume = ratio * root.maxVolumeRange // Dynamic range based on asset
                                unifiedGridRenderer.minVolumeFilter = volume
                            }
                        }
                    }
                }
            }
            
            Row {
                spacing: 15
                Text { text: "0"; color: "#00FF00"; font.pixelSize: 9 }
                Text { text: (root.maxVolumeRange / 2).toFixed(0); color: "#00FF00"; font.pixelSize: 9 }
                Text { text: root.maxVolumeRange.toFixed(0); color: "#00FF00"; font.pixelSize: 9 }
            }
        }
        
        // Grid Resolution Controls
        Column {
            spacing: 5
            z: 10
            Text { text: "Grid Resolution"; color: "white"; font.pixelSize: 12 }
            Row {
                spacing: 5
                Rectangle {
                    width: 50; height: 25; color: Qt.rgba(0, 0, 0.4, 0.8); border.color: "white"; border.width: 1; radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 10; text: "Fine" }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setGridMode(0) }
                }
                Rectangle {
                    width: 50; height: 25; color: Qt.rgba(0, 0, 0.4, 0.8); border.color: "white"; border.width: 1; radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 10; text: "Med" }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setGridMode(1) }
                }
                Rectangle {
                    width: 50; height: 25; color: Qt.rgba(0, 0, 0.4, 0.8); border.color: "white"; border.width: 1; radius: 3
                    Text { anchors.centerIn: parent; color: "white"; font.pixelSize: 10; text: "Coarse" }
                    MouseArea { anchors.fill: parent; onClicked: unifiedGridRenderer.setGridMode(2) }
                }
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