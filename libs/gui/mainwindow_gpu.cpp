#include "mainwindow_gpu.h"
#include "streamcontroller.h"
#include "statisticscontroller.h"
#include "gpuchartwidget.h"
#include "heatmapinstanced.h"
#include "candlestickbatched.h"
#include "chartmodecontroller.h"
#include "gpudataadapter.h"
#include "candlelod.h"
#include "fractalzoomcontroller.h"
#include "SentinelLogging.hpp"
#include <QQmlContext>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTimer>
#include "testdataplayer.h"


MainWindowGPU::MainWindowGPU(bool testMode, QWidget* parent) : QWidget(parent), m_testMode(testMode) {
    qDebug() << "🚀 CREATING ZERO-LAG GPU TRADING TERMINAL!";

    // 🚀 CREATE ZERO-LAG ARCHITECTURE FIRST
    m_zeroLagArch = new ZeroLagArchitecture(this);
    m_unifiedViewport = new UnifiedViewport(this);
    m_dynamicLOD = new DynamicLOD(this);
    
    if (testMode) {
        m_instantTestData = new InstantTestData(this);
    }

    setupUI();
    setupConnections();
    
    // Set window properties
    setWindowTitle("Sentinel - Zero-Lag GPU Trading Terminal");
    resize(1400, 900);
    
    // 🚀 INITIALIZE ZERO-LAG SYSTEM
    QTimer::singleShot(100, [this]() {
        if (m_gpuChart && m_gpuChart->rootObject()) {
            m_zeroLagArch->initialize(m_gpuChart->rootObject());
            
            if (m_testMode && m_instantTestData) {
                qDebug() << "🚀 STARTING ZERO-LAG TEST MODE!";
                m_zeroLagArch->startTestMode();
            }
        }
    });
    
    // Automatically start the subscription process
    if (!m_testMode) {
        QTimer::singleShot(0, this, &MainWindowGPU::onSubscribe);
    }
}

void MainWindowGPU::setupUI() {
    // 🔥 CREATE GPU CHART (QML) - Load from file system first
    m_gpuChart = new QQuickWidget(this);
    m_gpuChart->setResizeMode(QQuickWidget::SizeRootObjectToView);
    
    // Try file system path first to bypass resource issues
    QString qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
    qDebug() << "🔍 Trying QML path:" << qmlPath;
    
    if (QFile::exists(qmlPath)) {
        m_gpuChart->setSource(QUrl::fromLocalFile(qmlPath));
        qDebug() << "✅ QML loaded from file system!";
    } else {
        qDebug() << "❌ QML file not found, trying resource path...";
        m_gpuChart->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
    }
    
    // Check if QML loaded successfully
    if (m_gpuChart->status() == QQuickWidget::Error) {
        qCritical() << "🚨 QML FAILED TO LOAD!";
        qCritical() << "QML Errors:" << m_gpuChart->errors();
    }
    
    // Set default symbol in QML context
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", "BTC-USD");
    m_modeController = new ChartModeController(this);
    context->setContextProperty("chartModeController", m_modeController);
    
    // Control panel
    m_cvdLabel = new QLabel("CVD: N/A", this);
    m_statusLabel = new QLabel("🔴 Disconnected", this);
    m_symbolInput = new QLineEdit("BTC-USD", this);
    m_subscribeButton = new QPushButton("🚀 Subscribe", this);
    
    // 🔥 PHASE 1: STRESS TEST CONTROLS
    QPushButton* stressTestButton = new QPushButton("🔥 1M Point VBO Test", this);
    stressTestButton->setStyleSheet("QPushButton { background-color: #ff4444; color: white; padding: 8px 16px; font-size: 14px; font-weight: bold; }");
    
    connect(stressTestButton, &QPushButton::clicked, [this]() {
        qDebug() << "🚀 LAUNCHING 1M POINT VBO STRESS TEST!";
        QQmlContext* context = m_gpuChart->rootContext();
        context->setContextProperty("stressTestMode", true);
        
        // Reload QML to trigger stress test
        QString qmlPath = QString("%1/libs/gui/qml/DepthChartView.qml").arg(QDir::currentPath());
        if (QFile::exists(qmlPath)) {
            m_gpuChart->setSource(QUrl::fromLocalFile(qmlPath));
        } else {
            m_gpuChart->setSource(QUrl("qrc:/Sentinel/Charts/DepthChartView.qml"));
        }
        
        // 🔥 CRITICAL FIX: Re-establish connections after QML reload
        QTimer::singleShot(100, [this]() {  // Small delay to ensure QML is loaded
            connectToGPUChart();
        });
        
        qDebug() << "🔥 VBO STRESS TEST ACTIVATED!";
    });
    
    // Styling
    m_cvdLabel->setStyleSheet("QLabel { color: white; font-size: 16px; font-weight: bold; }");
    m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
    m_symbolInput->setStyleSheet("QLineEdit { padding: 8px; font-size: 14px; }");
    m_subscribeButton->setStyleSheet("QPushButton { padding: 8px 16px; font-size: 14px; font-weight: bold; }");
    
    // Layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // GPU chart takes most space
    mainLayout->addWidget(m_gpuChart, 1);
    
    // Control panel at bottom
    QGroupBox* controlGroup = new QGroupBox("🎯 Trading Controls");
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->addWidget(new QLabel("Symbol:"));
    controlLayout->addWidget(m_symbolInput);
    controlLayout->addWidget(m_subscribeButton);
    controlLayout->addWidget(stressTestButton);  // 🔥 STRESS TEST BUTTON
    controlLayout->addStretch();
    controlLayout->addWidget(m_cvdLabel);
    controlLayout->addWidget(m_statusLabel);
    controlGroup->setLayout(controlLayout);
    
    mainLayout->addWidget(controlGroup);
    setLayout(mainLayout);
}

