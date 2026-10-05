// Sentinel — StockChartDock
#include "StockChartDock.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantList>
#include <QVariantMap>
#include <QButtonGroup>
#include <QSurfaceFormat>
#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <cmath>

// ── Helpers ───────────────────────────────────────────────────────────────────

static QToolButton* makePeriodBtn(const QString& label, QWidget* parent) {
    auto* btn = new QToolButton(parent);
    btn->setText(label);
    btn->setCheckable(true);
    btn->setMinimumHeight(22);
    btn->setStyleSheet(
        "QToolButton { background:#1a2028; color:#6a8090; border:1px solid #253040;"
        " border-radius:3px; padding:0 6px; }"
        "QToolButton:checked { background:#1e3a50; color:#60c0e0; border-color:#2a6080; }"
        "QToolButton:hover { color:#c0d0dc; }");
    return btn;
}

// ── Construction ─────────────────────────────────────────────────────────────

StockChartDock::StockChartDock(QWidget* parent, ResearchProcess* candles, ResearchProcess* sec)
    : DockablePanel("StockChartDock", "Stock Chart", parent)
    , m_periodGroup(new QButtonGroup(this))
    , m_runner(candles ? candles : new ResearchProcess(this))
    , m_secApiClient(new SecApiClient(this, sec))
{
    connect(m_runner, &ResearchProcess::completed, this, &StockChartDock::acceptCandles);
    connect(m_secApiClient, &SecApiClient::insiderSignalsReady, this, &StockChartDock::onSecSignalsReady);
    connect(m_secApiClient, &SecApiClient::apiError, this, &StockChartDock::onSecApiError);

    buildUi();
}

StockChartDock::~StockChartDock() {
    m_secApiClient->disconnect(this);
    m_secApiClient->cancel();
    if (m_runner) m_runner->cancel();
}

// ── UI ────────────────────────────────────────────────────────────────────────

void StockChartDock::buildUi() {
    auto* layout = new QVBoxLayout(m_contentWidget);
    layout->setContentsMargins(4, 4, 4, 0);
    layout->setSpacing(4);

    // ── Toolbar ───────────────────────────────────────────────────────────────
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);

    m_tickerInput = new QLineEdit(m_contentWidget);
    m_tickerInput->setObjectName("stockTicker");
    m_tickerInput->setAccessibleName("Stock ticker");
    m_tickerInput->setPlaceholderText("Ticker…");
    m_tickerInput->setFixedWidth(80);
    m_tickerInput->setMaxLength(10);
    m_tickerInput->setStyleSheet(
        "QLineEdit { background:#141a20; color:#c0ccd8; border:1px solid #253040;"
        " border-radius:3px; padding:2px 6px; }");
    toolbar->addWidget(m_tickerInput);

    // Period buttons
    const QStringList periods = {"1y", "2y", "5y", "10y", "max"};
    for (const auto& p : periods) {
        auto* btn = makePeriodBtn(p, m_contentWidget);
        m_periodGroup->addButton(btn);
        toolbar->addWidget(btn);
        if (p == m_currentPeriod) btn->setChecked(true);
        connect(btn, &QToolButton::clicked, this, [this, p]{ onPeriodChanged(p); });
    }

    toolbar->addStretch();

    m_fetchBtn = new QToolButton(m_contentWidget);
    m_fetchBtn->setIcon(QIcon(":/svg/refresh.svg"));
    m_fetchBtn->setToolTip("Fetch daily candles");
    m_fetchBtn->setAccessibleName("Fetch daily candles");
    m_fetchBtn->setFixedSize(26, 26);
    toolbar->addWidget(m_fetchBtn);

    layout->addLayout(toolbar);

    // ── QML chart ─────────────────────────────────────────────────────────────
    m_quickView = new QQuickView;
    m_quickView->setPersistentSceneGraph(true);
    m_quickView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_quickView->setColor(Qt::black);
    m_quickView->setFormat(QSurfaceFormat::defaultFormat());
    m_quickView->engine()->addImportPath("qrc:/qt/qml");

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString modPath = QDir(appDir).absoluteFilePath("../../libs/gui");
    if (QFile::exists(QDir(modPath).filePath("qmldir")))
        m_quickView->engine()->addImportPath(modPath);

    // Try QRC first, fall back to source dir in dev builds
    if (QFile::exists(":/Sentinel/Charts/StockChartView.qml"))
        m_quickView->setSource(QUrl("qrc:/Sentinel/Charts/StockChartView.qml"));
    else if (QFile::exists(":/qt/qml/Sentinel/Charts/StockChartView.qml"))
        m_quickView->setSource(QUrl("qrc:/qt/qml/Sentinel/Charts/StockChartView.qml"));
    else {
#ifdef SENTINEL_SOURCE_DIR
        const QString local = QDir(QString::fromUtf8(SENTINEL_SOURCE_DIR)).filePath("libs/gui/qml/StockChartView.qml");
#else
        const QString local = QDir::current().filePath("libs/gui/qml/StockChartView.qml");
#endif
        m_quickView->setSource(QUrl::fromLocalFile(local));
    }

    if (auto* root = qmlRoot()) root->setProperty("uiFont", QApplication::font());

    m_qmlContainer = QWidget::createWindowContainer(m_quickView, m_contentWidget);
    m_qmlContainer->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(m_qmlContainer, 1);

    m_contentWidget->setLayout(layout);

    // Signals
    connect(m_fetchBtn,    &QToolButton::clicked, this, &StockChartDock::onFetchClicked);
    connect(m_tickerInput, &QLineEdit::returnPressed, this, &StockChartDock::onFetchClicked);
    connect(m_tickerInput, &QLineEdit::textEdited, this, [this] {
        clearData();
        m_currentTicker = m_tickerInput->text().trimmed().toUpper();
        m_currentCompany.clear();
        if (auto* root = qmlRoot()) {
            root->setProperty("ticker", m_currentTicker);
            root->setProperty("company", "");
        }
        setStatus("Press Enter to fetch daily candles");
    });
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool v) {
        if (m_qmlContainer) m_qmlContainer->setVisible(v);
        if (m_quickView)    m_quickView->setVisible(v);
    });
}

