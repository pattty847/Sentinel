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
    
    Text { 
        text: "Grid Resolution"
        color: "white"
        font.pixelSize: 12 
    }
    
    Row {
        spacing: 5
        Rectangle {
            width: 50; height: 25
            color: Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 10
                text: "Fine" 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setGridMode(0)
                        root.valueChanged(0)
                    }
                }
            }
        }
        Rectangle {
            width: 50; height: 25
            color: Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 10
                text: "Med" 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setGridMode(1)
                        root.valueChanged(1)
                    }
                }
            }
        }
        Rectangle {
            width: 50; height: 25
            color: Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 10
                text: "Coarse" 
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setGridMode(2)
                        root.valueChanged(2)
                    }
                }
            }
        }
    }
}