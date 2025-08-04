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
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 10
                text: "+"
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.zoomIn()
                        root.valueChanged("zoomIn")
                    }
                }
            }
        }
        Rectangle {
            width: 35
            height: 25
            color: Qt.rgba(0.4, 0, 0, 0.8)
            border.color: "red"
            border.width: 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 10
                text: "-"
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.zoomOut()
                        root.valueChanged("zoomOut")
                    }
                }
            }
        }
        Rectangle {
            width: 35
            height: 25
            color: Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "cyan"
            border.width: 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 10
                text: "RST"
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.resetZoom()
                        root.valueChanged("resetZoom")
                    }
                }
            }
        }
        Rectangle {
            width: 35
            height: 25
            color: (root.target && root.target.autoScrollEnabled) ? Qt.rgba(0, 0.4, 0, 0.8) : Qt.rgba(0.4, 0, 0, 0.8)
            border.color: (root.target && root.target.autoScrollEnabled) ? "lime" : "red"
            border.width: 1
            radius: 3
            enabled: root.enabled && root.target
            Text { 
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 10
                text: "AUTO"
            }
            MouseArea { 
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.enableAutoScroll(!root.target.autoScrollEnabled)
                        root.valueChanged("autoScroll")
                    }
                }
            }
        }
    }
}