import QtQuick 2.15
import QtQuick.Controls
import ".."
import "./"

Item {
    id: root
    property var target: null
    property int currentTimeframe: 0
    property real maxVolumeRange: 1000.0

    width: 240
    height: parent ? parent.height : 600

    Theme { id: theme }

    Rectangle {
        id: rail
        width: 44
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        color: theme.panel
        border.color: theme.border

        Column {
            anchors.top: parent.top
            anchors.topMargin: 12
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            Repeater {
                model: [
                    { icon: "qrc:/icons/icon-draw.svg", tip: "Drawing tools" },
                    { icon: "qrc:/icons/icon-timeframe.svg", tip: "Timeframe" },
                    { icon: "qrc:/icons/icon-indicators.svg", tip: "Indicators" },
                    { icon: "qrc:/icons/icon-watchlist.svg", tip: "Watchlist" }
                ]

                Rectangle {
                    width: 30
                    height: 30
                    radius: 6
                    color: "transparent"
                    border.color: "transparent"

                    Image {
                        anchors.centerIn: parent
                        source: modelData.icon
                        width: 18
                        height: 18
                        fillMode: Image.PreserveAspectFit
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: parent.color = theme.panelAlt
                        onExited: parent.color = "transparent"
                    }
                }
            }
        }
    }

    Rectangle {
        id: panel
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: rail.right
        anchors.right: parent.right
        color: theme.panelAlt
        border.color: theme.border

        Flickable {
            anchors.fill: parent
            contentWidth: panel.width
            contentHeight: controlsColumn.implicitHeight + 16
            clip: true

            Column {
                id: controlsColumn
                width: panel.width
                spacing: 8
                anchors.top: parent.top
                anchors.topMargin: 10
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10

                Text {
                    text: "Controls"
                    color: theme.text
                    font.pixelSize: 12
                    font.bold: true
                }

                NavigationControls { target: root.target }

                Text {
                    text: "Active: " + root.currentTimeframe + "ms"
                    color: theme.accent
                    font.pixelSize: 11
                    font.bold: true
                }

                TimeframeSelector { target: root.target }
                PriceResolutionSelector { target: root.target }
                VolumeFilter { target: root.target; maxVolumeRange: root.maxVolumeRange }
                LiquidityThresholdFilter { target: root.target }
                GridResolutionSelector { target: root.target }
            }
        }
    }
}
