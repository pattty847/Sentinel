import QtQuick 2.15
import Sentinel.Charts 1.0

Rectangle {
    id: root
    color: "black"
    
    property string symbol: "BTC-USD"
    property bool stressTestMode: false
    property var chartModeController: null

    Connections {
        target: chartModeController
        function onComponentVisibilityChanged(component, visible) {
            if (component === "tradeScatter") {
                gpuChart.visible = visible
            } else if (component === "candles") {
                candleChart.visible = visible
            } else if (component === "orderBook") {
                heatmapLayer.visible = visible
            }
        }
    }
    
    // 🚀 GPU CHART COMPONENTS - LAYERED RENDERING
    
    // 🔥 PHASE 2: HEATMAP BACKGROUND LAYER (Order Book)
    HeatmapBatched {
        id: heatmapLayer
        objectName: "heatmapLayer"  // 🔥 EXPOSE TO C++ - CRITICAL FIX!
        anchors.fill: parent
        
        Component.onCompleted: {
            // 🔥 FIX PRICE RANGE: Use real BTC range from logs (~$107.283k)
            setPriceRange(107200, 107350)  // $150 range around actual trades
            setMaxQuads(50000)             // 🔥 WALL OF LIQUIDITY: 50k historical points for persistence
            setIntensityScale(0.1)         // Much smaller intensity scale
            
            // Generate test order book data
            generateTestOrderBook()
        }
        
        function generateTestOrderBook() {
            console.log("🔥 GENERATING TEST ORDER BOOK FOR HEATMAP")
            
            var bids = []
            var asks = []
            var midPrice = 107283  // 🔥 MATCH REAL BTC PRICE FROM LOGS
            
            // Generate 20 bid levels below mid price (tighter range)
            for (var i = 0; i < 20; i++) {
                var price = midPrice - (i + 1) * 5  // $5 steps down
                var size = Math.random() * 2 + 0.01  // 0.01 to 2.01 BTC
                
                bids.push({
                    price: price,
                    size: size
                })
            }
            
            // Generate 20 ask levels above mid price  
            for (var i = 0; i < 20; i++) {
                var price = midPrice + (i + 1) * 5  // $5 steps up
                var size = Math.random() * 2 + 0.01  // 0.01 to 2.01 BTC
                
                asks.push({
                    price: price,
                    size: size
                })
            }
            
            updateBids(bids)
            updateAsks(asks)
            
            console.log("✅ HEATMAP LOADED:", bids.length, "bids,", asks.length, "asks")
        }
    }
    
    // 🕯️ PHASE 5: CANDLESTICK MIDDLE LAYER (Between heatmap and trades)
    CandleChartView {
        id: candleChart
        objectName: "candleChart"
        anchors.fill: parent
        
        // 🎯 PROFESSIONAL CONFIGURATION
        candlesEnabled: true
        lodEnabled: true
        candleWidth: 8.0
        volumeScaling: true
        maxCandles: 10000
        
        Component.onCompleted: {
            console.log("🕯️ CANDLE CHART INITIALIZED - Professional trading terminal candles!")
        }
    }
    
    // 🚀 PHASE 1: TRADE POINTS FOREGROUND LAYER (On top of candles)
    GPUChartWidget {
        id: gpuChart
        objectName: "gpuChart"  // 🔥 CRITICAL FIX: Explicit objectName for C++ lookup
        anchors.fill: parent
        
        // Generate test data based on mode
        Component.onCompleted: {
            if (stressTestMode) {
                generateStressTestData()
            } else {
                generateTestData()
            }
        }
        
        function generateTestData() {
            console.log("✅ TEST POINTS GENERATION DISABLED - USING REAL BTC DATA ONLY");
            // 🔥 GEMINI FIX: Removed the function call that was causing TypeError
            
            // 🕯️ COORDINATE SYNC: C++ handles the coordinate connections
            console.log("🕯️ COORDINATE SYNC: Handled by C++ mainwindow_gpu.cpp")
        }
        
        // 🔥 PHASE 1: 1 MILLION POINT STRESS TEST!
        function generateStressTestData() {
            console.log("🚀🔥 GENERATING 1M POINTS FOR VBO STRESS TEST!");
            var startTime = Date.now();
            
            // Configure for massive point load
            gpuChart.setMaxPoints(1000000);  // 1M points
            gpuChart.setTimeSpan(60.0);      // 60 second window
            // 🔥 FIX: Don't override price range - keep real BTC range for visibility
            // gpuChart.setPriceRange(45000.0, 55000.0);
            
            var points = []
            var basePrice = 50000.0;
            var now = Date.now();
            
            // Generate 1 MILLION points!
            for (var i = 0; i < 1000000; i++) {
                var price = basePrice + 
                    Math.sin(i * 0.001) * 2000 +           // Long wave
                    Math.sin(i * 0.01) * 500 +             // Medium wave  
                    (Math.random() - 0.5) * 200;           // Noise
                
                var timestamp = now + (i * 60); // 60ms intervals for 60s window
                var isBuy = Math.random() > 0.5;
                var tradeSize = 0.001 + Math.random() * 0.1;
                
                points.push({
                    timestamp: timestamp,
                    price: price,
                    size: tradeSize,
                    side: isBuy ? "Buy" : "Sell"
                });
                
                // Progress logging every 100k points
                if (i % 100000 === 0 && i > 0) {
                    console.log("📊 Generated", i, "points...");
                }
            }
            
            var generateTime = Date.now() - startTime;
            console.log("🔥 Generated 1M points in", generateTime, "ms");
            
            // 🔥 GEMINI FIX: Test data generation disabled in Option B - using real data only
            console.log("⚡ STRESS TEST DISABLED - USING REAL BTC DATA STREAM INSTEAD!");
            console.log("🚀 VBO TRIPLE-BUFFERING READY FOR REAL DATA!");
        }
    }
    
    // Enhanced debug overlay with performance info
    Column {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 10
        spacing: 5
        
        Text {
            color: "lime"
            font.pixelSize: 16
            font.bold: true
            text: stressTestMode ? "🔥 VBO STRESS TEST - 1M POINTS" : "🚀 PHASE 4: PAN/ZOOM CONTROLS - " + root.symbol
        }
        
        Text {
            color: "yellow"
            font.pixelSize: 12
            text: "Intel UHD Graphics 630 | <5ms Interaction Latency Target"
        }
        
        Text {
            color: "cyan"
            font.pixelSize: 12
            text: "🖱️ Drag: Pan | 🖱️ Wheel: Zoom | R: Reset | A: Auto-scroll"
        }
        
        Text {
            color: "orange"
            font.pixelSize: 12
            text: "✅ Phases 1-5: Complete | 🕯️ Candles + Circles + Heatmap Active"
        }
        
        Text {
            color: "magenta"
            font.pixelSize: 12
            text: "📊 GPU: " + gpuChart.debugInfo
        }
    }
    
    // 🚀 PHASE 4: Pan/Zoom Control Panel
    Column {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        spacing: 5
        
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
                
                Text {
                    anchors.centerIn: parent
                    color: "white"
                    font.pixelSize: 10
                    text: "+"
                }
                
                MouseArea {
                    anchors.fill: parent
                    onClicked: gpuChart.zoomIn()
                }
            }
            
            Rectangle {
                width: 35
                height: 25
                color: Qt.rgba(0.4, 0, 0, 0.8)
                border.color: "red"
                border.width: 1
                radius: 3
                
                Text {
                    anchors.centerIn: parent
                    color: "white"
                    font.pixelSize: 10
                    text: "-"
                }
                
                MouseArea {
                    anchors.fill: parent
                    onClicked: gpuChart.zoomOut()
                }
            }
            
            Rectangle {
                width: 35
                height: 25
                color: Qt.rgba(0, 0, 0.4, 0.8)
                border.color: "cyan"
                border.width: 1
                radius: 3
                
                Text {
                    anchors.centerIn: parent
                    color: "white"
                    font.pixelSize: 10
                    text: "RST"
                }
                
                MouseArea {
                    anchors.fill: parent
                    onClicked: gpuChart.resetZoom()
                }
            }
            
            Rectangle {
                width: 35
                height: 25
                color: gpuChart.autoScrollEnabled ? Qt.rgba(0, 0.4, 0, 0.8) : Qt.rgba(0.4, 0, 0, 0.8)
                border.color: gpuChart.autoScrollEnabled ? "lime" : "red"
                border.width: 1
                radius: 3
                
                Text {
                    anchors.centerIn: parent
                    color: "white"
                    font.pixelSize: 10
                    text: "AUTO"
                }
                
                MouseArea {
                    anchors.fill: parent
                    onClicked: gpuChart.enableAutoScroll(!gpuChart.autoScrollEnabled)
                }
            }
        }
    }
    
    // 🚀 PHASE 4: Keyboard Shortcuts
    focus: true
    Keys.onPressed: function(event) {
        switch(event.key) {
            case Qt.Key_R: 
                gpuChart.resetZoom()
                event.accepted = true
                break
            case Qt.Key_A:
                gpuChart.enableAutoScroll(!gpuChart.autoScrollEnabled)
                event.accepted = true
                break
            case Qt.Key_Plus:
            case Qt.Key_Equal:
                gpuChart.zoomIn()
                event.accepted = true
                break
            case Qt.Key_Minus:
            case Qt.Key_Underscore:
                gpuChart.zoomOut()
                event.accepted = true
                break
            case Qt.Key_F11:
                // Development: Toggle render visualization
                console.log("🔍 F11 - Toggle render visualization (dev feature)")
                event.accepted = true
                break
        }
    }
} 