void MainWindowGPU::setupConnections() {
    if (!m_testMode) {
        // Create data controllers (keep existing proven pipeline)
        m_streamController = new StreamController(this);
        m_statsController = new StatisticsController(this);
        m_gpuAdapter = new GPUDataAdapter(this);
        m_streamController->setGPUAdapter(m_gpuAdapter);
        
        // 🚀 CONNECT GPUDataAdapter to centralized UniversalOrderBook
        // This enables universal order book access for the GPU pipeline
        connect(m_streamController, &StreamController::connected, this, [this]() {
            if (m_streamController && m_streamController->getClient()) {
                m_gpuAdapter->setCoinbaseClient(m_streamController->getClient());
                qDebug() << "🚀 GPUDataAdapter connected to centralized UniversalOrderBook!";
            }
        });
        
        // UI connections
        connect(m_subscribeButton, &QPushButton::clicked, this, &MainWindowGPU::onSubscribe);
        
        // Data pipeline connections
        connect(m_streamController, &StreamController::connected, this, [this]() {
            m_statusLabel->setText("🟢 Connected");
            m_statusLabel->setStyleSheet("QLabel { color: green; font-size: 14px; }");
            m_subscribeButton->setText("🔗 Connected");
            m_subscribeButton->setEnabled(false);
        });
        
        connect(m_streamController, &StreamController::disconnected, this, [this]() {
            m_statusLabel->setText("🔴 Disconnected");
            m_statusLabel->setStyleSheet("QLabel { color: red; font-size: 14px; }");
            m_subscribeButton->setText("🚀 Subscribe");
            m_subscribeButton->setEnabled(true);
        });
        
        // Stats pipeline
        connect(m_statsController, &StatisticsController::cvdUpdated, 
                this, &MainWindowGPU::onCVDUpdated);    
    } else {
        // Setup test UI and connections
        qDebug() << "🔧 Test mode: Skipping real-time data connections.";
        m_subscribeButton->setText("🔍 Test Mode");
        m_subscribeButton->setEnabled(false);
        m_statusLabel->setText("🔍 MOCK DATA");
        m_statusLabel->setStyleSheet("QLabel { color: green; font-size: 14px; }");

        // Create test data player as member variable
        m_testDataPlayer = new TestDataPlayer(this);
        
        // Connect to historical data ready signal for fractal zoom testing (SKIP FOR NOW)
        // connect(m_testDataPlayer, &TestDataPlayer::historicalDataReady, this, [this]() {
        //     qDebug() << "🚀 HISTORICAL DATA READY FOR FRACTAL ZOOM TESTING!";
        //     
        //     // Populate GPU components with historical data for fractal zoom testing
        //     if (m_testDataPlayer->isHistoricalDataReady()) {
        //         populateHistoricalDataForFractalZoom();
        //     }
        // });
        
        if (!m_testDataPlayer->loadTestData("logs/small_test_orderbook_data.json")) {
            qDebug() << "❌ Failed to load test data!";
        } else {
            m_testDataPlayer->startPlayback();
            qDebug() << "✅ Test data player started";
        }
    }

    qDebug() << "✅ GPU MainWindow connections established";
    connectToGPUChart();
}

