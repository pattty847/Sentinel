import QtQuick 2.15
import QtQuick.Controls

ToolTip {
    id: tooltip

    property Item pointerItem
    readonly property point pointer: pointerItem && parent
        ? pointerItem.mapToItem(parent, pointerItem.mouseX, pointerItem.mouseY)
        : Qt.point(0, 0)
    readonly property real anchorX: pointer.x + 12 + width <= parent.width
        ? pointer.x + 12 : Math.max(0, pointer.x - width - 12)
    readonly property real anchorY: pointer.y + 16 + height <= parent.height
        ? pointer.y + 16 : Math.max(0, pointer.y - height - 12)

    delay: 550
    padding: 0
    leftPadding: 6
    rightPadding: 6
    topPadding: 4
    bottomPadding: 4
    width: Math.min(360, contentItem.implicitWidth + leftPadding + rightPadding)
    x: anchorX
    y: anchorY

    contentItem: Label {
        text: tooltip.text
        color: "#B6C2CF"
        font.family: "Roboto Mono"
        font.pixelSize: 11
        wrapMode: Text.Wrap
    }
    background: Rectangle {
        color: "#20252B"
        border.color: "#2B3138"
        border.width: 1
        radius: 3
    }
}
