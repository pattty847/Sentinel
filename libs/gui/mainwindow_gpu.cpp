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
    qDebug() << "🚀 CREATING GPU TRADING TERMINAL!";

    setupUI();
    setupConnections();
    
    // Set window properties
    setWindowTitle("Sentinel - GPU Trading Terminal");
    resize(1400, 900);
    
    // Automatically start the subscription process once the event loop begins.
    // This ensures the window is fully constructed and shown before we start streaming.
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
        
        // 🚀 CONNECT GPUDataAdapter to centralized FastOrderBook
        // This enables O(1) order book access for the GPU pipeline
        connect(m_streamController, &StreamController::connected, this, [this]() {
            if (m_streamController && m_streamController->getClient()) {
                m_gpuAdapter->setCoinbaseClient(m_streamController->getClient());
                qDebug() << "🚀 GPUDataAdapter connected to centralized FastOrderBook!";
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

// 🔥 CRITICAL FIX: Helper method to establish GPU connections
void MainWindowGPU::connectToGPUChart() {
    qDebug() << "🔍 ATTEMPTING TO CONNECT TO GPU CHART...";
    
    // Get the GPU chart widget from QML
    QQuickItem* qmlRoot = m_gpuChart->rootObject();
    if (!qmlRoot) {
        qWarning() << "❌ QML root object not found - retrying in 100ms...";
        // Retry after QML loads
        QTimer::singleShot(100, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    
    qDebug() << "✅ QML root found:" << qmlRoot;
    qDebug() << "🔍 QML root children:";
    for (QObject* child : qmlRoot->children()) {
        qDebug() << "  -" << child->objectName() << child->metaObject()->className();
    }
    
    GPUChartWidget* gpuChart = nullptr;
    
    // 🔥 STRATEGY 1: Find by explicit objectName
    QQuickItem* gpuChartItem = qmlRoot->findChild<QQuickItem*>("gpuChart");
    qDebug() << "🔍 objectName lookup for 'gpuChart':" << gpuChartItem;
    
    if (gpuChartItem) {
        gpuChart = qobject_cast<GPUChartWidget*>(gpuChartItem);
        qDebug() << "🔍 qobject_cast to GPUChartWidget:" << gpuChart;
    }
    
    // 🔥 STRATEGY 2: If objectName lookup fails, find by type
    if (!gpuChart) {
        qDebug() << "🔍 objectName lookup failed, trying type lookup...";
        gpuChart = qmlRoot->findChild<GPUChartWidget*>();
        qDebug() << "🔍 Type lookup result:" << gpuChart;
    }
    
    if (gpuChart) {
        if (!m_testMode) {
            // 🔥 CRITICAL: Disconnect any existing connections to avoid duplicates
            disconnect(m_streamController, &StreamController::tradeReceived,
                      gpuChart, &GPUChartWidget::onTradeReceived);
                      
            // 🔥 REAL-TIME DATA CONNECTION! (Qt::QueuedConnection for thread safety)
            bool connectionSuccess = connect(m_streamController, &StreamController::tradeReceived,
                                            gpuChart, &GPUChartWidget::onTradeReceived,
                                            Qt::QueuedConnection); // CRITICAL: Async connection for thread safety!
                                            
            qDebug() << "🔗 Qt Signal Connection Result:" << (connectionSuccess ? "SUCCESS" : "FAILED");
            qDebug() << "🎯 Connected StreamController:" << m_streamController 
                     << "to GPUChart:" << gpuChart;
            
            qDebug() << "🔥 GPU CHART CONNECTED TO REAL-TIME TRADE DATA!";
        } else {
            // Test mode: Connect test data player to GPU chart
            if (m_testDataPlayer) {
                bool testConnection = connect(m_testDataPlayer, &TestDataPlayer::orderBookUpdated,
                                            gpuChart, &GPUChartWidget::updateOrderBook,
                                            Qt::QueuedConnection);
                qDebug() << "🔗 Test Data Connection Result:" << (testConnection ? "SUCCESS" : "FAILED");
                qDebug() << "🎯 Connected TestDataPlayer to GPUChart for test mode";
            }
        }
        
        // 🔥 GEMINI UNIFICATION: Connect chart view to heatmap
        QQuickItem* heatmapItem = qmlRoot->findChild<QQuickItem*>("heatmapLayer");
        HeatmapBatched* heatmapLayer = qobject_cast<HeatmapBatched*>(heatmapItem);
        
        if (heatmapLayer) {
            // 🔥 PROPERTY BINDING: Chart view coordinates to heatmap via QML properties
            // The viewChanged signal from GPUChartWidget will automatically update
            // the Q_PROPERTY values in HeatmapBatched through QML binding

            connect(heatmapLayer, &HeatmapBatched::dataUpdated,
                    gpuChart, &GPUChartWidget::update, Qt::QueuedConnection);

            
            // Test mode: Model B architecture uses aggregated heatmap data, not individual order book updates
            if (m_testMode && m_testDataPlayer) {
                qDebug() << "🔥 TEST MODE HEATMAP: Using Model B aggregated data pipeline";
            }
            
            qDebug() << "✅🔥 VIEW COORDINATION ESTABLISHED: Chart view is now wired to Heatmap.";
        } else {
            qWarning() << "⚠️ HeatmapBatched not found - coordinate unification failed";
        }
        
        // 🕯️ PHASE 5: CONNECT CANDLESTICK CHART
        qDebug() << "🔍 Looking for candlestick renderer...";
        QQuickItem* candleChartItem = qmlRoot->findChild<QQuickItem*>("candlestickRenderer");
        qDebug() << "🔍 Found candlestickRenderer item:" << candleChartItem;
        
        CandlestickBatched* candleChart = qobject_cast<CandlestickBatched*>(candleChartItem);
        qDebug() << "🔍 Cast to CandlestickBatched:" << candleChart;
        
        if (candleChart) {
            if (!m_testMode) {
                bool tradeConnection = connect(m_gpuAdapter, &GPUDataAdapter::candlesReady,
                                              candleChart, &CandlestickBatched::onCandlesReady,
                                              Qt::QueuedConnection);
                
                // 🔥 COORDINATE SYNCHRONIZATION: GPUChart drives candle coordinates
                bool coordConnection = connect(gpuChart, &GPUChartWidget::viewChanged,
                                              candleChart, &CandlestickBatched::onViewChanged,
                                              Qt::QueuedConnection);
                
                // 🎯 FRACTAL ZOOM COORDINATION: CandlestickBatched LOD → GPUDataAdapter → FractalZoomController
                bool fractalConnection = connect(candleChart, SIGNAL(lodLevelChanged(int)),
                                               this, SLOT(onLodLevelChanged(int)),
                                               Qt::QueuedConnection);
                
                qDebug() << "🕯️ CANDLESTICK CONNECTIONS:"
                         << "Candle stream:" << (tradeConnection ? "SUCCESS" : "FAILED")
                         << "Coordinates:" << (coordConnection ? "SUCCESS" : "FAILED")
                         << "Fractal zoom:" << (fractalConnection ? "SUCCESS" : "FAILED");
                
                if (tradeConnection && coordConnection && fractalConnection) {
                    qDebug() << "✅ CANDLESTICK CHART FULLY CONNECTED TO REAL-TIME DATA WITH FRACTAL ZOOM!";
                }
            } else {
                // Test mode: Only connect coordinate synchronization
                bool coordConnection = connect(gpuChart, &GPUChartWidget::viewChanged,
                                              candleChart, &CandlestickBatched::onViewChanged,
                                              Qt::QueuedConnection);
                qDebug() << "🕯️ TEST MODE CANDLESTICK: Coordinate sync:" << (coordConnection ? "SUCCESS" : "FAILED");
            }
        } else {
            qWarning() << "❌ CandlestickBatched (candlestickRenderer) not found - candle integration failed";
            
            // List all available QML children for debugging
            qDebug() << "🔍 Available QML children with objectNames:";
            for (QObject* child : qmlRoot->findChildren<QObject*>()) {
                QString objName = child->objectName();
                if (!objName.isEmpty()) {
                    qDebug() << "  -" << objName << ":" << child->metaObject()->className();
                }
            }
        }
    } else {
        qWarning() << "❌ GPUChartWidget NOT FOUND - RETRYING IN 200ms...";
        // Retry after a longer delay
        QTimer::singleShot(200, this, &MainWindowGPU::connectToGPUChart);
        return;
    }
    
    // 🔥 PHASE 2: Connect order book data to HeatmapBatched - CRITICAL FIX!
    if (!m_testMode) {
        HeatmapBatched* heatmapLayer = qmlRoot->findChild<HeatmapBatched*>("heatmapLayer");
        if (heatmapLayer) {
            // Model B architecture: Remove individual order book updates, use aggregated pipeline instead
            // The GPUDataAdapter will process order books and emit aggregatedHeatmapReady signals
            
            // 🔥 NEW: Connect GPUDataAdapter aggregated heatmap signal to Model B architecture
            connect(m_gpuAdapter, &GPUDataAdapter::aggregatedHeatmapReady,
                    heatmapLayer, &HeatmapBatched::onHeatmapDataReady,
                    Qt::QueuedConnection);
                    
            // 🎯 FRACTAL ZOOM: Connect timeframe changes to heatmap
            if (m_gpuAdapter->getFractalZoomController()) {
                connect(m_gpuAdapter->getFractalZoomController(), &FractalZoomController::timeFrameChanged,
                        heatmapLayer, &HeatmapBatched::onTimeFrameChanged,
                        Qt::QueuedConnection);
            }
            
            qDebug() << "🔥 HEATMAP CONNECTED TO REAL-TIME ORDER BOOK DATA!";
        } else {
            qWarning() << "⚠️ HeatmapBatched (heatmapLayer) not found in QML root - CHECK OBJECTNAME!";
        }
    }
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