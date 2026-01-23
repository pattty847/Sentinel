import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: root
    color: "#0c0f12"

    property real labelScale: 1.0
    property string sampleText: "12.948"

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

            Grid {
                id: grid
                anchors.centerIn: parent
                columns: 6
                spacing: 6

                Repeater {
                    model: 36
                    Rectangle {
                        width: 88
                        height: 52
                        radius: 4
                        color: index % 2 === 0 ? "#133842" : "#0f2f37"
                        border.color: "#1b3c44"
                        border.width: 1

                        Text {
                            anchors.centerIn: parent
                            text: {
                                var base = ((index * 37) % 120) / 10.0
                                return base.toFixed(3)
                            }
                            color: index % 5 === 0 ? "#e7f1ff" : "#95a7b6"
                            font.pixelSize: 12 * root.labelScale
                            font.family: "Monospace"
                        }
                    }
                }
            }
        }
    }
}
