import QtQuick 2.15
import QtQuick.Controls 2.15
import Sentinel.Charts 1.0

Rectangle {
    id: root
    color: "#0c0f12"

    property real labelScale: 1.0
    property string sampleText: "12.948"
    property string fontFamily: "Roboto Mono"
    property int fontPx: 96
    property real msdfRange: 8.0

    Column {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Rectangle {
            height: 42
            width: parent.width
            color: "#151a1f"
            radius: 6

            Row {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 12

                Text {
                    text: "Lab"
                    color: "#d6dbe0"
                    font.pixelSize: 14
                    font.bold: true
                }

                Text {
                    text: "Text Scale"
                    color: "#98a2ad"
                    font.pixelSize: 12
                }

                Slider {
                    id: scaleSlider
                    from: 0.6
                    to: 1.8
                    value: 1.0
                    width: 180
                    onValueChanged: root.labelScale = value
                }

                Text {
                    text: Number(root.labelScale).toFixed(2) + "x"
                    color: "#98a2ad"
                    font.pixelSize: 12
                }

                Text {
                    text: "Font Px"
                    color: "#98a2ad"
                    font.pixelSize: 12
                }

                Slider {
                    id: fontSlider
                    from: 64
                    to: 128
                    stepSize: 1
                    value: 96
                    width: 160
                    onValueChanged: root.fontPx = Math.round(value)
                }

                Text {
                    text: root.fontPx + "px"
                    color: "#98a2ad"
                    font.pixelSize: 12
                }
            }
        }

        Rectangle {
            id: testArea
            width: parent.width
            height: parent.height - 66
            color: "#0f1419"
            radius: 8
            border.color: "#1d262e"
            border.width: 1

            Row {
                anchors.centerIn: parent
                spacing: 24

                Column {
                    spacing: 10
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        text: "MSDF"
                        color: "#98a2ad"
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        width: 220
                    }

                    Rectangle {
                        width: 220
                        height: 120
                        radius: 6
                        color: "#133842"
                        border.color: "#1b3c44"
                        border.width: 1

                        LabTextItem {
                            anchors.fill: parent
                            text: root.sampleText
                            scale: root.labelScale
                            color: "#e7f1ff"
                            fontFamily: root.fontFamily
                            fontPixelSize: root.fontPx
                            pixelRange: root.msdfRange
                        }
                    }
                }

                Column {
                    spacing: 10
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        text: "QML"
                        color: "#98a2ad"
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        width: 220
                    }

                    Rectangle {
                        width: 220
                        height: 120
                        radius: 6
                        color: "#133842"
                        border.color: "#1b3c44"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: root.sampleText
                            color: "#e7f1ff"
                            font.pixelSize: 12 * root.labelScale
                            font.family: root.fontFamily
                        }
                    }
                }
            }
        }
    }
}
