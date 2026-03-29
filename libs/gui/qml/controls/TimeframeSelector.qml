import QtQuick 2.15

Column {
    id: root
    spacing: 5
    z: 10

    property var target: null
    property bool enabled: true
    property int currentTimeframe: target ? target.timeframeMs : 1000

    signal valueChanged(var newValue)

    property var timeframes: [
        { text: "1s",  value: 1000      },
        { text: "1m",  value: 60000     },
        { text: "5m",  value: 300000    },
        { text: "15m", value: 900000    },
        { text: "1h",  value: 3600000   },
        { text: "4h",  value: 14400000  },
        { text: "1D",  value: 86400000  }
    ]

    Connections {
        target: root.target
        function onTimeframeChanged() {
            root.currentTimeframe = root.target.timeframeMs
        }
    }

    Text {
        text: "Timeframe"
        color: "white"
        font.pixelSize: 12
    }

    // Button template instantiated by each Repeater below.
    // modelData is the JS object { text, value } from the timeframes array.
    Component {
        id: timeframeButton
        Rectangle {
            width: 40; height: 25
            radius: 3
            color: root.currentTimeframe === modelData.value
                   ? Qt.rgba(0, 0.8, 0, 0.9)
                   : Qt.rgba(0, 0, 0.4, 0.8)
            border.color: "white"
            border.width: root.currentTimeframe === modelData.value ? 2 : 1
            enabled: root.enabled && root.target

            Text {
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 9
                font.bold: root.currentTimeframe === modelData.value
                text: modelData.text
            }

            MouseArea {
                anchors.fill: parent
                enabled: root.enabled && root.target
                onClicked: {
                    if (root.target) {
                        root.target.setTimeframe(modelData.value)
                        root.valueChanged(modelData.value)
                    }
                }
            }
        }
    }

    // Row 1: 1s  1m  5m  15m
    Row {
        spacing: 3
        Repeater { model: root.timeframes.slice(0, 4); delegate: timeframeButton }
    }

    // Row 2: 1h  4h  1D
    Row {
        spacing: 3
        Repeater { model: root.timeframes.slice(4, 7); delegate: timeframeButton }
    }
}