// ── Public API ────────────────────────────────────────────────────────────────

void StockChartDock::loadSymbol(const QString& ticker, const QString& companyName) {
    m_currentTicker  = ticker.toUpper().trimmed();
    m_currentCompany = companyName;
    m_tickerInput->setText(m_currentTicker);

    if (auto* root = qmlRoot()) {
        root->setProperty("ticker",  m_currentTicker);
        root->setProperty("company", m_currentCompany);
        QMetaObject::invokeMethod(root, "clearSecSignals");
    }

    startFetch();
    startSecFetch();
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void StockChartDock::onFetchClicked() {
    const QString t = m_tickerInput->text().trimmed().toUpper();
    if (t.isEmpty()) return;
    loadSymbol(t);
}

void StockChartDock::onPeriodChanged(const QString& period) {
    m_currentPeriod = period;
    if (auto* root = qmlRoot())
        root->setProperty("period", period);
    if (!m_currentTicker.isEmpty()) {
        startFetch();
        startSecFetch();
    }
}

void StockChartDock::acceptCandles(quint64 request, const QByteArray& output, const QString& error) {
    if (request != m_request || !m_pending) return;
    m_pending = false;
    auto* root = qmlRoot();
    if (!root) return;
    root->setProperty("loading", false);
    QByteArray payload, providerError;
    for (const auto& line : output.split('\n')) {
        if (line.startsWith("OHLCV_DATA:")) payload = line.mid(11).trimmed();
        if (line.startsWith("ERROR_DATA:")) providerError = line.mid(11).trimmed();
    }
    if (!providerError.isEmpty()) {
        setStatus(QJsonDocument::fromJson(providerError).object().value("error").toString("yfinance returned an error"), true);
        return;
    }
    if (!error.isEmpty()) { setStatus(error, true); return; }
    const auto document = QJsonDocument::fromJson(payload);
    const auto obj = document.object();
    if (!document.isObject() || obj["ticker"].toString() != m_currentTicker || obj["period"].toString() != m_currentPeriod) {
        setStatus("yfinance response did not match the requested ticker and period", true);
        return;
    }
    QVariantList list;
    QString asOf;
    for (const auto& value : obj["candles"].toArray()) {
        const auto c = value.toObject();
        bool valid = true;
        for (const auto* key : {"ts_ms", "open", "high", "low", "close", "volume"})
            valid = valid && c[key].isDouble() && std::isfinite(c[key].toDouble());
        if (!valid || c["ts_ms"].toDouble() <= 0 || c["low"].toDouble() <= 0
            || c["high"].toDouble() < c["low"].toDouble() || c["volume"].toDouble() < 0
            || c["open"].toDouble() < c["low"].toDouble() || c["open"].toDouble() > c["high"].toDouble()
            || c["close"].toDouble() < c["low"].toDouble() || c["close"].toDouble() > c["high"].toDouble()) {
            setStatus("yfinance returned incomplete or invalid candles", true);
            return;
        }
        QVariantMap candle = c.toVariantMap();
        candle["timestamp"] = static_cast<qint64>(c["ts_ms"].toDouble());
        list.append(candle);
        if (c["date"].toString() > asOf) asOf = c["date"].toString();
    }
    if (list.isEmpty()) { setStatus("No daily candles returned for " + m_currentTicker, true); return; }
    root->setProperty("dataSymbol", m_currentTicker);
    root->setProperty("asOf", asOf.isEmpty() ? "Unknown" : asOf);
    root->setProperty("retrievedAt", QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss 'UTC'"));
    QMetaObject::invokeMethod(root, "setCandles", Q_ARG(QVariant, QVariant::fromValue(list)));
    setStatus("");
}

void StockChartDock::onSecSignalsReady(const QJsonObject& payload) {
    const QString payloadSymbol = payload.value("symbol").toString().trimmed().toUpper();
    if (!payloadSymbol.isEmpty() && payloadSymbol != m_currentTicker) {
        return;
    }
    if (auto* root = qmlRoot()) {
        root->setProperty("secSignalsLoading", false);
        root->setProperty("secStatus", "SEC EDGAR · retrieved " + m_secApiClient->retrievedAt().toString("yyyy-MM-dd HH:mm:ss 'UTC'"));
        QMetaObject::invokeMethod(root, "setSecSignals", Q_ARG(QVariant, QVariant::fromValue(payload.toVariantMap())));
    }
}

void StockChartDock::onSecApiError(const QString& error) {
    if (auto* root = qmlRoot()) {
        root->setProperty("secSignalsLoading", false);
        QMetaObject::invokeMethod(root, "clearSecSignals");
    }
    if (auto* root = qmlRoot()) root->setProperty("secStatus", "SEC: " + error);
}

// ── Private helpers ───────────────────────────────────────────────────────────

void StockChartDock::clearData() {
    ++m_request;
    m_pending = false;
    if (m_runner) m_runner->cancel();
    m_secApiClient->cancel();
    if (auto* root = qmlRoot()) {
        QMetaObject::invokeMethod(root, "clearChart");
        root->setProperty("loading", false);
        root->setProperty("secSignalsLoading", false);
        root->setProperty("secStatus", "SEC insiders idle");
        root->setProperty("dataSymbol", "");
        root->setProperty("asOf", "Unknown");
        root->setProperty("retrievedAt", "Unknown");
    }
}
void StockChartDock::startFetch() {
    clearData();
    if (m_currentTicker.isEmpty()) { setStatus("Enter a ticker to fetch daily candles"); return; }
    if (!ResearchProcess::isEquityTicker(m_currentTicker)) {
        setStatus("Stock charts require an equity ticker; crypto pairs are unsupported", true);
        return;
    }
    if (!m_runner) { setStatus("yfinance request provider unavailable", true); return; }
    if (auto* root = qmlRoot()) root->setProperty("loading", true);
    setStatus(QString("Loading %1 · %2 daily candles…").arg(m_currentTicker, m_currentPeriod));
    m_pending = true;
    m_runner->run(m_request, QDir(ResearchProcess::scriptsPath()).filePath("stocks/fetch_daily_ohlcv.py"), {m_currentTicker, m_currentPeriod});
}
void StockChartDock::changeEvent(QEvent* event) {
    DockablePanel::changeEvent(event);
    if (event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::FontChange)
        if (auto* root = qmlRoot()) root->setProperty("uiFont", QApplication::font());
}

void StockChartDock::startSecFetch() {
    if (m_currentTicker.isEmpty() || !m_secApiClient) {
        return;
    }
    if (auto* root = qmlRoot()) {
        QMetaObject::invokeMethod(root, "clearSecSignals");
        root->setProperty("secSignalsLoading", true);
    }
    m_secApiClient->fetchInsiderSignals(m_currentTicker, secDaysBackForCurrentPeriod());
}

int StockChartDock::secDaysBackForCurrentPeriod() const {
    if (m_currentPeriod == "1y") return 365;
    if (m_currentPeriod == "2y") return 365 * 2;
    if (m_currentPeriod == "5y") return 365 * 5;
    if (m_currentPeriod == "10y") return 365 * 10;
    return 365 * 15;
}

QObject* StockChartDock::qmlRoot() const {
    return m_quickView ? m_quickView->rootObject() : nullptr;
}

void StockChartDock::setStatus(const QString& msg, bool error) {
    if (auto* root = qmlRoot())
        root->setProperty("statusMsg", error ? "Unavailable: " + msg : msg);
}