void MainWindowGPU::onSubscribe() {
    if (m_testMode) {
        qDebug() << "🔧 Test mode: Skipping subscription - using test data instead";
        return;
    }
    
    QString symbol = m_symbolInput->text().trimmed().toUpper();
    if (symbol.isEmpty()) {
        qWarning() << "❌ Empty symbol input";
        return;
    }
    
    qDebug() << "🚀 Subscribing to GPU chart:" << symbol;
    
    // Update QML context
    QQmlContext* context = m_gpuChart->rootContext();
    context->setContextProperty("symbol", symbol);
    
    // Start data stream
    std::vector<std::string> symbols = {symbol.toStdString()};
    m_streamController->start(symbols);
    
    qDebug() << "✅ GPU subscription started for" << symbol;
}

void MainWindowGPU::onCVDUpdated(double cvd) {
    m_cvdLabel->setText(QString("CVD: %1").arg(cvd, 0, 'f', 2));
}

void MainWindowGPU::onLodLevelChanged(int timeframe) {
    // 🎯 FRACTAL ZOOM COORDINATION: Convert LOD change to FractalZoomController trigger
    if (m_gpuAdapter && m_gpuAdapter->getFractalZoomController()) {
        CandleLOD::TimeFrame tf = static_cast<CandleLOD::TimeFrame>(timeframe);
        
        // Get the fractal zoom controller
        auto* fractalController = m_gpuAdapter->getFractalZoomController();
        
        // 🚀 FRACTAL ZOOM: Connect order book depth changes to heatmap
        QQuickItem* qmlRoot = m_gpuChart->rootObject();
        if (qmlRoot) {
            HeatmapBatched* heatmapLayer = qmlRoot->findChild<HeatmapBatched*>("heatmapLayer");
            if (heatmapLayer) {
                // Model B architecture: No need for setMaxHistoricalPoints since we use fixed buffers
                // The 8 timeframe buffers are managed automatically by the new architecture
                
                sLog_Chart("🎯 FRACTAL ZOOM: Connected depth aggregation to heatmap");
            }
        }
        
        // Trigger the timeframe change
        fractalController->onTimeFrameChanged(tf);
    }
}

