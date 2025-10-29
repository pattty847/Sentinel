import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10
    
    // Standard interface for all controls
    property var target: null  // UnifiedGridRenderer instance
    property bool enabled: true
    
    // Asset-aware volume scaling from parent context
    property real maxVolumeRange: 1000.0  // Default, should be set by parent
    
    // Signals
    signal valueChanged(var newValue)
    
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
            text: " VOLUME FILTER"
        }
    }
    
    Text { 
        text: "Min Volume: " + (root.target ? root.target.minVolumeFilter.toFixed(2) : "0.00")
        color: "#00FF00"
        font.pixelSize: 11
        font.bold: true 
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
                root.target ? (root.target.minVolumeFilter / root.maxVolumeRange) * (parent.width - width) : 0))
            
            MouseArea {
                anchors.fill: parent
                drag.target: parent
                drag.axis: Drag.XAxis
                drag.minimumX: 0
                drag.maximumX: parent.parent.width - parent.width
                enabled: root.enabled && root.target
                
                onPositionChanged: {
                    if (drag.active && root.target) {
                        var ratio = parent.x / (parent.parent.width - parent.width)
                        var volume = ratio * root.maxVolumeRange
                        root.target.minVolumeFilter = volume
                        root.valueChanged(volume)
                    }
                }
            }
        }
    }
    
    Row {
        spacing: 15
        Text { 
            text: "0"
            color: "#00FF00"
            font.pixelSize: 9 
        }
        Text { 
            text: (root.maxVolumeRange / 2).toFixed(0)
            color: "#00FF00"
            font.pixelSize: 9 
        }
        Text { 
            text: root.maxVolumeRange.toFixed(0)
            color: "#00FF00"
            font.pixelSize: 9 
        }
    }
}