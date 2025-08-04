import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10
    
    // Standard interface for all controls
    property var target: null  // UnifiedGridRenderer instance
    property bool enabled: true
    property int currentTimeframe: target ? target.timeframeMs : 100
    
    // Signals
    signal valueChanged(var newValue)
    
    // Update current timeframe when target changes
    Connections {
        target: root.target
        function onTimeframeChanged() {
            root.currentTimeframe = root.target.timeframeMs
        }
    }
    
    Text { 
        text: "Timeframe (ms)"
        color: "white"
        font.pixelSize: 12 
    }
    
    Text { 
        text: "Select aggregation timeframe:"
        color: "#CCCCCC"
        font.pixelSize: 10
        visible: true 
    }
    
    Row {
        spacing: 3
        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 100 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: root.currentTimeframe === 100 ? "white" : "white"
            border.width: root.currentTimeframe === 100 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 9
                text: "100"
                font.bold: root.currentTimeframe === 100 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setTimeframe(100)
                        root.valueChanged(100)
                    }
                }
            }
        }
        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 250 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: root.currentTimeframe === 250 ? "white" : "white"
            border.width: root.currentTimeframe === 250 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 9
                text: "250"
                font.bold: root.currentTimeframe === 250 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setTimeframe(250)
                        root.valueChanged(250)
                    }
                }
            }
        }
        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 500 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: root.currentTimeframe === 500 ? "white" : "white"
            border.width: root.currentTimeframe === 500 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 9
                text: "500"
                font.bold: root.currentTimeframe === 500 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setTimeframe(500)
                        root.valueChanged(500)
                    }
                }
            }
        }
    }
    
    Row {
        spacing: 3
        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 1000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: root.currentTimeframe === 1000 ? "white" : "white"
            border.width: root.currentTimeframe === 1000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 9
                text: "1s"
                font.bold: root.currentTimeframe === 1000 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setTimeframe(1000)
                        root.valueChanged(1000)
                    }
                }
            }
        }
        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 2000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: root.currentTimeframe === 2000 ? "white" : "white"
            border.width: root.currentTimeframe === 2000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 9
                text: "2s"
                font.bold: root.currentTimeframe === 2000 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setTimeframe(2000)
                        root.valueChanged(2000)
                    }
                }
            }
        }
        Rectangle {
            width: 40; height: 25
            color: root.currentTimeframe === 5000 ? Qt.rgba(0, 0.8, 0, 0.9) : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: root.currentTimeframe === 5000 ? "white" : "white"
            border.width: root.currentTimeframe === 5000 ? 2 : 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 9
                text: "5s"
                font.bold: root.currentTimeframe === 5000 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setTimeframe(5000)
                        root.valueChanged(5000)
                    }
                }
            }
        }
    }
}