// libs/gui/qml/ZeroLagDepthChart.qml
import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: root
    color: "#000000"
    
    property alias viewport: architecture.viewport
    property alias testMode: architecture.testMode
    
    // Zero-lag architecture instance
    ZeroLagArchitecture {
        id: architecture
        anchors.fill: parent
        
        Component.onCompleted: {
            console.log("🚀 ZERO-LAG CHART INITIALIZED")
            if (testMode) {
                startTestMode()
            }
        }
    }
    
    // Mouse interaction - IMMEDIATE response
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        
        property point lastPos
        property bool isDragging: false
        
        onPressed: function(mouse) {
            isDragging = true
            lastPos = Qt.point(mouse.x, mouse.y)
            architecture.handleMousePress(lastPos)
        }
        
        onPositionChanged: function(mouse) {
            if (isDragging) {
                var currentPos = Qt.point(mouse.x, mouse.y)
                var delta = Qt.point(currentPos.x - lastPos.x, currentPos.y - lastPos.y)
                
                // IMMEDIATE pan - no throttling!
                architecture.handleMouseMove(delta)
                lastPos = currentPos
            }
        }
        
        onReleased: {
            isDragging = false
        }
        
        onWheel: function(wheel) {
            var center = Qt.point(wheel.x, wheel.y)
            // IMMEDIATE zoom
            architecture.handleWheel(wheel.angleDelta.y, center)
        }
    }
    
    // Performance monitor overlay
    Rectangle {
        id: perfOverlay
        anchors.top: parent.top
        anchors.right: parent.right
        width: 300
        height: 100
        color: "#80000000"
        border.color: "#00ff00"
        border.width: 1
        visible: showPerformanceOverlay
        
        property bool showPerformanceOverlay: false
        
        Column {
            anchors.margins: 10
            anchors.fill: parent
            spacing: 5
            
            Text {
                text: "🚀 ZERO-LAG PERFORMANCE"
                color: "#00ff00"
                font.bold: true
            }
            
            Text {
                id: frameRateText
                text: "FPS: " + architecture.currentFPS.toFixed(1)
                color: "#ffffff"
            }
            
            Text {
                id: timeFrameText  
                text: "TimeFrame: " + architecture.currentTimeFrameName
                color: "#ffffff"
            }
            
            Text {
                id: bucketText
                text: "Buckets: " + architecture.visibleBuckets + " (" + architecture.bucketPixels.toFixed(1) + "px each)"
                color: "#ffffff"
            }
        }
    }
    
    // Debug controls
    Row {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.margins: 10
        spacing: 10
        
        Button {
            text: "📊 Test 100ms"
            onClicked: architecture.forceTimeFrame("100ms")
        }
        
        Button {
            text: "📊 Test 1s"  
            onClicked: architecture.forceTimeFrame("1s")
        }
        
        Button {
            text: "📊 Test 1m"
            onClicked: architecture.forceTimeFrame("1m")
        }
        
        Button {
            text: "📊 Auto LOD"
            onClicked: architecture.enableAutoLOD()
        }
        
        Button {
            text: perfOverlay.showPerformanceOverlay ? "Hide Perf" : "Show Perf"
            onClicked: perfOverlay.showPerformanceOverlay = !perfOverlay.showPerformanceOverlay
        }
    }
    
    // Zoom level indicator
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: 200
        height: 30
        color: "#80000000"
        border.color: "#ffffff"
        border.width: 1
        radius: 5
        
        Text {
            anchors.centerIn: parent
            text: "Zoom: " + architecture.zoomLevel.toFixed(2) + "x | " + architecture.currentTimeFrameName
            color: "#ffffff"
            font.bold: true
        }
    }
    
    // Grid overlay for bucket visualization
    Canvas {
        id: gridCanvas
        anchors.fill: parent
        
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            
            if (!architecture.showGrid) return
            
            // Draw time grid based on current bucket size
            ctx.strokeStyle = "#333333"
            ctx.lineWidth = 1
            
            var bucketPixels = architecture.bucketPixels
            if (bucketPixels > 5) { // Only draw if buckets are large enough
                for (var x = 0; x < width; x += bucketPixels) {
                    ctx.beginPath()
                    ctx.moveTo(x, 0)
                    ctx.lineTo(x, height)
                    ctx.stroke()
                }
            }
            
            // Draw price grid
            var priceStep = architecture.priceGridStep
            if (priceStep > 0) {
                var pixelsPerPrice = height / architecture.priceRange
                for (var y = 0; y < height; y += priceStep * pixelsPerPrice) {
                    ctx.beginPath()
                    ctx.moveTo(0, y)
                    ctx.lineTo(width, y)
                    ctx.stroke()
                }
            }
        }
        
        Connections {
            target: architecture
            function onGridChanged() {
                gridCanvas.requestPaint()
            }
        }
    }
}