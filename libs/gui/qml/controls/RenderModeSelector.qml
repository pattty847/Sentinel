import QtQuick 2.15
import QtQuick.Controls 2.15
import Sentinel.Charts 1.0

Column {
    id: root
    
    property var target: null
    spacing: 2
    
    Text {
        text: "Render Mode"
        color: "white"
        font.pixelSize: 10
        font.bold: true
    }
    
    ComboBox {
        id: renderModeCombo
        width: 150
        
        model: [
            { text: "Liquidity Heatmap", value: UnifiedGridRenderer.LiquidityHeatmap },
            { text: "Trade Flow", value: UnifiedGridRenderer.TradeFlow },
            { text: "Trade Bubbles", value: UnifiedGridRenderer.TradeBubbles },
            { text: "Volume Candles", value: UnifiedGridRenderer.VolumeCandles },
            { text: "Order Book Depth", value: UnifiedGridRenderer.OrderBookDepth }
        ]
        
        textRole: "text"
        
        currentIndex: {
            if (!target) return 0
            
            // Find index based on current render mode
            for (var i = 0; i < model.length; i++) {
                if (model[i].value === target.renderMode) {
                    return i
                }
            }
            return 0
        }
        
        onActivated: function(index) {
            if (target && index >= 0 && index < model.length) {
                target.renderMode = model[index].value
                console.log("🎨 Render mode changed to:", model[index].text)
            }
        }
        
        delegate: ItemDelegate {
            width: renderModeCombo.width
            contentItem: Text {
                text: modelData.text
                color: "white"
                font.pixelSize: 10
                verticalAlignment: Text.AlignVCenter
            }
            highlighted: renderModeCombo.highlightedIndex === index
        }
        
        background: Rectangle {
            color: "#2D2D2D"
            border.color: "#555"
            border.width: 1
            radius: 3
        }
        
        contentItem: Text {
            text: renderModeCombo.displayText
            color: "white"
            font.pixelSize: 10
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
        }
        
        popup: Popup {
            y: renderModeCombo.height
            width: renderModeCombo.width
            implicitHeight: contentItem.implicitHeight
            padding: 1
            
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: renderModeCombo.popup.visible ? renderModeCombo.delegateModel : null
                currentIndex: renderModeCombo.highlightedIndex
            }
            
            background: Rectangle {
                color: "#2D2D2D"
                border.color: "#555"
                border.width: 1
                radius: 3
            }
        }
    }
}