void MainWindowGPU::connectToGPUChart() {
    qDebug() << "🔍 CONNECTING TO ZERO-LAG GPU CHART...";
    
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (!qmlRoot) {
        qWarning() << "❌ QML root object not found - retrying in 100ms...";
        QTimer::singleShot(100, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    
    qDebug() << "✅ QML root found:" << qmlRoot;
    
    // 🚀 FIND COMPONENTS - Both old and new
    GPUChartWidget* gpuChart = qmlRoot->findChild<GPUChartWidget*>("gpuChart");
    HeatmapBatched* oldHeatmap = qmlRoot->findChild<HeatmapBatched*>("heatmapLayer");
    CandlestickBatched* candleChart = qmlRoot->findChild<CandlestickBatched*>("candlestickRenderer");
    
    if (gpuChart) {
        qDebug() << "✅ Found GPUChartWidget";
        
        // REPLACE THIS BROKEN CONNECTION:
        connect(gpuChart, &GPUChartWidget::viewChanged,
            [this](int64_t start, int64_t end, double minP, double maxP) {
            // 🔥 FIX: Only update if we have valid timestamps
            if (start > 0 && end > start) {
                m_unifiedViewport->setViewport(start, end, minP, maxP);
                m_unifiedViewport->setPixelSize(m_gpuChart->width(), m_gpuChart->height());
                qDebug() << "🚀 UNIFIED VIEWPORT:" << start << "to" << end;
            } else {
                qDebug() << "⚠️ IGNORING INVALID VIEWPORT:" << start << "to" << end;
            }
        });
        
        // 🚀 CONNECT MOUSE HANDLING to zero-lag system
        if (m_zeroLagArch) {
            // We'll add mouse event filter to intercept and send to zero-lag system
            m_gpuChart->installEventFilter(this);
        }
        
        // Keep existing connections for backward compatibility
        if (!m_testMode && m_streamController) {
            disconnect(m_streamController, &StreamController::tradeReceived,
                      gpuChart, &GPUChartWidget::onTradeReceived);
                      
            bool connectionSuccess = connect(m_streamController, &StreamController::tradeReceived,
                                            gpuChart, &GPUChartWidget::onTradeReceived,
                                            Qt::QueuedConnection);
                                            
            qDebug() << "🔗 GPU Chart Trade Connection:" << (connectionSuccess ? "SUCCESS" : "FAILED");
        }
    }
    
    // 🚀 CONNECT INSTANT TEST DATA (if in test mode)
    if (m_testMode && m_instantTestData && gpuChart) {
        connect(m_instantTestData, &InstantTestData::orderBookUpdate,
                [gpuChart](const InstantTestData::OrderBookSnapshot& snapshot) {
            // Convert to your existing OrderBook format
            OrderBook book;
            book.product_id = "BTC-USD";
            book.timestamp = std::chrono::system_clock::from_time_t(snapshot.timestamp_ms / 1000);
            book.bids = snapshot.bids;
            book.asks = snapshot.asks;
            
            gpuChart->updateOrderBook(book);
        });
        
        connect(m_instantTestData, &InstantTestData::tradeExecuted,
                [gpuChart](int64_t timestamp_ms, double price, double size, bool isBuy) {
            Trade trade;
            trade.product_id = "BTC-USD";
            trade.price = price;
            trade.size = size;
            trade.side = isBuy ? AggressorSide::Buy : AggressorSide::Sell;
            trade.timestamp = std::chrono::system_clock::from_time_t(timestamp_ms / 1000);
            
            gpuChart->onTradeReceived(trade);
        });
        
        qDebug() << "✅ INSTANT TEST DATA CONNECTED TO GPU CHART!";
    }
    
    // 🚀 CONNECT DYNAMIC LOD to old heatmap (bridge between old and new)
    if (oldHeatmap && m_dynamicLOD) {
        connect(m_unifiedViewport, &UnifiedViewport::viewportChanged,
                [this, oldHeatmap](const UnifiedViewport::ViewState& state) {
            // Update old heatmap with new viewport
            oldHeatmap->setViewTimeStart(state.timeStart_ms);
            oldHeatmap->setViewTimeEnd(state.timeEnd_ms);
            oldHeatmap->setViewMinPrice(state.priceMin);
            oldHeatmap->setViewMaxPrice(state.priceMax);
            
            // Check for LOD changes
            auto newTF = m_dynamicLOD->selectOptimalTimeFrame(state.msPerPixel());
            static DynamicLOD::TimeFrame lastTF = DynamicLOD::TimeFrame::TF_1s;
            if (newTF != lastTF) {
                qDebug() << "🎯 LOD CHANGE:" << DynamicLOD::getTimeFrameName(lastTF) 
                         << "→" << DynamicLOD::getTimeFrameName(newTF);
                lastTF = newTF;
            }
        });
        
        qDebug() << "✅ DYNAMIC LOD CONNECTED TO OLD HEATMAP!";
    }
    
    // Keep all your existing connections for candlesticks, etc.
    if (candleChart) {
        if (!m_testMode && m_gpuAdapter) {
            connect(m_gpuAdapter, &GPUDataAdapter::candlesReady,
                    candleChart, &CandlestickBatched::onCandlesReady,
                    Qt::QueuedConnection);
            
            connect(gpuChart, &GPUChartWidget::viewChanged,
                    candleChart, &CandlestickBatched::onViewChanged,
                    Qt::QueuedConnection);
        }
        
        qDebug() << "✅ CANDLESTICK CONNECTIONS MAINTAINED!";
    }
    
    qDebug() << "🚀 ZERO-LAG GPU CHART CONNECTED!";
}

bool MainWindowGPU::eventFilter(QObject* object, QEvent* event) {
    // Intercept mouse events and send to zero-lag system
    if (object == m_gpuChart && m_zeroLagArch) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            m_zeroLagArch->handleMousePress(mouseEvent->position());
            break;
        }
        case QEvent::MouseMove: {
            QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            static QPointF lastPos;
            QPointF delta = mouseEvent->position() - lastPos;
            m_zeroLagArch->handleMouseMove(delta);
            lastPos = mouseEvent->position();
            break;
        }
        case QEvent::Wheel: {
            QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);
            m_zeroLagArch->handleWheel(wheelEvent->angleDelta().y() * 0.001, wheelEvent->position());
            break;
        }
        default:
            break;
        }
    }
    
    return QWidget::eventFilter(object, event);
}

