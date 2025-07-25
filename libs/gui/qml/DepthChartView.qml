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
    
    UnifiedGridRenderer {
        id: unifiedGridRenderer
        objectName: "unifiedGridRenderer"
        anchors.fill: parent
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
        }
    }
} 