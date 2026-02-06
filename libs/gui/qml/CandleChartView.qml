import QtQuick 2.15
import Sentinel.Charts 1.0

Rectangle {
    id: root
    color: "transparent"
    
    property alias candlesEnabled: candleChart.visible
    property alias lodEnabled: candleChart.lodEnabled
    property alias candleWidth: candleChart.candleWidth
    property alias volumeScaling: candleChart.volumeScaling
    property alias maxCandles: candleChart.maxCandles
    
    signal candleCountChanged(int count)
    signal lodLevelChanged(int timeframe)
    signal renderTimeChanged(double ms)
    
    CandlestickBatched {
        id: candleChart
        objectName: "candlestickRenderer"
        anchors.fill: parent
        
        //  DEFAULT CONFIGURATION: Professional appearance
        lodEnabled: true
        candleWidth: 8.0
        volumeScaling: true
        maxCandles: 10000
        
        Component.onCompleted: {
            setBullishColor(Qt.rgba(0.0, 0.8, 0.2, 0.7));
            setBearishColor(Qt.rgba(0.8, 0.2, 0.0, 0.7));
            setWickColor(Qt.rgba(1.0, 1.0, 1.0, 0.8));
        }
        
        //  FORWARD SIGNALS: Pass performance metrics to parent
        onCandleCountChanged: function(count) { root.candleCountChanged(count); }
        onLodLevelChanged: function(timeframe) { root.lodLevelChanged(timeframe); }
        onRenderTimeChanged: function(ms) { root.renderTimeChanged(ms); }
    }
    
    function clearCandles() {
        candleChart.clearCandles();
    }
    
    function setTimeWindow(startTime, endTime) {
        candleChart.setTimeWindow(startTime, endTime);
    }
    
    function connectToViewSignals(chartWidget) {
        // Connect to GPUChartWidget's viewChanged signal for coordinate sync
        if (chartWidget && chartWidget.viewChanged) {
            chartWidget.viewChanged.connect(candleChart.onViewChanged);
            console.log("🕯 CONNECTED CANDLE CHART to coordinate system");
        }
    }
    
    
    function setAutoLOD(enabled) {
        candleChart.setAutoLOD(enabled);
    }
    
    function forceTimeFrame(timeframe) {
        candleChart.forceTimeFrame(timeframe);
    }
    
    function getCurrentPixelsPerCandle() {
        return candleChart.calculateCurrentPixelsPerCandle();
    }
    
    function setBullishColor(color) {
        candleChart.setBullishColor(color);
    }
    
    function setBearishColor(color) {
        candleChart.setBearishColor(color);
    }
    
    function setWickColor(color) {
        candleChart.setWickColor(color);
    }
} 