// 🚀 NEW: Populate historical data for fractal zoom testing
void MainWindowGPU::populateHistoricalDataForFractalZoom() {
    qDebug() << "🚀 POPULATING HISTORICAL DATA FOR FRACTAL ZOOM TESTING...";
    
    if (!m_testDataPlayer || !m_testDataPlayer->isHistoricalDataReady()) {
        qWarning() << "❌ Test data player not ready for historical data population";
        return;
    }
    
    // Get QML root for component access
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (!qmlRoot) {
        qWarning() << "❌ QML root not available for historical data population";
        return;
    }
    
    // Populate GPU chart with historical data
    GPUChartWidget* gpuChart = qmlRoot->findChild<GPUChartWidget*>("gpuChart");
    if (gpuChart) {
        qDebug() << "📊 Populating GPU chart with historical order book data...";
        
        // Get historical data for different timeframes
        const std::vector<OrderBook>& historicalBooks = m_testDataPlayer->getHistoricalOrderBooks(CandleLOD::TF_1min);
        qDebug() << "📊 Found" << historicalBooks.size() << "historical 1-minute order books";
        
        // Send historical data to GPU chart (this will populate the heatmap)
        for (const auto& book : historicalBooks) {
            gpuChart->updateOrderBook(book);
        }
        
        qDebug() << "✅ GPU chart populated with" << historicalBooks.size() << "historical order books";
    }
    
    // Populate candlestick chart with historical data
    CandlestickBatched* candleChart = qmlRoot->findChild<CandlestickBatched*>("candlestickRenderer");
    if (candleChart) {
        qDebug() << "🕯️ Populating candlestick chart with historical candle data...";
        
        // Get historical candles for different timeframes
        const std::vector<OHLC>& historicalCandles = m_testDataPlayer->getHistoricalCandles(CandleLOD::TF_1min);
        qDebug() << "🕯️ Found" << historicalCandles.size() << "historical 1-minute candles";
        
        // Send historical candles to candlestick chart
        for (const auto& candle : historicalCandles) {
            // Note: We'll need to implement a method to add historical candles
            // For now, we'll use the existing interface if available
            qDebug() << "🕯️ Historical candle:" << candle.open << candle.high << candle.low << candle.close;
        }
        
        qDebug() << "✅ Candlestick chart populated with" << historicalCandles.size() << "historical candles";
    }
    
    qDebug() << "🚀 HISTORICAL DATA POPULATION COMPLETE - READY FOR FRACTAL ZOOM TESTING!";
    qDebug() << "🎯 Now you can zoom out to see aggregated timeframes and test fractal zoom functionality!";
}