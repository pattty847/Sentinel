import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10
    
    // Standard interface for all controls
    property var target: null  // UnifiedGridRenderer instance
    property bool enabled: true
    
    // Signals
    signal valueChanged(var newValue)
    
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
        text: "Price Bucket: $" + (root.target ? root.target.getCurrentPriceResolution().toFixed(2) : "0.00")
        color: "#FFA500"
        font.pixelSize: 11
        font.bold: true 
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
                root.target ? (Math.log(root.target.getCurrentPriceResolution() / 0.01) / Math.log(100)) * (parent.width - width) : 0))
            
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
                        var resolution = 0.01 * Math.pow(100, ratio) // Range from $0.01 to $1.00
                        root.target.setPriceResolution(resolution)
                        root.valueChanged(resolution)
                    }
                }
            }
        }
    }
    
    Row {
        spacing: 3
        Text { 
            text: "$0.01"
            color: "#FFA500"
            font.pixelSize: 9 
        }
        Text { 
            text: "$0.10"
            color: "#FFA500"
            font.pixelSize: 9 
        }
        Text { 
            text: "$1.00"
            color: "#FFA500"
            font.pixelSize: 9 
        }
    }
}